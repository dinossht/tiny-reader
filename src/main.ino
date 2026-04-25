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
#include "utilities.h"
#include <TouchDrvGT911.hpp>
#include "Epub.h"

// Phase E: WiFi AP + on-board web server
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

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
static const int BOOK_LINE_HEIGHT = 50;     // FiraSans advance_y per firasans.h
static const int BOOK_TOP_RESERVE = 30;     // small visual margin only
static const int BOOK_FOOTER_RESERVE = 50;
static const int BOOK_LINES_PER_PAGE =
    (EPD_HEIGHT - BOOK_TOP_RESERVE - BOOK_FOOTER_RESERVE) / BOOK_LINE_HEIGHT;  // ~9
static const int BOOK_CHARS_PER_LINE = 44;  // FiraSans is variable-width; conservative limit

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

// Greedy word-wrap to ~BOOK_CHARS_PER_LINE per line; preserve paragraph breaks.
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
                        } else if ((int)(current.size() + 1 + word.size()) <= BOOK_CHARS_PER_LINE) {
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
        if (n_in_page >= BOOK_LINES_PER_PAGE) {
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

// SD path "/<book-without-extension>.pos" for the selected book.
static String pos_path_for_selected() {
    String name = String(books[selected].c_str());
    int dot = name.lastIndexOf('.');
    if (dot <= 0) dot = name.length();
    return String("/") + name.substring(0, dot) + ".pos";
}

static void save_position() {
    if (selected < 0 || selected >= (int)books.size()) return;
    String path = pos_path_for_selected();
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("save_position: open %s failed\n", path.c_str()); return; }
    f.printf("%d %d\n", current_spine, current_page_in_chapter);
    f.close();
    Serial.printf("[POS_SAVED] %s -> ch=%d p=%d\n",
                  path.c_str(), current_spine, current_page_in_chapter);
}

// Returns true if a position was loaded; sets *out_spine and *out_page.
static bool load_position(int *out_spine, int *out_page) {
    if (selected < 0 || selected >= (int)books.size()) return false;
    String path = pos_path_for_selected();
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    String line = f.readStringUntil('\n');
    f.close();
    int sp_idx = -1, pg_idx = -1;
    if (sscanf(line.c_str(), "%d %d", &sp_idx, &pg_idx) == 2) {
        *out_spine = sp_idx;
        *out_page = pg_idx;
        Serial.printf("[POS_LOADED] %s -> ch=%d p=%d\n",
                      path.c_str(), sp_idx, pg_idx);
        return true;
    }
    return false;
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
        cy += BOOK_LINE_HEIGHT;
    }
}

