/**
 * tiny-reader — minimal e-paper EPUB reader for the LilyGo T5 4.7" V2.3 (Touch).
 *
 *  - reads .epub files from SD card root
 *  - paginates + justifies XHTML body text on the e-paper
 *  - GT911 touch nav (left/right halves = prev/next page)
 *  - GPIO21 short-press = back to library / cycle selection
 *  - GPIO21 long-press (≥ 2 s) = WiFi share mode (softAP + web upload UI)
 *  - per-book reading position persisted as <name>.pos on SD
 *
 * Board: T5-ePaper-S3 (V2.3 / H716 Touch)
 * Toolchain: PlatformIO, framework=arduino, see platformio.ini.
 */

#ifndef BOARD_HAS_PSRAM
#error "Enable PSRAM (OPI)"
#endif

#include <Arduino.h>
#include <FS.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <vector>
#include <string>

#include "epd_driver.h"
#include "firasans.h"
#include "firasans_small.h"   // smaller bitmap font for footer/status lines
#include "utilities.h"
#include <TouchDrvGT911.hpp>
#include "Epub.h"

// Phase E: WiFi AP + on-board web server (async — sync WebServer.h dies after
// a couple of back-to-back requests; AsyncWebServer handles concurrency).
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Preferences.h>

// ---- globals ----
static uint8_t *framebuffer = nullptr;
static TouchDrvGT911 touch;
static bool touchOnline = false;

enum AppMode { MODE_LIBRARY, MODE_BOOK };
static AppMode app_mode = MODE_LIBRARY;

// library mode
static std::vector<std::string> books;   // filenames in SD root ending .epub
static int selected = 0;                 // index in books
static int page_first = 0;               // first index visible on the screen
static const int LINES_PER_PAGE = 8;     // ~8 books fit comfortably at 36px font
static const int LINE_HEIGHT = 56;
static const int LIST_X = 60;
static const int LIST_Y = 80;

// book mode
static std::string current_book_path;
static int current_spine = 0;
static int total_spine = 0;
static int current_page_in_chapter = 0;
static std::vector<std::string> chapter_pages;  // each entry = one page of \n-separated lines
static const int BOOK_MARGIN_X = 50;
static const int BOOK_TOP_RESERVE = 30;     // small visual margin only
static const int BOOK_FOOTER_RESERVE = 50;
// Layout values that depend on the user's "density" setting (see g_settings).
// FiraSans advance_y is 50 — line_height < 50 would overlap glyphs.
static int book_line_height = 50;
static int book_lines_per_page = 9;
static int book_chars_per_line = 44;

// User-editable settings persisted to /settings.json on SD.
static struct Settings {
    int density;           // 0=compact, 1=medium (default), 2=loose
    int ap_idle_minutes;   // default 5
} g_settings = { 1, 5 };

// STR_100 button on GPIO 0 shares the EPD CFG_STR strobe line. We can't hold
// pinMode(0, INPUT_PULLUP) without breaking display refresh, so instead we
// briefly flip the pin when the EPD is electrically idle (right after
// epd_poweroff()), sample it, then restore CFG_STR to OUTPUT/HIGH (the idle
// state push_cfg() leaves it in). Latched here, consumed in loop(). See
// backlog #15.
static volatile bool str100_pressed = false;

// Caller MUST guarantee the EPD is idle (e.g. just after epd_poweroff()).
// Returns true if STR_100 is held down at the moment of sampling.
static bool sample_str100_button() {
    pinMode(0, INPUT_PULLUP);
    delayMicroseconds(50);
    bool down = (digitalRead(0) == LOW);
    delayMicroseconds(50);
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    return down;
}

static void apply_density() {
    switch (g_settings.density) {
        case 0:  // compact — same line height, slightly more chars per line.
                 // 48 chars stays inside 860px body width on FiraSans (52
                 // overflowed on long lines).
            book_line_height = 50;
            book_chars_per_line = 48;
            break;
        case 2:  // loose — extra leading between lines, fewer chars per line
            book_line_height = 60;
            book_chars_per_line = 36;
            break;
        case 1:  // medium (default)
        default:
            g_settings.density = 1;
            book_line_height = 50;
            book_chars_per_line = 44;
            break;
    }
    book_lines_per_page =
        (EPD_HEIGHT - BOOK_TOP_RESERVE - BOOK_FOOTER_RESERVE) / book_line_height;
    Serial.printf("[SETTINGS] density=%d -> line_h=%d lines/page=%d chars/line=%d\n",
                  g_settings.density, book_line_height,
                  book_lines_per_page, book_chars_per_line);
}

// ---- helpers ----

// LilyGo V2.3: BATT_PIN = GPIO14, on a /2 voltage divider.
static float read_battery_voltage() {
    int raw = analogRead(BATT_PIN);
    return (raw / 4095.0f) * 2.0f * 3.3f;
}

// Linear LiPo approximation: 100% @ 4.20V, 0% @ 3.30V.
// Charger holds the cell at ~4.20V on USB, so it'll read ~100% while plugged in.
static int read_battery_percent() {
    float v = read_battery_voltage();
    if (v >= 4.20f) return 100;
    if (v <= 3.30f) return 0;
    return (int)((v - 3.30f) / 0.90f * 100.0f + 0.5f);
}

// Load g_settings from /settings.json on SD; falls back to defaults if missing.
static void load_settings() {
    File f = SD.open("/settings.json", FILE_READ);
    if (!f) {
        Serial.println("[SETTINGS] no /settings.json — using defaults");
        return;
    }
    String s = f.readString();
    f.close();
    int d_i = s.indexOf("\"density\"");
    int t_i = s.indexOf("\"ap_idle_minutes\"");
    if (d_i >= 0) {
        int colon = s.indexOf(':', d_i);
        if (colon >= 0) g_settings.density = s.substring(colon + 1).toInt();
    }
    if (t_i >= 0) {
        int colon = s.indexOf(':', t_i);
        if (colon >= 0) g_settings.ap_idle_minutes = s.substring(colon + 1).toInt();
    }
    if (g_settings.density < 0 || g_settings.density > 2) g_settings.density = 1;
    if (g_settings.ap_idle_minutes < 1) g_settings.ap_idle_minutes = 5;
    Serial.printf("[SETTINGS] loaded: density=%d ap_idle_minutes=%d\n",
                  g_settings.density, g_settings.ap_idle_minutes);
}

static bool save_settings() {
    File f = SD.open("/settings.json", FILE_WRITE);
    if (!f) { Serial.println("[SETTINGS] save failed"); return false; }
    f.printf("{\"density\":%d,\"ap_idle_minutes\":%d}\n",
             g_settings.density, g_settings.ap_idle_minutes);
    f.close();
    Serial.printf("[SETTINGS] saved: density=%d ap_idle_minutes=%d\n",
                  g_settings.density, g_settings.ap_idle_minutes);
    return true;
}

static void clear_and_flush() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    epd_poweron();
    epd_clear();
    epd_poweroff();
}

static void scan_sd_for_epubs() {
    books.clear();
    File root = SD.open("/");
    if (!root) {
        Serial.println("Could not open SD root");
        return;
    }
    while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = f.name();
            String lower = name; lower.toLowerCase();
            if (lower.endsWith(".epub")) {
                books.push_back(std::string(name.c_str()));
            }
        }
        f.close();
    }
    root.close();
    Serial.printf("Found %u .epub files on SD\n", (unsigned)books.size());
}

// Decode the most common HTML entities into UTF-8 in place.
static std::string decode_entities(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                const char *rep = nullptr;
                if (ent == "amp") rep = "&";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "quot") rep = "\"";
                else if (ent == "apos") rep = "'";
                else if (ent == "nbsp") rep = " ";
                else if (ent == "mdash") rep = "—";
                else if (ent == "ndash") rep = "–";
                else if (ent == "hellip") rep = "…";
                else if (ent == "lsquo" || ent == "rsquo") rep = "'";
                else if (ent == "ldquo" || ent == "rdquo") rep = "\"";
                if (rep) { out += rep; i = semi + 1; continue; }
                // numeric &#NN; or &#xHH; — best-effort: drop
                if (!ent.empty() && ent[0] == '#') { i = semi + 1; continue; }
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Strip XHTML tags into plain text. Block-level closing tags become \n.
// Skip everything inside <head>, <script>, <style>.
static std::string strip_xhtml(const char *src, size_t len) {
    std::string out;
    out.reserve(len);
    bool in_tag = false;
    int skip_depth = 0;     // >0 while inside head/script/style
    std::string tag;
    auto is_block = [](const std::string &t){
        return t == "p" || t == "br" || t == "div" || t == "li" || t == "blockquote" ||
               t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6" ||
               t == "tr" || t == "pre";
    };
    auto is_skip = [](const std::string &t){
        return t == "head" || t == "script" || t == "style";
    };
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        if (c == '<') { in_tag = true; tag.clear(); continue; }
        if (c == '>') {
            in_tag = false;
            std::string name = tag;
            bool is_close = !name.empty() && name[0] == '/';
            if (is_close) name.erase(0, 1);
            size_t sp = name.find_first_of(" \t/");
            if (sp != std::string::npos) name = name.substr(0, sp);
            for (auto &ch : name) ch = (char)tolower((unsigned char)ch);
            // self-closing detection: '<head/>' has '/' at end of original tag
            bool self_close = !tag.empty() && tag.back() == '/';
            if (is_skip(name)) {
                if (is_close || self_close) { if (skip_depth > 0) --skip_depth; }
                else ++skip_depth;
            } else if (skip_depth == 0 && is_block(name)) {
                if (out.empty() || out.back() != '\n') out.push_back('\n');
            }
            continue;
        }
        if (in_tag) { tag.push_back(c); continue; }
        if (skip_depth > 0) continue;
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')) continue;
        out.push_back(c);
    }
    // collapse 3+ newlines to 2
    std::string out2;
    out2.reserve(out.size());
    int run = 0;
    for (char c : out) {
        if (c == '\n') { if (++run <= 2) out2.push_back(c); }
        else { run = 0; out2.push_back(c); }
    }
    return decode_entities(out2);
}

// Greedy word-wrap to ~book_chars_per_line per line; preserve paragraph breaks.
static std::vector<std::string> wrap_text(const std::string &text) {
    std::vector<std::string> lines;
    std::string para;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            // wrap one paragraph
            std::string current;
            std::string word;
            for (size_t j = 0; j <= para.size(); ++j) {
                char c = (j < para.size()) ? para[j] : ' ';
                if (c == ' ') {
                    if (!word.empty()) {
                        if (current.empty()) {
                            current = word;
                        } else if ((int)(current.size() + 1 + word.size()) <= book_chars_per_line) {
                            current += ' ';
                            current += word;
                        } else {
                            lines.push_back(current);
                            current = word;
                        }
                        word.clear();
                    }
                } else {
                    word.push_back(c);
                }
            }
            if (!current.empty()) lines.push_back(current);
            // blank line between paragraphs
            if (i < text.size()) lines.push_back("");
            para.clear();
        } else {
            para.push_back(text[i]);
        }
    }
    return lines;
}

// Group lines into pages.
static std::vector<std::string> paginate_lines(const std::vector<std::string> &lines) {
    std::vector<std::string> pages;
    std::string page;
    int n_in_page = 0;
    for (const auto &line : lines) {
        if (n_in_page >= book_lines_per_page) {
            pages.push_back(page);
            page.clear();
            n_in_page = 0;
        }
        if (!page.empty()) page.push_back('\n');
        page += line;
        ++n_in_page;
    }
    if (!page.empty()) pages.push_back(page);
    return pages;
}

// Result struct + task that loads epub, extracts a specific spine item, paginates.
struct ChapterLoadResult {
    volatile bool done;
    bool ok;
    int spine_n;
    std::string title;
    std::string path;
    int spine_to_load;
    std::vector<std::string> pages;
};

static void chapter_load_task(void *param) {
    ChapterLoadResult *r = (ChapterLoadResult *)param;
    Epub epub(r->path);
    r->ok = epub.load();
    if (r->ok) {
        r->spine_n = epub.get_spine_items_count();
        r->title = epub.get_title();
        if (r->spine_to_load >= 0 && r->spine_to_load < r->spine_n) {
            std::string href = epub.get_spine_item(r->spine_to_load);
            size_t size = 0;
            uint8_t *data = epub.get_item_contents(href, &size);
            if (data && size > 0) {
                std::string text = strip_xhtml((const char *)data, size);
                free(data);
                auto lines = wrap_text(text);
                r->pages = paginate_lines(lines);
                Serial.printf("chapter %d: %u pages, %u lines, %u bytes\n",
                              r->spine_to_load, (unsigned)r->pages.size(),
                              (unsigned)lines.size(), (unsigned)text.size());
            } else {
                Serial.printf("chapter %d: get_item_contents failed (size=%u)\n",
                              r->spine_to_load, (unsigned)size);
            }
        }
    }
    r->done = true;
    vTaskDelete(NULL);
}

// SD path "/<book-without-extension>.pos" for any book by filename.
static String pos_path_for_name(const String &name) {
    int dot = name.lastIndexOf('.');
    if (dot <= 0) dot = name.length();
    return String("/") + name.substring(0, dot) + ".pos";
}

static bool read_position_for_name(const String &name, int *out_spine, int *out_page) {
    File f = SD.open(pos_path_for_name(name), FILE_READ);
    if (!f) return false;
    String line = f.readStringUntil('\n');
    f.close();
    int sp = -1, pg = -1;
    if (sscanf(line.c_str(), "%d %d", &sp, &pg) == 2) {
        *out_spine = sp;
        *out_page = pg;
        return true;
    }
    return false;
}

static bool write_position_for_name(const String &name, int spine, int page) {
    String path = pos_path_for_name(name);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("write_position: open %s failed\n", path.c_str()); return false; }
    f.printf("%d %d\n", spine, page);
    f.close();
    Serial.printf("[POS_SAVED] %s -> ch=%d p=%d\n", path.c_str(), spine, page);
    return true;
}

static void save_position() {
    if (selected < 0 || selected >= (int)books.size()) return;
    write_position_for_name(String(books[selected].c_str()),
                            current_spine, current_page_in_chapter);
}

static bool load_position(int *out_spine, int *out_page) {
    if (selected < 0 || selected >= (int)books.size()) return false;
    bool ok = read_position_for_name(String(books[selected].c_str()),
                                     out_spine, out_page);
    if (ok) Serial.printf("[POS_LOADED] ch=%d p=%d\n", *out_spine, *out_page);
    return ok;
}

// Spawn the load task and wait for it.
static bool load_chapter(int spine_index) {
    ChapterLoadResult r = {};
    r.path = current_book_path;
    r.spine_to_load = spine_index;
    xTaskCreatePinnedToCore(chapter_load_task, "chapter_load", 32768,
                            &r, 5, NULL, 1);
    while (!r.done) vTaskDelay(pdMS_TO_TICKS(50));
    if (!r.ok) return false;
    chapter_pages = r.pages;
    total_spine = r.spine_n;
    current_spine = spine_index;
    current_page_in_chapter = 0;
    // empty chapter (e.g. cover image only) — synthesize a placeholder page so
    // navigation still works.
    if (chapter_pages.empty()) {
        chapter_pages.push_back("(this chapter has no text — tap right to continue)");
    }
    return true;
}