static void render_book_page() {
    if (chapter_pages.empty()) return;
    if (current_page_in_chapter < 0) current_page_in_chapter = 0;
    if (current_page_in_chapter >= (int)chapter_pages.size())
        current_page_in_chapter = chapter_pages.size() - 1;
    dump_current_page_to_serial();

    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int target_w = EPD_WIDTH - 2 * BOOK_MARGIN_X;
    render_book_page_text(BOOK_MARGIN_X, target_w, BOOK_TOP_RESERVE + 30, framebuffer);

    // 3-column footer at bottom: [battery]   [chapter]   [page]
    int32_t footer_y = EPD_HEIGHT - 15;
    char left[24], center[24], right[24];
    snprintf(left, sizeof(left), "%d%%", read_battery_percent());
    snprintf(center, sizeof(center), "Chapter %d/%d", current_spine + 1, total_spine);
    snprintf(right, sizeof(right), "%d/%d",
             current_page_in_chapter + 1, (int)chapter_pages.size());

    int32_t lx = BOOK_MARGIN_X, ly = footer_y;
    writeln((GFXfont *)&FiraSans, left, &lx, &ly, framebuffer);

    // measure center to actually center it
    int32_t cw, ch_ = 0, cmx = 0, cmy = 0, cmx1, cmy1;
    get_text_bounds((GFXfont *)&FiraSans, center, &cmx, &cmy, &cmx1, &cmy1, &cw, &ch_, NULL);
    int32_t cx_ = (EPD_WIDTH - cw) / 2, cy_ = footer_y;
    writeln((GFXfont *)&FiraSans, center, &cx_, &cy_, framebuffer);

    int32_t rw, rh_ = 0, rmx = 0, rmy = 0, rmx1, rmy1;
    get_text_bounds((GFXfont *)&FiraSans, right, &rmx, &rmy, &rmx1, &rmy1, &rw, &rh_, NULL);
    int32_t rx = EPD_WIDTH - BOOK_MARGIN_X - rw, ry = footer_y;
    writeln((GFXfont *)&FiraSans, right, &rx, &ry, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
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
    snprintf(header, sizeof(header), "Library  (%u books)", (unsigned)books.size());
    writeln((GFXfont *)&FiraSans, header, &cx, &cy, framebuffer);

    int total_pages = (books.size() + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    int cur_page = page_first / LINES_PER_PAGE;
    cx = EPD_WIDTH - 240;
    cy = LIST_Y;
    char pageinfo[32];
    snprintf(pageinfo, sizeof(pageinfo), "page %d / %d", cur_page + 1, total_pages > 0 ? total_pages : 1);
    writeln((GFXfont *)&FiraSans, pageinfo, &cx, &cy, framebuffer);

    int row = 0;
    for (size_t i = page_first; i < books.size() && row < LINES_PER_PAGE; ++i, ++row) {
        cx = LIST_X + 40;
        cy = LIST_Y + 60 + row * LINE_HEIGHT;
        const char *prefix = ((int)i == selected) ? "> " : "  ";
        std::string line = std::string(prefix) + books[i];
        writeln((GFXfont *)&FiraSans, line.c_str(), &cx, &cy, framebuffer);
    }

    // footer: tap zones legend
    cx = LIST_X;
    cy = EPD_HEIGHT - 30;
    writeln((GFXfont *)&FiraSans,
            "tap left = prev   tap center = open   tap right = next",
            &cx, &cy, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
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

static WebServer *web_server = nullptr;
static DNSServer *dns_server = nullptr;
static bool ap_active = false;
static String ap_ssid;
static String ap_password;
static uint32_t ap_last_activity = 0;
static const uint32_t AP_IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 min
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
                json += "{\"name\":\"" + esc + "\",\"size\":" + String(f.size()) + "}";
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
    int32_t cx = 60, cy = 80;
    writeln((GFXfont *)&FiraSans, "WiFi Share Mode", &cx, &cy, framebuffer);

    char line[160];
    cx = 60; cy = 180;
    snprintf(line, sizeof(line), "SSID:     %s", ap_ssid.c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    cx = 60; cy = 240;
    snprintf(line, sizeof(line), "Password: %s", ap_password.c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    cx = 60; cy = 300;
    snprintf(line, sizeof(line), "URL:      http://tiny-reader.local");
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    cx = 60; cy = 360;
    snprintf(line, sizeof(line), "or        http://%s",
             WiFi.softAPIP().toString().c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    cx = 60; cy = EPD_HEIGHT - 30;
    writeln((GFXfont *)&FiraSans,
            "Press button to exit (also auto-exits after 5 min idle)",
            &cx, &cy, framebuffer);

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

static const char AP_INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html><head><meta name=viewport content="width=device-width,initial-scale=1"/>
<title>tiny-reader</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:640px;margin:0 auto;padding:1em;}
h1{font-size:1.4em;}
.book{padding:.6em;border-bottom:1px solid #ccc;display:flex;align-items:center;gap:.5em;}
.book-name{flex:1;word-break:break-all;}
.book-size{color:#666;font-size:.85em;}
button{font-size:1em;padding:.4em .8em;}
form{margin-top:1em;padding:1em;border:1px solid #ccc;border-radius:6px;}
.danger{background:#fee;color:#900;border:1px solid #c66;}
.muted{color:#666;font-size:.9em;}
</style></head><body>
<h1>tiny-reader</h1>
<div id=books class=muted>loading...</div>
<form id=up onsubmit="upload(event)">
<h3>upload .epub</h3>
<input type=file name=file accept=".epub" required/>
<button>upload</button>
<div id=status class=muted></div>
</form>
<script>
 var load=function(){
  fetch('/api/books').then(function(r){return r.json();}).then(function(list){
   var d=document.getElementById('books');
   d.innerHTML=list.length? list.map(function(b){return '<div class=book><span class=book-name>'+b.name+'</span><span class=book-size>'+(b.size/1024).toFixed(0)+'KB</span><a href="/api/books/'+encodeURIComponent(b.name)+'" download>download</a> <button class=danger onclick="del(\''+b.name+'\')">delete</button></div>';}).join('') : '<p class=muted>(empty)</p>';
  });
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
 load();
</script></body></html>
)HTML";

static void ap_handle_root() {
    ap_last_activity = millis();
    web_server->send_P(200, "text/html", AP_INDEX_HTML);
}
static void ap_handle_books_json() {
    ap_last_activity = millis();
    web_server->send(200, "application/json", ap_build_books_json());
}
static void ap_handle_upload_progress() {
    ap_last_activity = millis();
    HTTPUpload &u = web_server->upload();
    if (u.status == UPLOAD_FILE_START) {
        String path = String("/") + u.filename;
        if (SD.exists(path)) SD.remove(path);
        ap_upload_file = SD.open(path, FILE_WRITE);
        Serial.printf("[UPLOAD] start %s\n", path.c_str());
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (ap_upload_file) ap_upload_file.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (ap_upload_file) {
            ap_upload_file.close();
            Serial.printf("[UPLOAD] done %u bytes\n", (unsigned)u.totalSize);
        }
    }
}
static void ap_handle_upload_done() {
    web_server->send(200, "application/json", "{\"ok\":true}");
}
static void ap_handle_book_path() {
    ap_last_activity = millis();
    String uri = web_server->uri();
    if (!uri.startsWith("/api/books/")) {
        web_server->send(404, "text/plain", "not found");
        return;
    }
    String name = uri.substring(strlen("/api/books/"));
    name = web_server->urlDecode(name);
    String path = String("/") + name;
    if (web_server->method() == HTTP_DELETE) {
        if (SD.exists(path)) {
            SD.remove(path);
            // also remove sidecar .pos
            int dot = name.lastIndexOf('.');
            if (dot > 0) {
                String pos_path = String("/") + name.substring(0, dot) + ".pos";
                if (SD.exists(pos_path)) SD.remove(pos_path);
            }
            web_server->send(200, "application/json", "{\"ok\":true}");
        } else {
            web_server->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    } else {
        File f = SD.open(path, FILE_READ);
        if (!f) {
            web_server->send(404, "application/json", "{\"error\":\"not found\"}");
            return;
        }
        web_server->streamFile(f, "application/epub+zip");
        f.close();
    }
}

static void ap_handle_not_found() {
    String uri = web_server->uri();
    if (uri.startsWith("/api/books/")) { ap_handle_book_path(); return; }
    // Captive-portal probes: redirect everything to our index so the phone
    // recognises this as a portal and doesn't switch back to mobile data.
    Serial.printf("[HTTP] catchall %s\n", uri.c_str());
    web_server->sendHeader("Location", "/", true);
    web_server->send(302, "text/plain", "");
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

    WiFi.onEvent(on_wifi_event);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());
    delay(200);

    IPAddress ip = WiFi.softAPIP();

    // Captive-portal DNS: every domain resolves to our IP so phones treat us
    // as a portal and stay connected.
    dns_server = new DNSServer();
    dns_server->setErrorReplyCode(DNSReplyCode::NoError);
    dns_server->start(53, "*", ip);

    if (!MDNS.begin(MDNS_HOSTNAME)) Serial.println("[AP] mDNS begin failed");
    MDNS.addService("http", "tcp", 80);

    web_server = new WebServer(80);
    web_server->on("/", HTTP_GET, ap_handle_root);
    web_server->on("/api/books", HTTP_GET, ap_handle_books_json);
    web_server->on("/upload", HTTP_POST,
                   ap_handle_upload_done, ap_handle_upload_progress);
    // Captive-portal probes from Apple/Android/Microsoft
    web_server->on("/generate_204", HTTP_GET, ap_handle_root);
    web_server->on("/hotspot-detect.html", HTTP_GET, ap_handle_root);
    web_server->on("/connecttest.txt", HTTP_GET, ap_handle_root);
    web_server->onNotFound(ap_handle_not_found);
    web_server->begin();

    ap_active = true;
    ap_last_activity = millis();
    ap_started_at = millis();
    Serial.printf("[AP] up: SSID=%s pw=%s ip=%s\n",
                  ap_ssid.c_str(), ap_password.c_str(),
                  WiFi.softAPIP().toString().c_str());

    render_ap_screen();
}

static void exit_ap_mode() {
    if (!ap_active) return;
    Serial.println("[AP] exit_ap_mode() called");
    if (web_server) {
        web_server->stop();
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
    delay(200);
    Serial.println("\ntiny-reader starting");

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

    pinMode(BUTTON_1, INPUT_PULLUP);

    scan_sd_for_epubs();
    clear_and_flush();
    render_book_list();
}

// returns 0/1/2/3 = none/left/center/right ; (or -1 on no touch)
static int classify_tap(int16_t x, int16_t y) {
    if (x < EPD_WIDTH / 4) return 1;            // left third
    if (x > 3 * EPD_WIDTH / 4) return 3;        // right third
    return 2;                                   // center
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
            } else if (cmd == "help") {
                Serial.println("commands: next prev back open <n> goto <ch> dump share stop_share help");
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
        web_server->handleClient();
        if (now - ap_last_activity > AP_IDLE_TIMEOUT_MS) {
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
                    if (xs[0] < EPD_WIDTH / 2) book_prev_page();
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

    // Button1 (GPIO21): short press = back-to-library / next-selection;
    // long press (≥ 2 s) = enter share/AP mode.
    bool btn_down = (digitalRead(BUTTON_1) == LOW);
    if (btn_down) {
        if (button_press_start == 0) {
            button_press_start = now;
            button_long_handled = false;
        } else if (!button_long_handled && now - button_press_start >= 2000) {
            button_long_handled = true;
            Serial.println("[BTN] long press → AP mode");
            enter_ap_mode();
            input_cooldown_until = now + 1000;
        }
    } else {
        if (button_press_start && !button_long_handled &&
            now >= input_cooldown_until) {
            // released as a short press
            if (app_mode == MODE_BOOK) {
                app_mode = MODE_LIBRARY;
                render_book_list();
            } else {
                move_selection(+1);
            }
            input_cooldown_until = now + 600;
        }
        button_press_start = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}