static void dump_current_page_to_serial() {
    Serial.printf("[PAGE_BEGIN ch=%d/%d p=%d/%d]\n",
                  current_spine + 1, total_spine,
                  current_page_in_chapter + 1, (int)chapter_pages.size());
    if (!chapter_pages.empty()) {
        Serial.println(chapter_pages[current_page_in_chapter].c_str());
    }
    Serial.println("[PAGE_END]");
    Serial.flush();
}

// Render a single line, optionally distributing slack between words so that
// the rendered width matches `target_w`.
static void render_line(const GFXfont *font, const char *line,
                        int x_start, int target_w, int32_t y,
                        uint8_t *fb, bool justify) {
    // tokenize words
    std::vector<std::string> words;
    {
        std::string cur;
        for (const char *p = line; *p; ++p) {
            if (*p == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur.push_back(*p);
        }
        if (!cur.empty()) words.push_back(cur);
    }
    if (words.empty()) return;

    int32_t cx = x_start, cy = y;

    if (!justify || words.size() == 1) {
        writeln((GFXfont *)font, line, &cx, &cy, fb);
        return;
    }

    // measure each word
    std::vector<int> ww;
    int total_word_w = 0;
    for (auto &w : words) {
        int32_t mx = 0, my = 0, mx1, my1, mw, mh;
        get_text_bounds(font, w.c_str(), &mx, &my, &mx1, &my1, &mw, &mh, NULL);
        ww.push_back(mw);
        total_word_w += mw;
    }
    int gaps = (int)words.size() - 1;
    // baseline space width
    int32_t mx = 0, my = 0, mx1, my1, sp_w, mh;
    get_text_bounds(font, " ", &mx, &my, &mx1, &my1, &sp_w, &mh, NULL);
    int natural_w = total_word_w + sp_w * gaps;
    int slack = target_w - natural_w;
    // bail out of justification if slack is huge (last short line, etc.)
    if (slack < 0 || slack > target_w / 3) {
        writeln((GFXfont *)font, line, &cx, &cy, fb);
        return;
    }
    int extra_total = slack;
    int extra_each = extra_total / gaps;
    int extra_rem = extra_total % gaps;

    for (size_t i = 0; i < words.size(); ++i) {
        writeln((GFXfont *)font, words[i].c_str(), &cx, &cy, fb);
        if ((int)i < gaps) {
            cx += sp_w + extra_each + ((int)i < extra_rem ? 1 : 0);
        }
    }
}

// Render the current page with justified body text (last line of each
// paragraph is left as-is).
static void render_book_page_text(int x_start, int target_w, int32_t y0, uint8_t *fb) {
    if (chapter_pages.empty()) return;
    const std::string &page = chapter_pages[current_page_in_chapter];
    // split on \n
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : page) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(cur);
    }
    int32_t cy = y0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        bool last_of_para = (i + 1 >= lines.size()) || lines[i + 1].empty();
        bool blank = line.empty();
        if (!blank) {
            render_line((GFXfont *)&FiraSans, line.c_str(),
                        x_start, target_w, cy, fb, !last_of_para);
        }
        cy += book_line_height;
    }
}

// Draw a 1-px horizontal black line at row y, x range [x0, x1).
// 4-bit packed framebuffer: even pixel = low nibble, odd pixel = high nibble.
static void draw_hline(int y, int x0, int x1, uint8_t *fb) {
    if (y < 0 || y >= EPD_HEIGHT) return;
    if (x0 < 0) x0 = 0;
    if (x1 > EPD_WIDTH) x1 = EPD_WIDTH;
    for (int x = x0; x < x1; ++x) {
        int idx = (y * EPD_WIDTH + x) / 2;
        fb[idx] &= (x & 1) ? 0x0F : 0xF0;
    }
}

// Strip the .epub extension from the selected book filename; "Foo.epub" -> "Foo".
static std::string short_title_for_selected() {
    if (selected < 0 || selected >= (int)books.size()) return "";
    std::string n = books[selected];
    size_t dot = n.rfind(".epub");
    if (dot != std::string::npos) n.erase(dot);
    return n;
}

static void render_book_page() {
    if (chapter_pages.empty()) return;
    if (current_page_in_chapter < 0) current_page_in_chapter = 0;
    if (current_page_in_chapter >= (int)chapter_pages.size())
        current_page_in_chapter = chapter_pages.size() - 1;
    dump_current_page_to_serial();

    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    // (no header strip — body uses the full vertical space; the top 80px is
    // still the tap-back-to-library zone, just invisibly so.)
    int target_w = EPD_WIDTH - 2 * BOOK_MARGIN_X;
    render_book_page_text(BOOK_MARGIN_X, target_w, BOOK_TOP_RESERVE + 30, framebuffer);

    // 3-column footer rendered in the smaller font so it occupies less vertical
    // space and reads as secondary metadata: [battery%] [Ch X/Y · NN%] [p A/B]
    int book_progress_pct = (total_spine > 0)
        ? (current_spine * 100) / total_spine
        : 0;
    const GFXfont *small = (const GFXfont *)&firasans_small;
    int32_t footer_y = EPD_HEIGHT - 12;
    // divider just above the footer text
    draw_hline(EPD_HEIGHT - 42, BOOK_MARGIN_X, EPD_WIDTH - BOOK_MARGIN_X, framebuffer);
    char left[24], center[40], right[24];
    snprintf(left, sizeof(left), "%d%%", read_battery_percent());
    snprintf(center, sizeof(center), "Ch %d/%d  -  %d%% read",
             current_spine + 1, total_spine, book_progress_pct);
    snprintf(right, sizeof(right), "p %d/%d",
             current_page_in_chapter + 1, (int)chapter_pages.size());

    int32_t lx = BOOK_MARGIN_X, ly = footer_y;
    writeln(small, left, &lx, &ly, framebuffer);

    int32_t cw, ch_ = 0, cmx = 0, cmy = 0, cmx1, cmy1;
    get_text_bounds(small, center, &cmx, &cmy, &cmx1, &cmy1, &cw, &ch_, NULL);
    int32_t cx_ = (EPD_WIDTH - cw) / 2, cy_ = footer_y;
    writeln(small, center, &cx_, &cy_, framebuffer);

    int32_t rw, rh_ = 0, rmx = 0, rmy = 0, rmx1, rmy1;
    get_text_bounds(small, right, &rmx, &rmy, &rmx1, &rmy1, &rw, &rh_, NULL);
    int32_t rx = EPD_WIDTH - BOOK_MARGIN_X - rw, ry = footer_y;
    writeln(small, right, &rx, &ry, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    // EPD is now electrically idle — safe window to peek at GPIO 0 (STR_100).
    // We latch a flag rather than acting here so loop() consumes it.
    if (sample_str100_button()) {
        Serial.println("[STR_100] press detected (post-render)");
        str100_pressed = true;
    }
}

// Advance/go-back a page; cross chapter boundaries by loading next/prev spine item.
static void book_next_page() {
    if (current_page_in_chapter + 1 < (int)chapter_pages.size()) {
        ++current_page_in_chapter;
        render_book_page();
        save_position();
    } else if (current_spine + 1 < total_spine) {
        if (load_chapter(current_spine + 1)) { render_book_page(); save_position(); }
    }
}

static void book_prev_page() {
    if (current_page_in_chapter > 0) {
        --current_page_in_chapter;
        render_book_page();
        save_position();
    } else if (current_spine > 0) {
        if (load_chapter(current_spine - 1)) {
            current_page_in_chapter = chapter_pages.size() - 1;
            render_book_page();
            save_position();
        }
    }
}

static void dump_library_to_serial() {
    Serial.println("[LIB_BEGIN]");
    for (size_t i = 0; i < books.size(); ++i) {
        Serial.printf("%c %u %s\n",
                      ((int)i == selected) ? '>' : ' ',
                      (unsigned)i, books[i].c_str());
    }
    Serial.println("[LIB_END]");
    Serial.flush();
}

static void render_book_list() {
    Serial.printf("render_book_list: sel=%d page_first=%d total=%u\n",
                  selected, page_first, (unsigned)books.size());
    Serial.flush();
    dump_library_to_serial();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    int32_t cx = LIST_X, cy = LIST_Y;
    char header[96];
    snprintf(header, sizeof(header), "Library  (%u %s)",
             (unsigned)books.size(),
             books.size() == 1 ? "book" : "books");
    writeln((GFXfont *)&FiraSans, header, &cx, &cy, framebuffer);

    if (books.empty()) {
        cx = LIST_X; cy = LIST_Y + 120;
        writeln((GFXfont *)&FiraSans, "No books on the SD card.",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy = LIST_Y + 180;
        writeln((GFXfont *)&FiraSans,
                "Hold the button >= 2s to enter WiFi share mode,",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy = LIST_Y + 240;
        writeln((GFXfont *)&FiraSans,
                "then upload .epub files via the hub.",
                &cx, &cy, framebuffer);
    } else {
        int total_pages = (books.size() + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
        int cur_page = page_first / LINES_PER_PAGE;
        cx = EPD_WIDTH - 240;
        cy = LIST_Y;
        char pageinfo[32];
        snprintf(pageinfo, sizeof(pageinfo), "page %d / %d",
                 cur_page + 1, total_pages > 0 ? total_pages : 1);
        writeln((GFXfont *)&FiraSans, pageinfo, &cx, &cy, framebuffer);

        int row = 0;
        for (size_t i = page_first; i < books.size() && row < LINES_PER_PAGE; ++i, ++row) {
            const char *prefix = ((int)i == selected) ? ">" : " ";
            // Title line
            cx = LIST_X + 40;
            cy = LIST_Y + 60 + row * LINE_HEIGHT;
            std::string title = books[i];
            size_t dot = title.rfind(".epub");
            if (dot != std::string::npos) title.erase(dot);
            if (title.size() > 48) title = title.substr(0, 45) + "...";
            std::string line = std::string(prefix) + " " + title;
            writeln((GFXfont *)&FiraSans, line.c_str(), &cx, &cy, framebuffer);

            // Saved position, right-aligned
            int sp = 0, pg = 0;
            if (read_position_for_name(String(books[i].c_str()), &sp, &pg)) {
                char info[40];
                snprintf(info, sizeof(info), "ch %d  p %d", sp + 1, pg + 1);
                int32_t mx = 0, my = 0, mx1, my1, mw, mh;
                get_text_bounds((GFXfont *)&FiraSans, info,
                                &mx, &my, &mx1, &my1, &mw, &mh, NULL);
                int32_t rx = EPD_WIDTH - LIST_X - mw;
                int32_t ry = LIST_Y + 60 + row * LINE_HEIGHT;
                writeln((GFXfont *)&FiraSans, info, &rx, &ry, framebuffer);
            }
        }
    }

    // footer: tap zones legend in the smaller font, with a divider above.
    draw_hline(EPD_HEIGHT - 80, LIST_X, EPD_WIDTH - LIST_X, framebuffer);
    cx = LIST_X;
    cy = EPD_HEIGHT - 50;
    writeln((GFXfont *)&firasans_small,
            "tap left/right = navigate    tap center = open",
            &cx, &cy, framebuffer);
    cx = LIST_X;
    cy = EPD_HEIGHT - 12;
    writeln((GFXfont *)&firasans_small,
            "hold button:  2s = WiFi share    5s = sleep",
            &cx, &cy, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    // Same idle-window peek as render_book_page(). In library mode the latch
    // is a debug-log only (see loop()), but we still keep it warm so the user
    // gets feedback if the wiring works.
    if (sample_str100_button()) {
        Serial.println("[STR_100] press detected (library post-render)");
        str100_pressed = true;
    }
}

static void move_selection(int delta) {
    if (books.empty()) return;
    int new_sel = selected + delta;
    if (new_sel < 0) new_sel = 0;
    if (new_sel >= (int)books.size()) new_sel = books.size() - 1;
    selected = new_sel;
    page_first = (selected / LINES_PER_PAGE) * LINES_PER_PAGE;
    render_book_list();
}

static void open_selected() {
    if (selected < 0 || selected >= (int)books.size()) return;
    Serial.printf("OPEN: %s\n", books[selected].c_str());
    Serial.flush();

    current_book_path = std::string("/sd/") + books[selected];

    // Try to resume from saved position.
    int saved_ch = 0, saved_pg = 0;
    bool resumed = load_position(&saved_ch, &saved_pg);

    if (load_chapter(resumed ? saved_ch : 0)) {
        Serial.printf("Loaded book: spine=%d, chapter has %u pages%s\n",
                      total_spine, (unsigned)chapter_pages.size(),
                      resumed ? " (resumed)" : "");
        if (resumed && saved_pg >= 0 && saved_pg < (int)chapter_pages.size()) {
            current_page_in_chapter = saved_pg;
        }
        app_mode = MODE_BOOK;
        render_book_page();
    } else {
        Serial.println("Failed to load chapter 0");
    }
}

// ---- Phase E: WiFi AP + web upload server ----

static AsyncWebServer *web_server = nullptr;
static DNSServer *dns_server = nullptr;
static bool ap_active = false;
static String ap_ssid;
static String ap_password;
static uint32_t ap_last_activity = 0;
// Computed at AP entry from g_settings.ap_idle_minutes.
static uint32_t ap_idle_timeout_ms = 5UL * 60UL * 1000UL;
static File ap_upload_file;

static const char *AP_SSID_FIXED = "tiny-reader";
static const char *AP_PASSWORD_FIXED = "bv-birdy";
static const char *MDNS_HOSTNAME = "tiny-reader";

static String ap_build_books_json() {
    String json = "[";
    File root = SD.open("/");
    bool first = true;
    while (root) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = f.name();
            String lower = name; lower.toLowerCase();
            if (lower.endsWith(".epub")) {
                if (!first) json += ",";
                first = false;
                String esc = name;
                esc.replace("\\", "\\\\");
                esc.replace("\"", "\\\"");
                int sp = 0, pg = 0;
                bool has_pos = read_position_for_name(name, &sp, &pg);
                json += "{\"name\":\"" + esc + "\"";
                json += ",\"size\":" + String(f.size());
                json += ",\"spine\":" + String(sp);
                json += ",\"page\":" + String(pg);
                json += ",\"hasPos\":" + String(has_pos ? "true" : "false");
                json += "}";
            }
        }
        f.close();
    }
    if (root) root.close();
    json += "]";
    return json;
}

static void render_ap_screen() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    // Title
    int32_t cx = 60, cy = 70;
    writeln((GFXfont *)&FiraSans, "tiny-reader hub", &cx, &cy, framebuffer);

    // Underline
    for (int x = 60; x < EPD_WIDTH - 60; ++x) {
        int idx = (90 * EPD_WIDTH + x) / 2;
        framebuffer[idx] &= (x & 1) ? 0x0F : 0xF0;
    }

    // 1. Connect
    cx = 60; cy = 160;
    writeln((GFXfont *)&FiraSans, "1.  Connect to this WiFi network:",
            &cx, &cy, framebuffer);
    char line[200];
    cx = 100; cy = 215;
    snprintf(line, sizeof(line), "SSID:      %s", ap_ssid.c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);
    cx = 100; cy = 265;
    snprintf(line, sizeof(line), "Password:  %s", ap_password.c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    // 2. Open URL
    cx = 60; cy = 340;
    writeln((GFXfont *)&FiraSans, "2.  Open in any browser:",
            &cx, &cy, framebuffer);
    cx = 100; cy = 395;
    snprintf(line, sizeof(line), "http://%s",
             WiFi.softAPIP().toString().c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    // Footer
    cx = 60; cy = EPD_HEIGHT - 30;
    snprintf(line, sizeof(line),
             "Hold button >= 1.5s to exit  -  auto-exits after %d min idle",
             g_settings.ap_idle_minutes);
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

static const char AP_INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"/>
<meta name=viewport content="width=device-width,initial-scale=1"/>
<title>tiny-reader</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:640px;margin:0 auto;padding:1em;}
h1{font-size:1.4em;}
.book{padding:.6em;border-bottom:1px solid #ccc;}
.book-row{display:flex;align-items:center;gap:.5em;}
.book-name{flex:1;word-break:break-all;}
.book-meta{color:#666;font-size:.85em;display:flex;gap:.6em;margin-top:.2em;}
button{font-size:.95em;padding:.4em .8em;}
.btn-link{background:none;border:none;color:#06c;padding:0;cursor:pointer;text-decoration:underline;}
form{margin-top:1em;padding:1em;border:1px solid #ccc;border-radius:6px;}
.danger{background:#fee;color:#900;border:1px solid #c66;}
.muted{color:#666;font-size:.9em;}
.editor{margin-top:.4em;padding:.4em;background:#f5f5f5;border-radius:4px;display:flex;align-items:center;gap:.4em;flex-wrap:wrap;}
.editor input{width:5em;font-size:1em;padding:.2em;}
</style></head><body>
<h1>tiny-reader</h1>
<div id=books class=muted>loading...</div>
<form id=up onsubmit="upload(event)">
<h3>upload .epub</h3>
<input type=file name=file accept=".epub" required/>
<button>upload</button>
<div id=status class=muted></div>
</form>
<form id=sf onsubmit="saveSettings(event)">
<h3>settings</h3>
<label>text density
 <select name=density id=density>
  <option value=0>compact (more text per page)</option>
  <option value=1>medium (default)</option>
  <option value=2>loose (more breathing room)</option>
 </select>
</label>
<br><br>
<label>WiFi share auto-exit after
 <input type=number name=ap_idle_minutes id=apidle min=1 max=120 step=1 style="width:5em">
 minutes idle
</label>
<br><br>
<button>save settings</button>
<div id=settings_status class=muted></div>
</form>
<script>
 var esc=function(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;};
 var jsq=function(s){return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'");};
 var load=function(){
  fetch('/api/books').then(function(r){return r.json();}).then(function(list){
   var d=document.getElementById('books');
   if(!list.length){d.innerHTML='<p class=muted>(empty)</p>';return;}
   d.innerHTML=list.map(function(b){
    var pos = b.hasPos? ('ch '+(b.spine+1)+', page '+(b.page+1)) : 'not started';
    return '<div class=book>'+
     '<div class=book-row>'+
      '<span class=book-name>'+esc(b.name)+'</span>'+
      '<button class=danger onclick="del(\''+jsq(b.name)+'\')">delete</button>'+
     '</div>'+
     '<div class=book-meta>'+
      '<span>'+(b.size/1024).toFixed(0)+'KB</span>'+
      '<span>'+pos+'</span>'+
      '<a href="/api/books/'+encodeURIComponent(b.name)+'" download>download</a>'+
      '<button class=btn-link onclick="editPos(\''+jsq(b.name)+'\','+b.spine+','+b.page+')">edit position</button>'+
     '</div>'+
     '<div class=editor id="ed-'+esc(b.name)+'" style="display:none"></div>'+
    '</div>';
   }).join('');
  });
 };
 var editPos=function(name,sp,pg){
  var sel='#ed-'+CSS.escape(name);
  var ed=document.querySelector(sel);
  ed.style.display='flex';
  ed.innerHTML='chapter <input id=sp value='+(sp+1)+' min=1> page <input id=pg value='+(pg+1)+' min=1> '+
   '<button class=btn-link onclick="savePos(\''+jsq(name)+'\')">save</button>'+
   '<button class=btn-link onclick="cancelPos(\''+jsq(name)+'\')">cancel</button>';
 };
 var cancelPos=function(name){
  var ed=document.querySelector('#ed-'+CSS.escape(name));
  ed.style.display='none'; ed.innerHTML='';
 };
 var savePos=function(name){
  var ed=document.querySelector('#ed-'+CSS.escape(name));
  var sp=Math.max(0,parseInt(ed.querySelector('#sp').value,10)-1);
  var pg=Math.max(0,parseInt(ed.querySelector('#pg').value,10)-1);
  var fd=new URLSearchParams(); fd.append('spine',sp); fd.append('page',pg);
  fetch('/api/books/'+encodeURIComponent(name)+'/pos',
        {method:'POST',body:fd,headers:{'Content-Type':'application/x-www-form-urlencoded'}})
   .then(function(r){if(r.ok){load();}else{alert('save failed');}});
 };
 var upload=function(e){
  e.preventDefault();
  var f=e.target.file.files[0]; if(!f) return;
  var fd=new FormData(); fd.append('file',f);
  document.getElementById('status').textContent='uploading...';
  fetch('/upload',{method:'POST',body:fd}).then(function(r){
   if(r.ok){document.getElementById('status').textContent='done';load();}
   else r.text().then(function(t){document.getElementById('status').textContent='failed: '+t;});
  });
 };
 var del=function(n){
  if(!confirm('delete '+n+'?')) return;
  fetch('/api/books/'+encodeURIComponent(n),{method:'DELETE'}).then(function(r){
   if(r.ok) load(); else alert('delete failed');
  });
 };
 var loadSettings=function(){
  fetch('/api/settings').then(function(r){return r.json();}).then(function(s){
   document.getElementById('density').value=s.density;
   document.getElementById('apidle').value=s.ap_idle_minutes;
  });
 };
 var saveSettings=function(e){
  e.preventDefault();
  var st=document.getElementById('settings_status');
  var fd=new URLSearchParams();
  fd.append('density',document.getElementById('density').value);
  fd.append('ap_idle_minutes',document.getElementById('apidle').value);
  st.textContent='saving...';
  fetch('/api/settings',{method:'POST',body:fd,
        headers:{'Content-Type':'application/x-www-form-urlencoded'}})
   .then(function(r){
    if(!r.ok) throw new Error('HTTP '+r.status);
    return r.json();
   })
   .then(function(j){
    st.textContent=j.changed? 'saved (applies on next book open)'
                            : 'saved (no changes)';
   })
   .catch(function(err){
    st.textContent='save failed: '+err.message;
   });
 };
 load();
 loadSettings();
</script></body></html>
)HTML";

// Async handlers below take AsyncWebServerRequest *req.
static void ap_handle_root(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    AsyncWebServerResponse *r = req->beginResponse_P(200, "text/html; charset=utf-8", AP_INDEX_HTML);
    req->send(r);
}
static void ap_handle_settings_get(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    Serial.printf("[HTTP] GET /api/settings (density=%d ap_idle=%d)\n",
                  g_settings.density, g_settings.ap_idle_minutes);
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"density\":%d,\"ap_idle_minutes\":%d}",
             g_settings.density, g_settings.ap_idle_minutes);
    req->send(200, "application/json", buf);
}

static void ap_handle_settings_post(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    Serial.printf("[HTTP] POST /api/settings — params: %d\n", req->params());
    for (size_t i = 0; i < req->params(); ++i) {
        const AsyncWebParameter *p = req->getParam(i);
        Serial.printf("  - %s = %s (post=%d)\n",
                      p->name().c_str(), p->value().c_str(), p->isPost());
    }
    bool changed = false;
    if (req->hasParam("density", true)) {
        int d = req->getParam("density", true)->value().toInt();
        if (d >= 0 && d <= 2 && d != g_settings.density) {
            g_settings.density = d;
            changed = true;
        }
    }
    if (req->hasParam("ap_idle_minutes", true)) {
        int m = req->getParam("ap_idle_minutes", true)->value().toInt();
        if (m >= 1 && m <= 120 && m != g_settings.ap_idle_minutes) {
            g_settings.ap_idle_minutes = m;
            // Apply to the live session too so the user doesn't have to
            // re-enter AP for a longer timeout to take effect.
            ap_idle_timeout_ms = (uint32_t)m * 60UL * 1000UL;
            Serial.printf("[AP] idle timeout updated live to %d min\n", m);
            changed = true;
        }
    }
    if (changed) {
        save_settings();
        apply_density();
    }
    req->send(200, "application/json", changed ? "{\"ok\":true,\"changed\":true}"
                                                : "{\"ok\":true,\"changed\":false}");
}

static void ap_handle_books_json(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    String json = ap_build_books_json();
    Serial.printf("[HTTP] GET /api/books -> %d bytes\n", json.length());
    req->send(200, "application/json", json);
}

// Multipart upload: AsyncWebServer calls back per-chunk.
static void ap_handle_upload_chunk(AsyncWebServerRequest *req, String filename,
                                   size_t index, uint8_t *data, size_t len, bool final) {
    ap_last_activity = millis();
    if (index == 0) {
        String path = String("/") + filename;
        if (SD.exists(path)) SD.remove(path);
        ap_upload_file = SD.open(path, FILE_WRITE);
        Serial.printf("[UPLOAD] start %s\n", path.c_str());
    }
    if (ap_upload_file && len) {
        ap_upload_file.write(data, len);
    }
    if (final) {
        if (ap_upload_file) ap_upload_file.close();
        Serial.printf("[UPLOAD] done %u bytes\n", (unsigned)(index + len));
    }
}
static void ap_handle_upload_done(AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"ok\":true}");
}

// Common /api/books/<name>... handler
static void ap_handle_book_path(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    String uri = req->url();
    if (!uri.startsWith("/api/books/")) {
        req->send(404, "text/plain", "not found");
        return;
    }
    String tail = uri.substring(strlen("/api/books/"));
    String name = tail, action;
    int slash = tail.lastIndexOf('/');
    if (slash > 0) {
        action = tail.substring(slash + 1);
        name = tail.substring(0, slash);
    }
    // AsyncWebServer auto-decodes URI; it's already decoded in url()
    String path = String("/") + name;

    if (action == "pos") {
        if (req->method() == HTTP_GET) {
            int sp = 0, pg = 0;
            bool ok = read_position_for_name(name, &sp, &pg);
            char buf[80];
            snprintf(buf, sizeof(buf), "{\"spine\":%d,\"page\":%d,\"saved\":%s}",
                     sp, pg, ok ? "true" : "false");
            req->send(200, "application/json", buf);
        } else if (req->method() == HTTP_POST) {
            int sp = 0, pg = 0;
            if (req->hasParam("spine", true)) sp = req->getParam("spine", true)->value().toInt();
            if (req->hasParam("page",  true)) pg = req->getParam("page",  true)->value().toInt();
            if (sp < 0) sp = 0;
            if (pg < 0) pg = 0;
            bool ok = write_position_for_name(name, sp, pg);
            req->send(ok ? 200 : 500, "application/json",
                      ok ? "{\"ok\":true}" : "{\"error\":\"write failed\"}");
        } else {
            req->send(405, "text/plain", "method not allowed");
        }
        return;
    }

    if (req->method() == HTTP_DELETE) {
        if (SD.exists(path)) {
            SD.remove(path);
            String pos_path = pos_path_for_name(name);
            if (SD.exists(pos_path)) SD.remove(pos_path);
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    } else {
        if (!SD.exists(path)) {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
            return;
        }
        req->send(SD, path, "application/epub+zip");
    }
}

static void ap_handle_not_found(AsyncWebServerRequest *req) {
    String uri = req->url();
    if (uri.startsWith("/api/books/")) { ap_handle_book_path(req); return; }
    Serial.printf("[HTTP] catchall %s\n", uri.c_str());
    AsyncWebServerResponse *r = req->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "/");
    req->send(r);
}

static uint32_t ap_started_at = 0;

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_START:        Serial.println("[WIFI] AP_START"); break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED: Serial.println("[WIFI] STA connected"); break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:Serial.println("[WIFI] STA disconnected"); break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:Serial.println("[WIFI] STA got IP"); break;
        default: break;
    }
}

static void enter_ap_mode() {
    if (ap_active) return;
    Serial.println("[AP] entering AP mode");

    ap_ssid = AP_SSID_FIXED;
    ap_password = AP_PASSWORD_FIXED;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
    WiFi.setSleep(false);   // keep radio on; AP power-save drops connections.
    delay(200);

    // DHCP-DNS hijack: tell connecting phones to use 192.168.4.1 as their DNS.
    // Without this, phones resolve connectivitycheck.gstatic.com via cellular
    // DNS, fail (no internet path), declare "no internet" and drop the link.
    // Have to stop dhcps, set option + DNS, then restart.
    {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) {
            esp_netif_dns_info_t dns_info = {};
            IP_ADDR4(&dns_info.ip, 192, 168, 4, 1);
            esp_netif_dhcps_stop(ap_netif);
            uint8_t opt_val = 1;  // OFFER_DNS = 1
            esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_DOMAIN_NAME_SERVER,
                                   &opt_val, sizeof(opt_val));
            esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
            esp_netif_dhcps_start(ap_netif);
            Serial.println("[AP] DHCP advertising 192.168.4.1 as DNS");
        } else {
            Serial.println("[AP] WIFI_AP_DEF netif not found; DHCP-DNS skip");
        }
    }

    IPAddress ip = WiFi.softAPIP();

    // Captive-portal DNS: every domain resolves to our IP so phones treat us
    // as a portal and stay connected.
    dns_server = new DNSServer();
    dns_server->setErrorReplyCode(DNSReplyCode::NoError);
    dns_server->start(53, "*", ip);

    if (!MDNS.begin(MDNS_HOSTNAME)) Serial.println("[AP] mDNS begin failed");
    MDNS.addService("http", "tcp", 80);

    web_server = new AsyncWebServer(80);
    web_server->on("/", HTTP_GET, ap_handle_root);
    web_server->on("/api/books", HTTP_GET, ap_handle_books_json);
    web_server->on("/api/settings", HTTP_GET, ap_handle_settings_get);
    web_server->on("/api/settings", HTTP_POST, ap_handle_settings_post);
    web_server->on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
        Serial.println("[HTTP] GET /ping");
        r->send(200, "text/plain", "pong");
    });
    // /upload — multipart POST, called as upload chunks stream in
    web_server->on("/upload", HTTP_POST, ap_handle_upload_done,
                   ap_handle_upload_chunk);
    // Captive-portal probes — return what each OS expects so the device
    // considers the network "online" and doesn't restrict fetches.
    web_server->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(204, "text/plain", "");
    });
    web_server->on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(204, "text/plain", "");
    });
    web_server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html",
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    web_server->on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html",
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    web_server->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/plain", "Microsoft Connect Test");
    });
    web_server->on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/plain", "Microsoft NCSI");
    });
    web_server->onNotFound(ap_handle_not_found);
    web_server->begin();

    ap_active = true;
    ap_last_activity = millis();
    ap_started_at = millis();
    ap_idle_timeout_ms = (uint32_t)g_settings.ap_idle_minutes * 60UL * 1000UL;
    Serial.printf("[AP] up: SSID=%s pw=%s ip=%s\n",
                  ap_ssid.c_str(), ap_password.c_str(),
                  WiFi.softAPIP().toString().c_str());

    render_ap_screen();
}

static void exit_ap_mode() {
    if (!ap_active) return;
    Serial.println("[AP] exit_ap_mode() called");
    if (web_server) {
        web_server->end();
        delete web_server;
        web_server = nullptr;
    }
    if (dns_server) {
        dns_server->stop();
        delete dns_server;
        dns_server = nullptr;
    }
    MDNS.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    ap_active = false;

    // refresh and return to library so any newly uploaded books appear
    scan_sd_for_epubs();
    app_mode = MODE_LIBRARY;
    render_book_list();
}

// ---- setup / loop ----

void setup() {
    Serial.begin(115200);
    // Detect wake-from-deep-sleep: USB-CDC takes ~1.5s to re-enumerate on the
    // host, so logs printed before that get lost. Give it time.
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool from_sleep = (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED);
    delay(from_sleep ? 1800 : 200);
    Serial.printf("\ntiny-reader starting (wake_cause=%d%s)\n",
                  (int)wake_cause, from_sleep ? " [post-sleep]" : "");

    // SD card on built-in slot
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI);
    if (!SD.begin(SD_CS, SPI)) {
        Serial.println("SD init failed");
    } else {
        Serial.printf("SD ok, card size %.2f GB\n",
                      SD.cardSize() / 1024.0 / 1024.0 / 1024.0);
    }

    framebuffer = (uint8_t *)ps_calloc(1, EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("framebuffer alloc failed!");
        while (true) delay(1000);
    }

    epd_init();

    // GT911 touch wakeup pulse, scan I²C for address, init driver
    pinMode(TOUCH_INT, OUTPUT);
    digitalWrite(TOUCH_INT, HIGH);
    Wire.begin(BOARD_SDA, BOARD_SCL);

    uint8_t touchAddress = 0x14;
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) touchAddress = 0x14;
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) touchAddress = 0x5D;

    touch.setPins(-1, TOUCH_INT);
    touchOnline = touch.begin(Wire, touchAddress, BOARD_SDA, BOARD_SCL);
    if (touchOnline) {
        touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
        touch.setMirrorXY(false, false);
        touch.setSwapXY(true);
        Serial.printf("GT911 ok at 0x%02X\n", touchAddress);
    } else {
        Serial.println("GT911 init failed — falling back to BUTTON_1");
    }

    pinMode(BUTTON_1, INPUT_PULLUP);   // SENSOR_VN, GPIO 21
    // NOTE: STR_100 button is on GPIO 0, which is also the EPD CFG_STR strobe
    // (see ed047tc1.h: CFG_STR = GPIO_NUM_0). We can't hold it as a button
    // input. Instead, sample_str100_button() flips the pin to INPUT_PULLUP
    // for ~100µs after each epd_poweroff() and restores it to OUTPUT/HIGH —
    // see render_book_page() / render_book_list(). Backlog #15.

    // Register WiFi event callback once (was inside enter_ap_mode → stacked).
    WiFi.onEvent(on_wifi_event);

    load_settings();
    apply_density();

    scan_sd_for_epubs();
    clear_and_flush();

    // If the last shutdown was from sleep while reading a book, auto-resume.
    bool resumed_from_sleep = false;
    {
        Preferences prefs;
        prefs.begin("tinyreader", true);   // read-only
        String last = prefs.getString("last_book", "");
        prefs.end();
        Serial.printf("[WAKE] NVS last_book = '%s' (%d bytes)\n",
                      last.c_str(), last.length());
        if (last.length() > 0) {
            for (size_t i = 0; i < books.size(); ++i) {
                if (last == books[i].c_str()) {
                    selected = (int)i;
                    Serial.printf("[WAKE] auto-opening idx=%d: %s\n",
                                  (int)i, last.c_str());
                    open_selected();   // .pos resume happens inside
                    resumed_from_sleep = true;
                    break;
                }
            }
            if (!resumed_from_sleep) {
                Serial.println("[WAKE] last book not in library, showing list");
            }
        }
    }
    if (!resumed_from_sleep) render_book_list();
}

// returns 0/1/2/3 = none/left/center/right ; (or -1 on no touch)
static int classify_tap(int16_t x, int16_t y) {
    if (x < EPD_WIDTH / 4) return 1;            // left third
    if (x > 3 * EPD_WIDTH / 4) return 3;        // right third
    return 2;                                   // center
}

// Render a "sleeping" screen, persist book position if applicable, and
// enter ESP32 deep sleep. Wake on GPIO 21 going LOW (the same button used
// for navigation). After wake, the chip resets and setup() runs again —
// the reader resumes from the .pos file written here.
static void enter_deep_sleep() {
    Serial.println("[SLEEP] entering deep sleep");
    Serial.flush();

    // Persist the currently-open book name to NVS (durable across deep sleep
    // — SD-write caches can lose the marker if we sleep right after).
    Preferences prefs;
    prefs.begin("tinyreader", false);
    if (app_mode == MODE_BOOK) {
        save_position();
        prefs.putString("last_book", books[selected].c_str());
        Serial.printf("[SLEEP] last_book NVS = %s\n", books[selected].c_str());
    } else {
        prefs.remove("last_book");
        Serial.println("[SLEEP] cleared last_book NVS");
    }
    prefs.end();

    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int32_t cx = 60, cy = 200;
    writeln((GFXfont *)&FiraSans, "Sleeping", &cx, &cy, framebuffer);
    cx = 60; cy = 280;
    writeln((GFXfont *)&FiraSans,
            "Press the button to wake.", &cx, &cy, framebuffer);
    cx = 60; cy = 340;
    writeln((GFXfont *)&FiraSans,
            "(Your reading position is saved.)", &cx, &cy, framebuffer);
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    // Wait for the button to be released before arming wake — otherwise ext0
    // sees it still LOW and wakes the chip immediately, looking like sleep
    // never happened.
    Serial.println("[SLEEP] waiting for button release...");
    Serial.flush();
    uint32_t wait_start = millis();
    while (digitalRead(BUTTON_1) == LOW) {
        delay(20);
        if (millis() - wait_start > 10000) break;  // safety bail
    }
    delay(150);   // debounce after release

    // Wake on BUTTON_1 (GPIO 21) pulled LOW. The Arduino INPUT_PULLUP we set
    // earlier is gated on the digital GPIO peripheral, which is powered down
    // during deep sleep. Explicitly enable the RTC-domain pull-up so the pin
    // stays HIGH while sleeping (otherwise it floats LOW → ext0 fires
    // immediately → "sleep" appears to do nothing).
    rtc_gpio_init(GPIO_NUM_21);
    rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(GPIO_NUM_21);
    rtc_gpio_pullup_en(GPIO_NUM_21);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_21, 0);
    Serial.println("[SLEEP] arming ext0 wake on GPIO 21 (RTC pull-up on), sleeping");
    Serial.flush();
    esp_deep_sleep_start();   // does not return
}

// Process newline-terminated serial commands. Returns true when a command was handled.
static void handle_serial_commands() {
    static String buf;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf.trim();
            if (buf.length() == 0) { buf = ""; continue; }
            Serial.printf("[CMD] %s\n", buf.c_str());
            String cmd = buf;
            buf = "";
            if (cmd == "next") {
                if (app_mode == MODE_BOOK) book_next_page();
            } else if (cmd == "prev") {
                if (app_mode == MODE_BOOK) book_prev_page();
            } else if (cmd == "back") {
                if (app_mode == MODE_BOOK) { app_mode = MODE_LIBRARY; render_book_list(); }
            } else if (cmd == "dump") {
                if (app_mode == MODE_BOOK) dump_current_page_to_serial();
                else dump_library_to_serial();
            } else if (cmd.startsWith("open ")) {
                int n = cmd.substring(5).toInt();
                if (n >= 0 && n < (int)books.size()) {
                    selected = n;
                    open_selected();
                }
            } else if (cmd.startsWith("goto ")) {
                int ch = cmd.substring(5).toInt();
                if (app_mode == MODE_BOOK && ch >= 0 && ch < total_spine) {
                    if (load_chapter(ch)) render_book_page();
                }
            } else if (cmd == "share") {
                enter_ap_mode();
            } else if (cmd == "stop_share" || cmd == "unshare") {
                exit_ap_mode();
            } else if (cmd == "sleep") {
                enter_deep_sleep();   // does not return
            } else if (cmd == "probe_buttons") {
                Serial.println("[PROBE] press each button. Listening for 30s.");
                const int pins[] = {0, 10, 14, 21, 38, 39, 45, 48};
                const int n = sizeof(pins)/sizeof(pins[0]);
                for (int i = 0; i < n; ++i) pinMode(pins[i], INPUT_PULLUP);
                bool last[16];
                for (int i = 0; i < n; ++i) last[i] = (digitalRead(pins[i]) == LOW);
                uint32_t end_at = millis() + 30000;
                while (millis() < end_at) {
                    for (int i = 0; i < n; ++i) {
                        bool now_low = (digitalRead(pins[i]) == LOW);
                        if (now_low != last[i]) {
                            Serial.printf("[PROBE] GPIO %d -> %s\n",
                                          pins[i], now_low ? "LOW (pressed)" : "HIGH (released)");
                            last[i] = now_low;
                        }
                    }
                    delay(15);
                }
                Serial.println("[PROBE] done");
            } else if (cmd == "help") {
                Serial.println("commands: next prev back open <n> goto <ch> dump share stop_share sleep probe_buttons help");
            } else {
                Serial.printf("[ERR] unknown cmd: %s\n", cmd.c_str());
            }
        } else if (buf.length() < 64) {
            buf += c;
        }
    }
}

void loop() {
    static uint32_t input_cooldown_until = 0;
    static uint32_t button_press_start = 0;
    static bool button_long_handled = false;
    uint32_t now = millis();

    handle_serial_commands();

    // ---- AP / web-server mode ----
    if (ap_active) {
        if (dns_server) dns_server->processNextRequest();
        // AsyncWebServer runs its own task; no handleClient() needed.
        if (now - ap_last_activity > ap_idle_timeout_ms) {
            Serial.println("[AP] idle timeout");
            exit_ap_mode();
            return;
        }
        // To exit: button must be held LOW for ≥1500ms. First 3s after entering
        // ignore the button so the long-press that entered won't auto-exit.
        if (now - ap_started_at > 3000) {
            static uint32_t btn_low_since = 0;
            if (digitalRead(BUTTON_1) == LOW) {
                if (btn_low_since == 0) btn_low_since = now;
                if (now - btn_low_since > 1500) {
                    Serial.println("[AP] button-hold exit");
                    btn_low_since = 0;
                    exit_ap_mode();
                    input_cooldown_until = millis() + 1000;
                    return;
                }
            } else {
                btn_low_since = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        return;
    }

    // touch — edge-triggered on press (not held)
    if (touchOnline) {
        static bool touch_was_down = false;
        int16_t xs[5], ys[5];
        uint8_t n = touch.getPoint(xs, ys, 1);
        if (n > 0) {
            if (!touch_was_down && now >= input_cooldown_until) {
                int zone = classify_tap(xs[0], ys[0]);
                Serial.printf("tap (%d,%d) zone=%d mode=%d\n",
                              xs[0], ys[0], zone, app_mode);
                Serial.flush();
                if (app_mode == MODE_BOOK) {
                    if (ys[0] < 80) {
                        Serial.println("[TAP] top → back to library");
                        app_mode = MODE_LIBRARY;
                        render_book_list();
                    } else if (xs[0] < EPD_WIDTH / 2) book_prev_page();
                    else book_next_page();
                } else {
                    if (zone == 1) move_selection(-1);
                    else if (zone == 3) move_selection(+1);
                    else if (zone == 2) open_selected();
                }
                input_cooldown_until = millis() + 300;
            }
            touch_was_down = true;
        } else {
            touch_was_down = false;
        }
    }

    // GPIO 21 (SENSOR_VN, BUTTON_1): short press in book mode = back to library;
    //                                short press in library = cycle selection;
    //                                long press 2-5 s on release = WiFi share mode;
    //                                very long press ≥ 5 s = deep sleep (immediate).
    {
        bool down = (digitalRead(BUTTON_1) == LOW);
        if (down) {
            if (button_press_start == 0) {
                button_press_start = now;
                button_long_handled = false;
            } else if (!button_long_handled && now - button_press_start >= 5000) {
                // Hit the 5 s mark while still held → sleep, don't fall
                // through to the 2 s AP-mode path on release.
                button_long_handled = true;
                Serial.println("[BTN] very-long press → deep sleep");
                enter_deep_sleep();   // does not return
            }
        } else {
            if (button_press_start && !button_long_handled &&
                now >= input_cooldown_until) {
                uint32_t held = now - button_press_start;
                if (held >= 2000) {
                    Serial.println("[BTN] long press (released) → AP mode");
                    enter_ap_mode();
                    input_cooldown_until = now + 1000;
                } else if (app_mode == MODE_BOOK) {
                    app_mode = MODE_LIBRARY;
                    render_book_list();
                    input_cooldown_until = now + 600;
                } else {
                    move_selection(+1);
                    input_cooldown_until = now + 600;
                }
            }
            button_press_start = 0;
        }
    }

    // STR_100 button (GPIO 0): it shares the EPD CFG_STR strobe, so we can't
    // poll it from loop(). Instead the render functions sample it during the
    // post-poweroff idle window and set str100_pressed. Consume it here.
    if (str100_pressed) {
        str100_pressed = false;
        if (now >= input_cooldown_until) {
            if (app_mode == MODE_BOOK) {
                Serial.println("[STR_100] consumed → back to library");
                app_mode = MODE_LIBRARY;
                render_book_list();
                input_cooldown_until = millis() + 600;
            } else {
                Serial.println("[STR_100] consumed in library mode (no-op)");
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}
