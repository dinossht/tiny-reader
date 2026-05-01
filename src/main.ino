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
#include "firasans_small.h"          // smaller bitmap font for footer/status lines
#include "freesans_body_small.h"     // 18pt body font used for "compact" density
#include "freesans_body.h"           // 22pt body font (regular) — medium density
#include "freesans_body_bold.h"      // 22pt body bold
#include "freesans_body_italic.h"    // 22pt body italic (oblique)
#include "freesans_body_bolditalic.h" // 22pt body bold-italic
#include "freesans_title.h"          // 32pt title font for screen headers
#include "logo.h"                    // 1-bit packed book-sparkle logo
#include "todo_logo.h"               // 1-bit packed notepad-checkmark logo
#include "qr_wifi.h"                 // pre-generated WIFI:tiny-reader/bv-birdy QR
extern "C" {
#include "tjpgd.h"                   // tiny JPEG decoder (grayscale output)
}
#include "hub_html.h"                // generated from src/hub.html (HUB_HTML[])
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
#include <ArduinoJson.h>

// ---- globals ----
static uint8_t *framebuffer = nullptr;
// Number of clear cycles per refresh. epdiy default is 4 (~1.35 s clear
// for full screen on this panel). 1 cycle drops it to ~315 ms with no
// visible ghost on the ED047TC1 — verified empirically across 5–10
// pages of dense text. Configurable via serial commands c1/c2/c3/c4.
static int g_clear_cycles = 1;
static TouchDrvGT911 touch;
static bool touchOnline = false;

// Background touch poller. Runs on core 0 every ~15 ms so taps are captured
// even while loop() is blocked inside an EPD waveform (~900 ms per page
// turn). Each press-edge (no-touch → touch) is enqueued as one TouchTap;
// loop() drains the queue. Touch I2C is on its own bus, separate from the
// EPD parallel bus, so the two run independently.
struct TouchTap {
    int16_t x;
    int16_t y;
    uint32_t t_ms;
};
static QueueHandle_t g_touch_queue = nullptr;

enum AppMode { MODE_LIBRARY, MODE_BOOK, MODE_TODO, MODE_CHAPTER_JUMP,
               MODE_BOOK_END, MODE_BOOKMARK_LIST };
static AppMode app_mode = MODE_LIBRARY;

// library mode
static std::vector<std::string> books;   // filenames in SD root ending .epub
static int selected = 0;                 // index in books
static int page_first = 0;               // first index visible on the screen
static const int LINES_PER_PAGE = 5;     // 64-px rows fit 5 above the corner-icon button
static const int LINE_HEIGHT = 56;
static const int LIST_X = 60;
static const int LIST_Y = 80;

// book mode
static std::string current_book_path;
static int current_spine = 0;
static int total_spine = 0;
static int current_page_in_chapter = 0;
static std::vector<std::string> chapter_pages;  // each entry = one page of \n-separated lines
// ~8% symmetric page margin — generous enough that italic-glyph overhang
// (the slanted top of the last glyph on a justified line) doesn't poke
// past the right edge, no asymmetric reserve needed.
static const int BOOK_MARGIN_X = 80;
static const int BODY_RIGHT_RESERVE = 0;
// First line of each paragraph is rendered with a leading indent. Both
// wrap_text and render_book_page_text honor it — wrap so the first line
// fits the indented body width, render so the first line starts at
// x_start + PARAGRAPH_INDENT. If they disagree the first line of every
// paragraph protrudes into the right margin.
static const int PARAGRAPH_INDENT = 40;
static const int BOOK_TOP_RESERVE = 30;     // small visual margin only
static const int BOOK_FOOTER_RESERVE = 50;
// Body font and layout values driven by the user's "density" setting (see
// g_settings + apply_density()).
static const GFXfont *g_body_font = (const GFXfont *)&FiraSans;
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
        case 0:  // compact — uses an 18pt FreeSans body so a lot more text
                 // fits on a page; line height matches the smaller font.
            g_body_font = (const GFXfont *)&freesans_body_small;
            book_line_height = 42;
            book_chars_per_line = 70;
            break;
        case 2:  // loose — same body font as medium but airy: extra leading
                 // and short lines.
            g_body_font = (const GFXfont *)&freesans_body;
            book_line_height = 72;
            book_chars_per_line = 30;
            break;
        case 1:  // medium (default)
        default:
            g_settings.density = 1;
            g_body_font = (const GFXfont *)&freesans_body;
            book_line_height = 50;
            book_chars_per_line = 44;
            break;
    }
    // Lines per page = (max baseline range) / line_height + 1.
    // First line's baseline sits at BOOK_TOP_RESERVE+30 (matches the y0
    // passed to render_book_page_text). Last line's baseline must leave
    // ~10 px of descender slack above the footer reserve. The previous
    // formula treated the available area as N×line_height bands and
    // truncated, which lost a usable line in compact mode.
    {
        const int first_baseline = BOOK_TOP_RESERVE + 30;
        const int last_baseline_max =
            EPD_HEIGHT - BOOK_FOOTER_RESERVE - 10;
        book_lines_per_page =
            (last_baseline_max - first_baseline) / book_line_height + 1;
    }
    Serial.printf("[SETTINGS] density=%d -> line_h=%d lines/page=%d chars/line=%d\n",
                  g_settings.density, book_line_height,
                  book_lines_per_page, book_chars_per_line);
}

// ---- helpers ----

// LilyGo V2.3: BATT_PIN = GPIO14, on a /2 voltage divider. ADC1.
// Average 8 samples to smooth out the ESP32-S3 SAR-ADC's per-sample noise
// (typically ±10 mV at this attenuation), which is otherwise enough to
// flicker the percent reading by a couple of points.
static float read_battery_voltage() {
    long sum = 0;
    for (int i = 0; i < 8; ++i) sum += analogRead(BATT_PIN);
    return (sum / 8.0f / 4095.0f) * 2.0f * 3.3f;
}

// LiPo discharge curve (typical 1S cell at light load, ~250 mA draw).
// The cell holds ~4.0–4.2 V for the first ~30 % of capacity, then sits
// near 3.7–3.8 V for the bulk, then drops rapidly below 3.6 V — so the
// old linear 3.3–4.2 V mapping was very wrong in the middle of the curve.
// Pairs of {volts, percent} sorted descending. Linear-interpolated.
static int read_battery_percent() {
    float v = read_battery_voltage();
    static const float CURVE[][2] = {
        {4.20f, 100}, {4.15f, 95}, {4.10f, 90}, {4.05f, 85},
        {4.00f, 80},  {3.95f, 73}, {3.90f, 65}, {3.85f, 58},
        {3.80f, 50},  {3.75f, 42}, {3.70f, 33}, {3.65f, 23},
        {3.60f, 14},  {3.55f, 8},  {3.50f, 5},  {3.40f, 2},
        {3.30f, 0},
    };
    if (v >= CURVE[0][0]) return 100;
    int last = sizeof(CURVE)/sizeof(CURVE[0]) - 1;
    if (v <= CURVE[last][0]) return 0;
    for (int i = 0; i < last; ++i) {
        float v_hi = CURVE[i][0], v_lo = CURVE[i+1][0];
        if (v <= v_hi && v >= v_lo) {
            float p_hi = CURVE[i][1], p_lo = CURVE[i+1][1];
            float t = (v - v_lo) / (v_hi - v_lo);
            return (int)(p_lo + t * (p_hi - p_lo) + 0.5f);
        }
    }
    return 0;
}

// Charging detection — voltage trend over a ~60 s window. The CHRG/STDBY
// pins of the HX6610S charge IC are wired to LEDs only on V2.3, not to
// any ESP32 GPIO, so we infer charging from the cell voltage going up.
// Returns true when voltage has risen by ≥ 5 mV over the last 30 s, or
// when it's pinned at the charger's float voltage (≥ 4.18 V).
static float g_batt_trend_v_prev = 0.0f;
static uint32_t g_batt_trend_ts_prev = 0;
static bool g_batt_charging = false;
static void update_charging_state() {
    float v = read_battery_voltage();
    uint32_t now = millis();
    if (g_batt_trend_ts_prev == 0) {
        g_batt_trend_v_prev = v;
        g_batt_trend_ts_prev = now;
        return;
    }
    uint32_t dt_ms = now - g_batt_trend_ts_prev;
    if (dt_ms < 30000) return;   // sample only every 30 s
    float dv = v - g_batt_trend_v_prev;
    bool was_charging = g_batt_charging;
    if (v >= 4.18f) g_batt_charging = true;          // float-charge
    else if (dv >= 0.005f) g_batt_charging = true;   // visible rise
    else if (dv <= -0.002f) g_batt_charging = false; // clear discharge
    if (was_charging != g_batt_charging) {
        Serial.printf("[BAT] charging=%d v=%.3f dv=%+.3f\n",
                      (int)g_batt_charging, v, dv);
    }
    g_batt_trend_v_prev = v;
    g_batt_trend_ts_prev = now;
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

// ---- TODO list (backlog #11): hub-managed notes/todo persisted on SD. ----
// Schema on disk (/todos.json):
//   {"items":[{"text":"buy milk","done":false}, ...]}
// Caps: TODOS_MAX_ITEMS items, TODOS_MAX_TEXT bytes per item.
struct TodoItem { std::string text; bool done; };
static std::vector<TodoItem> g_todos;
static const size_t TODOS_MAX_ITEMS = 50;
static const size_t TODOS_MAX_TEXT  = 200;

static void load_todos() {
    g_todos.clear();
    File f = SD.open("/todos.json", FILE_READ);
    if (!f) {
        Serial.println("[TODOS] no /todos.json — starting empty");
        return;
    }
    // ArduinoJson v7 uses elastic JsonDocument; size to file length + slack.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[TODOS] parse failed: %s — starting empty\n", err.c_str());
        return;
    }
    JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    for (JsonVariantConst v : items) {
        if (g_todos.size() >= TODOS_MAX_ITEMS) break;
        const char *t = v["text"] | "";
        bool d = v["done"] | false;
        if (!t || !*t) continue;
        TodoItem it;
        it.text.assign(t, strnlen(t, TODOS_MAX_TEXT));
        it.done = d;
        g_todos.push_back(std::move(it));
    }
    Serial.printf("[TODOS] loaded %u item(s)\n", (unsigned)g_todos.size());
}

static bool save_todos() {
    JsonDocument doc;
    JsonArray items = doc["items"].to<JsonArray>();
    for (const TodoItem &it : g_todos) {
        JsonObject o = items.add<JsonObject>();
        o["text"] = it.text;
        o["done"] = it.done;
    }
    File f = SD.open("/todos.json", FILE_WRITE);
    if (!f) { Serial.println("[TODOS] save failed"); return false; }
    size_t n = serializeJson(doc, f);
    f.close();
    Serial.printf("[TODOS] saved %u item(s), %u bytes\n",
                  (unsigned)g_todos.size(), (unsigned)n);
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
// Append a Unicode code point to a string as UTF-8 bytes.
static void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) { out.push_back((char)cp); return; }
    if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    // 4-byte UTF-8 (rare for body text)
    out.push_back((char)(0xF0 | (cp >> 18)));
    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
}

static std::string decode_entities(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 9) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                // Named entities. Smart quotes returned as proper UTF-8
                // U+2018/2019/201C/201D so the new General-Punctuation
                // glyphs in freesans_body render them.
                const char *rep = nullptr;
                if      (ent == "amp")    rep = "&";
                else if (ent == "lt")     rep = "<";
                else if (ent == "gt")     rep = ">";
                else if (ent == "quot")   rep = "\"";
                else if (ent == "apos")   rep = "'";
                else if (ent == "nbsp")   rep = " ";
                else if (ent == "mdash")  rep = "\xE2\x80\x94"; // —
                else if (ent == "ndash")  rep = "\xE2\x80\x93"; // –
                else if (ent == "hellip") rep = "\xE2\x80\xA6"; // …
                else if (ent == "lsquo")  rep = "\xE2\x80\x98"; // '
                else if (ent == "rsquo")  rep = "\xE2\x80\x99"; // '
                else if (ent == "ldquo")  rep = "\xE2\x80\x9C"; // "
                else if (ent == "rdquo")  rep = "\xE2\x80\x9D"; // "
                if (rep) { out += rep; i = semi + 1; continue; }
                // Numeric &#NN; or &#xHH;
                if (!ent.empty() && ent[0] == '#') {
                    uint32_t cp = 0;
                    bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                    size_t k = hex ? 2 : 1;
                    bool ok = (k < ent.size());
                    for (; k < ent.size() && ok; ++k) {
                        char c = ent[k];
                        if (c >= '0' && c <= '9') cp = cp * (hex ? 16 : 10) + (c - '0');
                        else if (hex && c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
                        else if (hex && c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
                        else { ok = false; }
                    }
                    if (ok && cp > 0) {
                        append_utf8(out, cp);
                        i = semi + 1;
                        continue;
                    }
                    // Couldn't parse — drop.
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    // ASCII-style typographic substitutions + collapse Unicode whitespace
    // (NBSP, thin/hair space, narrow no-break space) to ASCII space so that
    // word-tokenizers downstream split correctly. Without this, an EPUB
    // that used `&#160;` between words inside `<em>...</em>` ends up
    // rendered as one merged blob.
    std::string t;
    t.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        // Multi-byte Unicode space sequences in UTF-8.
        if (i + 1 < out.size() &&
            (uint8_t)out[i] == 0xC2 && (uint8_t)out[i + 1] == 0xA0) {
            // U+00A0 NBSP
            t.push_back(' ');
            i += 1;
            continue;
        }
        if (i + 2 < out.size() && (uint8_t)out[i] == 0xE2 &&
            (uint8_t)out[i + 1] == 0x80) {
            uint8_t b2 = (uint8_t)out[i + 2];
            // U+2002..U+200A and U+202F whitespace forms
            if ((b2 >= 0x82 && b2 <= 0x8A) || b2 == 0xAF) {
                t.push_back(' ');
                i += 2;
                continue;
            }
        }
        if (i + 1 < out.size() && out[i] == '-' && out[i + 1] == '-') {
            t += "\xE2\x80\x94"; // —
            i += 1;
        } else if (i + 2 < out.size() && out[i] == '.' && out[i + 1] == '.' &&
                   out[i + 2] == '.') {
            t += "\xE2\x80\xA6"; // …
            i += 2;
        } else {
            t.push_back(out[i]);
        }
    }
    return t;
}

// Strip XHTML tags into plain text. Block-level closing tags become \n.
// Skip everything inside <head>, <script>, <style>.
// Inline style markers used in the stripped text. These are ASCII control
// bytes that pass through wrap/paginate untouched and are interpreted by
// render_line to swap fonts mid-line. Zero-width in width calculations.
static const char STY_BOLD_ON    = '\x01';
static const char STY_BOLD_OFF   = '\x02';
static const char STY_ITALIC_ON  = '\x03';
static const char STY_ITALIC_OFF = '\x04';
static inline bool is_style_marker(char c) {
    return c == STY_BOLD_ON || c == STY_BOLD_OFF ||
           c == STY_ITALIC_ON || c == STY_ITALIC_OFF;
}

// Forward decl — defined alongside the renderer; used by wrap_text's
// measure() so wrap and render see exactly the same word widths.
static int draw_or_measure_styled_word(const GFXfont *base_font,
                                       const char *word, int len,
                                       bool &bold, bool &italic,
                                       int32_t x, int32_t y, uint8_t *fb);

static std::string strip_xhtml(const char *src, size_t len) {
    std::string out;
    out.reserve(len);
    bool in_tag = false;
    int skip_depth = 0;     // >0 while inside head/script/style
    int bold_depth = 0;     // nested <b>/<strong>
    int italic_depth = 0;   // nested <em>/<i>
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
            } else if (skip_depth == 0 && (name == "b" || name == "strong")) {
                if (is_close) {
                    if (bold_depth > 0) --bold_depth;
                    if (bold_depth == 0) out.push_back(STY_BOLD_OFF);
                } else {
                    if (bold_depth == 0) out.push_back(STY_BOLD_ON);
                    ++bold_depth;
                }
            } else if (skip_depth == 0 && (name == "em" || name == "i")) {
                if (is_close) {
                    if (italic_depth > 0) --italic_depth;
                    if (italic_depth == 0) out.push_back(STY_ITALIC_OFF);
                } else {
                    if (italic_depth == 0) out.push_back(STY_ITALIC_ON);
                    ++italic_depth;
                }
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

// Algorithmic hyphenation (vowel-consonant heuristic, KOReader-style).
// No data file, ~50 lines. Returns a position `pos` in [2, len-2] such that
// `word[0..pos] + "-"` is a reasonable break point, or -1 if no good break.
// The latest valid break <= max_pos wins so we fit as much as possible on
// the current line.
static inline bool _ah_is_vowel(char c) {
    c = (char)tolower((unsigned char)c);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}
static inline bool _ah_is_letter(char c) {
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'z';
}
static int find_hyphen_break(const std::string &word, int max_pos) {
    int len = (int)word.size();
    if (len < 6) return -1;
    if (max_pos > len - 2) max_pos = len - 2;
    if (max_pos < 2) return -1;
    for (int pos = max_pos; pos >= 2; --pos) {
        char a = (char)tolower((unsigned char)word[pos - 1]);
        char b = (char)tolower((unsigned char)word[pos]);
        if (!_ah_is_letter(a) || !_ah_is_letter(b)) continue;
        // Don't split common digraphs / clusters.
        if ((a == 's' || a == 't' || a == 'p' || a == 'c' ||
             a == 'g' || a == 'w') && b == 'h') continue;
        if (a == 'c' && b == 'k') continue;
        if (a == 'n' && b == 'g') continue;
        if (a == 'q' && b == 'u') continue;
        bool va = _ah_is_vowel(a), vb = _ah_is_vowel(b);
        bool ca = !va, cb = !vb;
        // Accept V|C ("compu-ter"), C|V ("feel-ings"), or C|C ("but-ter").
        if ((va && cb) || (ca && vb) || (ca && cb)) {
            return pos;
        }
    }
    return -1;
}

// Pixel-width word wrap — measures actual rendered width with the body font
// so wide chars like M/W don't push lines past the right edge. Preserves
// paragraph breaks (consecutive \n becomes a blank line in the output).
static std::vector<std::string> wrap_text(const std::string &text) {
    std::vector<std::string> lines;
    const int target_w = EPD_WIDTH - 2 * BOOK_MARGIN_X - BODY_RIGHT_RESERVE;
    const GFXfont *font = g_body_font;

    auto measure = [font](const char *s) -> int {
        // Use the same styled walker as render_line so wrap and render
        // agree byte-for-byte. Style markers are zero-width directives;
        // each non-marker segment is measured with its style-aware font
        // (italic glyphs are slightly wider than regular, etc.). Returns
        // the cursor advance — what writeln will actually skip.
        bool bold = false, italic = false;
        return draw_or_measure_styled_word(
            font, s, (int)strlen(s), bold, italic, 0, 0, nullptr);
    };

    std::string para;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string current;
            std::string word;
            // First line of each paragraph is indented at render time, so
            // it must fit a narrower width (target_w - PARAGRAPH_INDENT).
            // After the first line is committed, subsequent lines wrap to
            // the full target_w.
            bool first_line = true;
            auto line_max = [&]() {
                return first_line ? (target_w - PARAGRAPH_INDENT) : target_w;
            };
            for (size_t j = 0; j <= para.size(); ++j) {
                char c = (j < para.size()) ? para[j] : ' ';
                if (c == ' ') {
                    if (!word.empty()) {
                        if (current.empty()) {
                            current = word;
                        } else {
                            std::string candidate = current + " " + word;
                            if (measure(candidate.c_str()) <= line_max()) {
                                current = std::move(candidate);
                            } else {
                                // Word doesn't fit. Try to hyphenate: find
                                // the largest prefix of `word` such that
                                // `current + " " + prefix + "-"` fits.
                                int avail = line_max()
                                          - measure((current + " ").c_str());
                                int best_pos = -1;
                                if (avail > 0 && (int)word.size() >= 6) {
                                    // Walk from longest prefix down.
                                    for (int pos = (int)word.size() - 2;
                                         pos >= 2; --pos) {
                                        int hpos = find_hyphen_break(word, pos);
                                        if (hpos < 0) break;
                                        std::string prefix
                                            = word.substr(0, hpos) + "-";
                                        if (measure(prefix.c_str()) <= avail) {
                                            best_pos = hpos;
                                            break;
                                        }
                                        pos = hpos;  // try next-shorter break
                                    }
                                }
                                if (best_pos > 0) {
                                    std::string prefix
                                        = word.substr(0, best_pos) + "-";
                                    lines.push_back(current + " " + prefix);
                                    current = word.substr(best_pos);
                                } else {
                                    lines.push_back(current);
                                    current = word;
                                }
                                first_line = false;
                            }
                        }
                        word.clear();
                    }
                } else {
                    word.push_back(c);
                }
            }
            if (!current.empty()) lines.push_back(current);
            if (i < text.size()) lines.push_back("");   // paragraph separator
            para.clear();
        } else {
            para.push_back(text[i]);
        }
    }
    return lines;
}

// Group lines into pages.
static std::vector<std::string> paginate_lines(
        const std::vector<std::string> &lines,
        const std::vector<std::string> *break_titles = nullptr) {
    std::vector<std::string> pages;
    std::string page;
    int n_in_page = 0;
    for (const auto &line : lines) {
        // Force a page break before any line that begins a known TOC title
        // — used for single-spine Gutenberg books so chapter titles always
        // land at the top of a fresh page. (line is the wrap-output, may
        // be only the first wrapped piece of the title, hence prefix-match.)
        bool chapter_break = false;
        if (break_titles && n_in_page > 0 && line.size() >= 8) {
            for (const auto &t : *break_titles) {
                if (t.size() >= line.size() &&
                    t.compare(0, line.size(), line) == 0) {
                    chapter_break = true;
                    break;
                }
            }
        }
        if (n_in_page >= book_lines_per_page || chapter_break) {
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
struct TocCacheEntry { int spine_index; std::string title; };
struct ChapterLoadResult {
    volatile bool done;
    bool ok;
    int spine_n;
    std::string title;
    std::string path;
    int spine_to_load;
    std::vector<std::string> pages;
    std::vector<TocCacheEntry> toc;   // spine_index -> chapter title (NCX TOC)
};

// ---- Cover thumbnail cache ------------------------------------------------
// Each book gets a tiny grayscale thumbnail (4-bit packed) extracted from
// its EPUB cover image once, cached on SD as "<bookname>.thumb". The
// library row blits it next to the title. No decode at render time.
static const int THUMB_W = 80;
static const int THUMB_H = 110;
static const size_t THUMB_BYTES = (size_t)THUMB_W * THUMB_H / 2;

static String thumb_path_for_name(const String &name) {
    int dot = name.lastIndexOf('.');
    if (dot <= 0) dot = name.length();
    return String("/") + name.substring(0, dot) + ".thumb";
}

struct TjpgCtx {
    const uint8_t *jpeg;
    size_t jpeg_size;
    size_t jpeg_pos;
    uint8_t *gray;
    int gray_w, gray_h;
};

static size_t tjpg_input_cb(JDEC *jd, uint8_t *buf, size_t sz) {
    TjpgCtx *ctx = (TjpgCtx *)jd->device;
    size_t avail = ctx->jpeg_size - ctx->jpeg_pos;
    if (sz > avail) sz = avail;
    if (buf) memcpy(buf, ctx->jpeg + ctx->jpeg_pos, sz);
    ctx->jpeg_pos += sz;
    return sz;
}

static int tjpg_output_cb(JDEC *jd, void *bitmap, JRECT *rect) {
    TjpgCtx *ctx = (TjpgCtx *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; ++y) {
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x >= 0 && x < ctx->gray_w && y >= 0 && y < ctx->gray_h) {
                ctx->gray[y * ctx->gray_w + x] = *src;
            }
            ++src;
        }
    }
    return 1;
}

static bool decode_cover_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                              uint8_t *thumb_4bit) {
    JDEC jd;
    // 3100 is tjpgd's documented minimum for plain baseline JPEGs but real
    // EPUB covers (especially progressive or larger) need much more.
    // atomic14's reader uses 64 KB; we have 8 MB PSRAM, so use the same.
    const size_t WORKBUF_SIZE = 64 * 1024;
    void *workbuf = ps_malloc(WORKBUF_SIZE);
    if (!workbuf) return false;
    TjpgCtx ctx = {jpeg, jpeg_size, 0, nullptr, 0, 0};
    JRESULT r = jd_prepare(&jd, tjpg_input_cb, workbuf, WORKBUF_SIZE, &ctx);
    if (r != JDR_OK) {
        Serial.printf("[THUMB] jd_prepare failed: %d\n", r);
        free(workbuf);
        return false;
    }
    int scale = 0;
    while (scale < 3 &&
           ((jd.width >> scale) > 256 || (jd.height >> scale) > 384))
        ++scale;
    int gw = jd.width >> scale;
    int gh = jd.height >> scale;
    if (gw <= 0 || gh <= 0) { free(workbuf); return false; }
    ctx.gray = (uint8_t *)ps_malloc((size_t)gw * gh);
    if (!ctx.gray) { free(workbuf); return false; }
    ctx.gray_w = gw; ctx.gray_h = gh;

    r = jd_decomp(&jd, tjpg_output_cb, scale);
    free(workbuf);
    if (r != JDR_OK) {
        Serial.printf("[THUMB] jd_decomp failed: %d\n", r);
        free(ctx.gray);
        return false;
    }
    memset(thumb_4bit, 0xFF, THUMB_BYTES);
    for (int y = 0; y < THUMB_H; ++y) {
        int sy = y * gh / THUMB_H;
        for (int x = 0; x < THUMB_W; ++x) {
            int sx = x * gw / THUMB_W;
            uint8_t g = ctx.gray[sy * gw + sx];
            uint8_t nib = g >> 4;
            int idx = (y * THUMB_W + x) / 2;
            if (x & 1) thumb_4bit[idx] = (thumb_4bit[idx] & 0x0F) | (nib << 4);
            else       thumb_4bit[idx] = (thumb_4bit[idx] & 0xF0) | nib;
        }
    }
    free(ctx.gray);
    return true;
}

static void generate_thumb_if_missing(Epub &epub, const std::string &book_path) {
    size_t slash = book_path.find_last_of('/');
    std::string fname = (slash != std::string::npos)
                        ? book_path.substr(slash + 1) : book_path;
    String thumb_path = thumb_path_for_name(String(fname.c_str()));
    if (SD.exists(thumb_path)) {
        File chk = SD.open(thumb_path, FILE_READ);
        long sz = chk ? chk.size() : 0;
        if (chk) chk.close();
        if (sz == (long)THUMB_BYTES) {
            Serial.printf("[THUMB] %s already cached\n", thumb_path.c_str());
            return;
        }
        // Stale cache from an older thumb size — wipe and regenerate.
        Serial.printf("[THUMB] %s stale (%ld vs %u), regenerating\n",
                      thumb_path.c_str(), sz, (unsigned)THUMB_BYTES);
        SD.remove(thumb_path);
    }
    const std::string &cover_href = epub.get_cover_image_item();
    if (cover_href.empty()) {
        Serial.printf("[THUMB] %s: no cover in manifest\n", fname.c_str());
        return;
    }
    Serial.printf("[THUMB] %s: cover='%s'\n", fname.c_str(), cover_href.c_str());
    size_t size = 0;
    uint8_t *data = epub.get_item_contents(cover_href, &size);
    if (!data || size == 0) {
        if (data) free(data);
        Serial.printf("[THUMB] %s: read cover bytes failed\n", fname.c_str());
        return;
    }
    Serial.printf("[THUMB] %s: cover %u bytes, decoding...\n",
                  fname.c_str(), (unsigned)size);
    uint8_t *thumb = (uint8_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_8BIT);
    if (!thumb) { free(data); return; }
    bool ok = decode_cover_jpeg(data, size, thumb);
    free(data);
    if (ok) {
        File f = SD.open(thumb_path, FILE_WRITE);
        if (f) {
            f.write(thumb, THUMB_BYTES);
            f.close();
            Serial.printf("[THUMB] cached %s\n", thumb_path.c_str());
        } else {
            Serial.printf("[THUMB] %s: SD write failed\n", thumb_path.c_str());
        }
    } else {
        Serial.printf("[THUMB] %s: decode failed (likely PNG cover or "
                      "unsupported JPEG variant)\n", fname.c_str());
    }
    free(thumb);
}

static void chapter_load_task(void *param) {
    ChapterLoadResult *r = (ChapterLoadResult *)param;
    Epub epub(r->path);
    r->ok = epub.load();
    if (r->ok) {
        r->spine_n = epub.get_spine_items_count();
        r->title = epub.get_title();
        if (r->title.empty()) {
            // EPUB had no <dc:title>; use the filename (sans .epub) so the
            // UI has something to show. r->path is "/Foo Bar.epub".
            std::string p = r->path;
            size_t slash = p.find_last_of('/');
            std::string base = (slash != std::string::npos)
                               ? p.substr(slash + 1) : p;
            if (base.size() >= 5 &&
                base.compare(base.size() - 5, 5, ".epub") == 0) {
                base.resize(base.size() - 5);
            }
            r->title = base;
        }
        // Capture the NCX TOC mapping spine -> chapter title.
        int ntoc = epub.get_toc_items_count();
        r->toc.reserve(ntoc);
        for (int i = 0; i < ntoc; ++i) {
            EpubTocEntry &e = epub.get_toc_item(i);
            int sp = epub.get_spine_index_for_toc_index(i);
            if (sp >= 0) r->toc.push_back({sp, e.title});
        }
        if (r->spine_to_load >= 0 && r->spine_to_load < r->spine_n) {
            std::string href = epub.get_spine_item(r->spine_to_load);
            size_t size = 0;
            uint8_t *data = epub.get_item_contents(href, &size);
            if (data && size > 0) {
                std::string text = strip_xhtml((const char *)data, size);
                free(data);
                auto lines = wrap_text(text);
                // Collect titles from the TOC that point at this spine —
                // when many TOC entries share one spine (Gutenberg) we use
                // them as page-break hints so each chapter starts at top.
                std::vector<std::string> break_titles;
                for (const auto &e : r->toc) {
                    if (e.spine_index == r->spine_to_load)
                        break_titles.push_back(e.title);
                }
                r->pages = paginate_lines(lines, break_titles.empty() ? nullptr
                                                                      : &break_titles);
                Serial.printf("chapter %d: %u pages, %u lines, %u bytes (toc=%u)\n",
                              r->spine_to_load, (unsigned)r->pages.size(),
                              (unsigned)lines.size(), (unsigned)text.size(),
                              (unsigned)r->toc.size());
            } else {
                Serial.printf("chapter %d: get_item_contents failed (size=%u)\n",
                              r->spine_to_load, (unsigned)size);
            }
        }
        // Cache the cover thumbnail on first load (skipped if already cached).
        generate_thumb_if_missing(epub, r->path);
    }
    r->done = true;
    vTaskDelete(NULL);
}

// Cached after the first chapter load, used to label spine items by their
// real TOC chapter title (e.g. "Chapter Three" instead of "Sec 8/19").
static std::vector<TocCacheEntry> g_toc_cache;

// Find the latest TOC entry whose spine_index <= cur. Returns "" if none.
static std::string get_toc_label_for(int cur_spine) {
    std::string best;
    int best_si = -1;
    for (const auto &e : g_toc_cache) {
        if (e.spine_index <= cur_spine && e.spine_index > best_si) {
            best = e.title;
            best_si = e.spine_index;
        }
    }
    return best;
}


// SD path "/<book-without-extension>.pos" for any book by filename.
static String pos_path_for_name(const String &name) {
    int dot = name.lastIndexOf('.');
    if (dot <= 0) dot = name.length();
    return String("/") + name.substring(0, dot) + ".pos";
}

// SD path "/<book-without-extension>.bm" — one bookmark per line as
// "spine page". Read into g_bookmarks at book-open, written on toggle.
static String bookmark_path_for_name(const String &name) {
    int dot = name.lastIndexOf('.');
    if (dot <= 0) dot = name.length();
    return String("/") + name.substring(0, dot) + ".bm";
}

static std::vector<std::pair<int,int>> g_bookmarks;

static void load_bookmarks_for_current() {
    g_bookmarks.clear();
    if (selected < 0 || selected >= (int)books.size()) return;
    File f = SD.open(bookmark_path_for_name(String(books[selected].c_str())),
                     FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        int sp = -1, pg = -1;
        if (sscanf(line.c_str(), "%d %d", &sp, &pg) == 2 && sp >= 0 && pg >= 0) {
            g_bookmarks.push_back({sp, pg});
        }
    }
    f.close();
    Serial.printf("[BM] loaded %u bookmark(s)\n", (unsigned)g_bookmarks.size());
}

static void save_bookmarks_for_current() {
    if (selected < 0 || selected >= (int)books.size()) return;
    String path = bookmark_path_for_name(String(books[selected].c_str()));
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[BM] open %s failed\n", path.c_str()); return; }
    for (const auto &b : g_bookmarks) f.printf("%d %d\n", b.first, b.second);
    f.close();
    Serial.printf("[BM] saved %u bookmark(s)\n", (unsigned)g_bookmarks.size());
}

static bool is_current_bookmarked() {
    for (const auto &b : g_bookmarks) {
        if (b.first == current_spine && b.second == current_page_in_chapter)
            return true;
    }
    return false;
}

static void toggle_bookmark_on_current_page() {
    for (auto it = g_bookmarks.begin(); it != g_bookmarks.end(); ++it) {
        if (it->first == current_spine &&
            it->second == current_page_in_chapter) {
            g_bookmarks.erase(it);
            Serial.println("[BM] removed");
            save_bookmarks_for_current();
            return;
        }
    }
    g_bookmarks.push_back({current_spine, current_page_in_chapter});
    Serial.printf("[BM] added (ch=%d p=%d)\n",
                  current_spine, current_page_in_chapter);
    save_bookmarks_for_current();
}

// .pos format: "<spine> <page> [<percent> [<chapter_pages>]]\n".
// chapter_pages records how many pages the chapter had at save time. On
// load with a different density, page is rescaled proportionally so the
// reader lands at roughly the same place even though the page count
// differs. Legacy two/three-int files are still readable.
static bool read_position_for_name(const String &name, int *out_spine,
                                   int *out_page, int *out_percent = nullptr,
                                   int *out_chap_pages = nullptr) {
    File f = SD.open(pos_path_for_name(name), FILE_READ);
    if (!f) return false;
    String line = f.readStringUntil('\n');
    f.close();
    int sp = -1, pg = -1, pct = 0, cp = 0;
    int n = sscanf(line.c_str(), "%d %d %d %d", &sp, &pg, &pct, &cp);
    if (n >= 2) {
        *out_spine = sp;
        *out_page = pg;
        if (out_percent)    *out_percent    = (n >= 3) ? pct : 0;
        if (out_chap_pages) *out_chap_pages = (n >= 4) ? cp  : 0;
        return true;
    }
    return false;
}

static bool write_position_for_name(const String &name, int spine, int page,
                                    int percent = 0, int chap_pages = 0) {
    String path = pos_path_for_name(name);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("write_position: open %s failed\n", path.c_str()); return false; }
    f.printf("%d %d %d %d\n", spine, page, percent, chap_pages);
    f.close();
    Serial.printf("[POS_SAVED] %s -> ch=%d p=%d/%d %d%%\n",
                  path.c_str(), spine, page, chap_pages, percent);
    return true;
}

// Compute book-level progress % the same way render_book_page() does, so the
// number we persist matches what the user just saw on screen.
static int compute_book_progress_pct() {
    if (total_spine <= 0 || chapter_pages.empty()) return 0;
    int chap_pages = (int)chapter_pages.size();
    int read_pages = chap_pages * current_spine + (current_page_in_chapter + 1);
    int total_pages_est = chap_pages * total_spine;
    if (total_pages_est <= 0) return 0;
    int pct = (read_pages * 100 + total_pages_est / 2) / total_pages_est;
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return pct;
}

static void save_position() {
    if (selected < 0 || selected >= (int)books.size()) return;
    write_position_for_name(String(books[selected].c_str()),
                            current_spine, current_page_in_chapter,
                            compute_book_progress_pct(),
                            (int)chapter_pages.size());
}

static bool load_position(int *out_spine, int *out_page,
                          int *out_chap_pages = nullptr) {
    if (selected < 0 || selected >= (int)books.size()) return false;
    int dummy_pct = 0, cp = 0;
    bool ok = read_position_for_name(String(books[selected].c_str()),
                                     out_spine, out_page, &dummy_pct, &cp);
    if (out_chap_pages) *out_chap_pages = cp;
    if (ok) Serial.printf("[POS_LOADED] ch=%d p=%d (was /%d)\n",
                          *out_spine, *out_page, cp);
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
    g_toc_cache = std::move(r.toc);   // refresh cache on every chapter load
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
// Style-aware font picker. Tracks bold/italic state across calls within a
// single render_line invocation by reading the marker bytes embedded in the
// text.
static const GFXfont *font_for_style(const GFXfont *base, bool bold, bool italic) {
    // Only honor styling for the medium body font (matching variants exist).
    // For compact / loose density we fall back to the base.
    if (base != (const GFXfont *)&freesans_body) return base;
    if (bold && italic) return (const GFXfont *)&freesans_body_bolditalic;
    if (bold)           return (const GFXfont *)&freesans_body_bold;
    if (italic)         return (const GFXfont *)&freesans_body_italic;
    return base;
}

// Update style state from a marker byte. Idempotent for non-markers.
static inline void apply_marker(char m, bool &bold, bool &italic) {
    switch (m) {
        case STY_BOLD_ON:    bold   = true;  break;
        case STY_BOLD_OFF:   bold   = false; break;
        case STY_ITALIC_ON:  italic = true;  break;
        case STY_ITALIC_OFF: italic = false; break;
    }
}

// Measure / draw a "word" (no spaces) that may contain style markers.
// Splits the word at marker boundaries and processes each segment with the
// font matching the current style. Returns the rendered width.
// If `fb` is NULL, just measures.
static int draw_or_measure_styled_word(const GFXfont *base_font,
                                       const char *word, int len,
                                       bool &bold, bool &italic,
                                       int32_t x, int32_t y, uint8_t *fb) {
    int total_w = 0;
    int seg_start = 0;
    auto flush = [&](int upto) {
        if (upto <= seg_start) return;
        std::string seg(word + seg_start, upto - seg_start);
        const GFXfont *f = font_for_style(base_font, bold, italic);
        if (fb) {
            // Drawing pass: trust writeln's actual cursor advance.
            int32_t cx = x + total_w, cy = y;
            int32_t cx_init = cx;
            writeln((GFXfont *)f, seg.c_str(), &cx, &cy, fb);
            int adv = cx - cx_init;
            total_w += adv;
            return;
        }
        int32_t mx = 0, my = 0, mx1, my1, mw, mh;
        get_text_bounds(f, seg.c_str(), &mx, &my, &mx1, &my1, &mw, &mh, NULL);
        total_w += mx;
    };
    for (int i = 0; i <= len; ++i) {
        char c = (i < len) ? word[i] : '\0';
        if (i == len) { flush(i); break; }
        if (is_style_marker(c)) {
            flush(i);
            apply_marker(c, bold, italic);
            seg_start = i + 1;
        }
    }
    return total_w;
}

static void render_line(const GFXfont *font, const char *line,
                        int x_start, int target_w, int32_t y,
                        uint8_t *fb, bool justify) {
    // tokenize words. Style markers stay attached to whichever word they
    // sit next to (typically prefix of next word or suffix of current).
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

    // Style state persists across words within the line.
    bool bold = false, italic = false;

    int32_t cx = x_start, cy = y;

    // Space width — read the cursor advance after measuring " " (parameter
    // *x), NOT the bbox width (*w). A space has no inked pixels so its
    // bbox width is 0; the real width-between-words is advance_x.
    int32_t sp_x = 0, by = 0, bx1, by1, bw, bh;
    get_text_bounds(font, " ", &sp_x, &by, &bx1, &by1, &bw, &bh, NULL);
    int sp_w = (int)sp_x;

    if (!justify || words.size() == 1) {
        int gaps = (int)words.size() - 1;
        for (size_t i = 0; i < words.size(); ++i) {
            int adv = draw_or_measure_styled_word(font, words[i].data(),
                                                  (int)words[i].size(),
                                                  bold, italic, cx, cy, fb);
            cx += adv;
            if ((int)i < gaps) cx += sp_w;
        }
        return;
    }

    // Measure each word with its current style sequence. Width depends on
    // the order of markers, so use a copy of the state for measurement and
    // restore for the draw pass.
    bool m_bold = bold, m_italic = italic;
    std::vector<int> ww;
    int total_word_w = 0;
    for (auto &w : words) {
        int mw = draw_or_measure_styled_word(font, w.data(), (int)w.size(),
                                             m_bold, m_italic, 0, 0, nullptr);
        ww.push_back(mw);
        total_word_w += mw;
    }
    int gaps = (int)words.size() - 1;
    int natural_w = total_word_w + sp_w * gaps;
    int slack = target_w - natural_w;
    if (slack < 0 || slack > target_w / 3) {
        // Bail out of justification: just left-align with style switching.
        for (auto &w : words) {
            int adv = draw_or_measure_styled_word(font, w.data(), (int)w.size(),
                                                  bold, italic, cx, cy, fb);
            cx += adv + sp_w;
        }
        return;
    }
    int extra_each = slack / gaps;
    int extra_rem  = slack % gaps;

    for (size_t i = 0; i < words.size(); ++i) {
        int adv = draw_or_measure_styled_word(font, words[i].data(),
                                              (int)words[i].size(),
                                              bold, italic, cx, cy, fb);
        cx += adv;
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
    // Indent first line of each paragraph (book-style). Skip the indent for
    // the *very first* line of the page — that's either a continuation from
    // the previous page or a chapter heading, neither wants an indent.
    int32_t cy = y0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        bool last_of_para = (i + 1 >= lines.size()) || lines[i + 1].empty();
        bool blank = line.empty();
        bool first_of_para = (i > 0) && lines[i - 1].empty() && !blank;
        if (!blank) {
            int32_t x = first_of_para ? (x_start + PARAGRAPH_INDENT) : x_start;
            int32_t w = first_of_para ? (target_w - PARAGRAPH_INDENT) : target_w;
            render_line((GFXfont *)g_body_font, line.c_str(),
                        x, w, cy, fb, !last_of_para);
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

static void draw_vline(int x, int y0, int y1, uint8_t *fb) {
    if (x < 0 || x >= EPD_WIDTH) return;
    if (y0 < 0) y0 = 0;
    if (y1 > EPD_HEIGHT) y1 = EPD_HEIGHT;
    for (int y = y0; y < y1; ++y) {
        int idx = (y * EPD_WIDTH + x) / 2;
        fb[idx] &= (x & 1) ? 0x0F : 0xF0;
    }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint8_t *fb) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > EPD_WIDTH) x1 = EPD_WIDTH;
    if (y1 > EPD_HEIGHT) y1 = EPD_HEIGHT;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            int idx = (y * EPD_WIDTH + x) / 2;
            fb[idx] &= (x & 1) ? 0x0F : 0xF0;
        }
    }
}

// Blit a 1-bit packed bitmap (MSB-first within each byte, row-padded to a
// byte boundary; bit=1 = black, bit=0 = white/transparent) into the 4-bit
// framebuffer at top-left (x0, y0). Used for the boot-splash logo.
static void blit_1bit(int x0, int y0, int w, int h,
                      const uint8_t *bits, uint8_t *fb) {
    int row_bytes = (w + 7) / 8;
    for (int y = 0; y < h; ++y) {
        int dy = y0 + y;
        if (dy < 0 || dy >= EPD_HEIGHT) continue;
        const uint8_t *row = bits + y * row_bytes;
        for (int x = 0; x < w; ++x) {
            if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;
            int dx = x0 + x;
            if (dx < 0 || dx >= EPD_WIDTH) continue;
            int idx = (dy * EPD_WIDTH + dx) / 2;
            fb[idx] &= (dx & 1) ? 0x0F : 0xF0;   // black nibble
        }
    }
}

// Set a rectangle of pixels back to "white" (4-bit nibble = 0xF). Inverse
// of fill_rect. Used to carve a check mark out of a filled checkbox.
static void clear_rect(int x0, int y0, int x1, int y1, uint8_t *fb) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > EPD_WIDTH) x1 = EPD_WIDTH;
    if (y1 > EPD_HEIGHT) y1 = EPD_HEIGHT;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            int idx = (y * EPD_WIDTH + x) / 2;
            fb[idx] |= (x & 1) ? 0xF0 : 0x0F;
        }
    }
}

// Small checkbox sprite. (x0,y0) is top-left, size is the side length.
// Unchecked: hollow square. Checked: filled square with a white check mark.
static void draw_checkbox(int x0, int y0, int size, bool checked, uint8_t *fb) {
    int x1 = x0 + size, y1 = y0 + size;
    if (!checked) {
        draw_hline(y0, x0, x1 + 1, fb);
        draw_hline(y1, x0, x1 + 1, fb);
        draw_vline(x0, y0, y1 + 1, fb);
        draw_vline(x1, y0, y1 + 1, fb);
        return;
    }
    fill_rect(x0, y0, x1 + 1, y1 + 1, fb);
    // White check inside: draw two diagonals as 2-px-thick "✓".
    // Short leg: (x0+s/4, y0+s/2) → (x0+s/2, y1 - s/4)
    // Long leg : (x0+s/2, y1 - s/4) → (x1 - s/5, y0 + s/5)
    int ax = x0 + size / 4,         ay = y0 + size / 2;
    int bx = x0 + size / 2,         by = y1 - size / 4;
    int cx = x1 - size / 5,         cy = y0 + size / 5;
    auto line = [&](int xa, int ya, int xb, int yb) {
        int dx = xb - xa, dy = yb - ya;
        int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
        if (steps == 0) return;
        for (int i = 0; i <= steps; ++i) {
            int x = xa + dx * i / steps, y = ya + dy * i / steps;
            for (int oy = -1; oy <= 0; ++oy)        // 2-px thick
                for (int ox = -1; ox <= 0; ++ox)
                    clear_rect(x + ox, y + oy, x + ox + 1, y + oy + 1, fb);
        }
    };
    line(ax, ay, bx, by);
    line(bx, by, cx, cy);
}

// Fill a rounded rectangle with a 4-bit gray level (0=black, 0xF=white).
// If preserve_non_white is true, only pixels currently white (0xF) are
// touched — useful when the rect already contains text and you want the
// glyph body + anti-alias edges to remain unaffected.
static void fill_rect_rounded_gray(int x0, int y0, int x1, int y1, int r,
                                   uint8_t nibble, bool preserve_non_white,
                                   uint8_t *fb) {
    if (x0 > x1 || y0 > y1) return;
    nibble &= 0x0F;
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= EPD_HEIGHT) continue;
        int inset = 0;
        if (r > 0) {
            if (y < y0 + r) {
                int dy = (y0 + r) - y;
                int dx2 = r * r - dy * dy;
                int dx = (dx2 > 0) ? (int)sqrtf((float)dx2) : 0;
                inset = r - dx;
            } else if (y > y1 - r) {
                int dy = y - (y1 - r);
                int dx2 = r * r - dy * dy;
                int dx = (dx2 > 0) ? (int)sqrtf((float)dx2) : 0;
                inset = r - dx;
            }
        }
        int xa = x0 + inset, xb = x1 - inset;
        for (int x = xa; x <= xb; ++x) {
            if (x < 0 || x >= EPD_WIDTH) continue;
            int idx = (y * EPD_WIDTH + x) / 2;
            uint8_t cur = (x & 1) ? (fb[idx] >> 4) : (fb[idx] & 0x0F);
            if (preserve_non_white && cur != 0x0F) continue;
            if (x & 1) fb[idx] = (fb[idx] & 0x0F) | (nibble << 4);
            else       fb[idx] = (fb[idx] & 0xF0) | nibble;
        }
    }
}

// Rounded-rectangle outline. Corner radius `r` should be small (≤ ~10).
// Uses a midpoint-circle inner loop for the four quarter arcs and the
// existing line primitives for the four shortened sides.
static void draw_rect_rounded(int x0, int y0, int x1, int y1, int r,
                              uint8_t *fb) {
    if (r <= 0) {
        draw_hline(y0, x0, x1 + 1, fb);
        draw_hline(y1, x0, x1 + 1, fb);
        draw_vline(x0, y0, y1 + 1, fb);
        draw_vline(x1, y0, y1 + 1, fb);
        return;
    }
    draw_hline(y0, x0 + r, x1 - r + 1, fb);
    draw_hline(y1, x0 + r, x1 - r + 1, fb);
    draw_vline(x0, y0 + r, y1 - r + 1, fb);
    draw_vline(x1, y0 + r, y1 - r + 1, fb);
    auto plot = [fb](int x, int y) {
        if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
        int idx = (y * EPD_WIDTH + x) / 2;
        fb[idx] &= (x & 1) ? 0x0F : 0xF0;
    };
    int cx_tl = x0 + r, cy_tl = y0 + r;
    int cx_tr = x1 - r, cy_tr = y0 + r;
    int cx_bl = x0 + r, cy_bl = y1 - r;
    int cx_br = x1 - r, cy_br = y1 - r;
    int x = r, y = 0, err = 0;
    while (x >= y) {
        plot(cx_tl - x, cy_tl - y); plot(cx_tl - y, cy_tl - x);
        plot(cx_tr + x, cy_tr - y); plot(cx_tr + y, cy_tr - x);
        plot(cx_bl - x, cy_bl + y); plot(cx_bl - y, cy_bl + x);
        plot(cx_br + x, cy_br + y); plot(cx_br + y, cy_br + x);
        ++y;
        if (err <= 0) err += 2 * y + 1;
        else { --x; err -= 2 * x + 1; }
    }
}

// Bumped on every render and every touch / button event. The loop uses
// it to drive auto-sleep: after AUTO_SLEEP_MINUTES of no interaction in
// any reading-related mode (book / library / TOC / bookmark / TODO),
// enter deep sleep automatically. AP mode is exempt because the user is
// actively transferring files and the panel is showing a code/QR they
// may be reading off-screen.
static uint32_t g_last_interact_ms = 0;
static const uint32_t AUTO_SLEEP_MS = 5UL * 60UL * 1000UL;   // 5 min

// Set by render_book_page() / render_book_list() / etc. just before the
// CPU-side rendering kicks off; used by flush_framebuffer() to report
// how long layout+draw took relative to the e-paper waveform.
static uint32_t g_pt_layout_start_ms = 0;

// Push the current framebuffer to the EPD. Always full-screen — the
// per-page diff/region path was tested and removed because the typical
// text page changes ~93 % of vertical rows (body + page-number footer +
// progress line), so the savings were ~5 % and not worth the complexity.
// The big lever is g_clear_cycles, which scales clear time linearly.
//
// Logs:
//   [FLUSH] tag=X layout=Aa pwr=Pp clr=Cc draw=Dd off=Oo total=Tt cy=K
// where layout = ms from caller setting g_pt_layout_start_ms to the
// flush call, pwr/clr/draw/off are the four epd_* phases, total is
// wall-clock from layout-start to flush-end, cy is the active cycle count.
static void flush_framebuffer(bool force_full = false, const char *tag = "?") {
    (void)force_full;
    uint32_t t_flush_in = millis();
    uint32_t layout_ms = (g_pt_layout_start_ms != 0)
                         ? (t_flush_in - g_pt_layout_start_ms) : 0;
    uint32_t t0 = millis();
    epd_poweron();
    uint32_t t1 = millis();
    epd_clear_area_cycles(epd_full_screen(), g_clear_cycles, 50);
    uint32_t t2 = millis();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    uint32_t t3 = millis();
    epd_poweroff();
    uint32_t t4 = millis();
    Serial.printf("[FLUSH] tag=%s layout=%ums pwr=%ums clr=%ums "
                  "draw=%ums off=%ums total=%ums cy=%d\n",
                  tag, (unsigned)layout_ms,
                  (unsigned)(t1 - t0), (unsigned)(t2 - t1),
                  (unsigned)(t3 - t2), (unsigned)(t4 - t3),
                  (unsigned)(t4 - (g_pt_layout_start_ms ?
                                   g_pt_layout_start_ms : t0)),
                  g_clear_cycles);
    g_pt_layout_start_ms = 0;
    g_last_interact_ms = millis();
}

// Filled triangle in the top-right corner of the page = "this page is
// bookmarked" (a dog-eared corner). Drawn in render_book_page when
// is_current_bookmarked() is true. Size is in pixels along the diagonal.
static void draw_dogear(int size, uint8_t *fb) {
    const int x_right = EPD_WIDTH - 1;
    const int y_top   = 0;
    // Triangle vertices: (x_right, y_top), (x_right, y_top+size),
    // (x_right - size, y_top). Filled scanline-by-scanline.
    for (int dy = 0; dy <= size; ++dy) {
        int reach = size - dy;  // shrinks as we go down
        for (int dx = 0; dx <= reach; ++dx) {
            int px = x_right - dx, py = y_top + dy;
            if (px < 0 || px >= EPD_WIDTH || py < 0 || py >= EPD_HEIGHT) continue;
            int idx = (py * EPD_WIDTH + px) / 2;
            fb[idx] &= (px & 1) ? 0x0F : 0xF0;
        }
    }
}

// Right-pointing filled triangle "▶" — used as the selection cursor in the
// library and chapter-jump screens. (x, y_top) is the bounding box top-left;
// width is ~3/4 of `size` so it has a tasteful aspect ratio.
static void draw_selection_marker(int x, int y_top, int size, uint8_t *fb) {
    int h = size, w = (size * 3) / 4;
    for (int dy = 0; dy <= h; ++dy) {
        int reach = (dy * 2 <= h) ? (2 * dy * w) / h
                                  : (2 * (h - dy) * w) / h;
        for (int dx = 0; dx <= reach; ++dx) {
            int px = x + dx, py = y_top + dy;
            if (px < 0 || px >= EPD_WIDTH || py < 0 || py >= EPD_HEIGHT) continue;
            int idx = (py * EPD_WIDTH + px) / 2;
            fb[idx] &= (px & 1) ? 0x0F : 0xF0;
        }
    }
}

// Battery-shaped icon: outline rectangle with a small tip on the right and a
// fill bar proportional to pct. Positioned with x0 at the left edge,
// vertically centred around y_centre. When charging, draws a small lightning
// bolt over the fill so the user can see USB power is doing its job.
static void draw_battery_icon(int x0, int y_centre, int pct, uint8_t *fb) {
    const int W = 32, H = 14;
    int top = y_centre - H / 2;
    int bot = top + H;
    int left = x0;
    int right = left + W;
    draw_hline(top,     left, right + 1, fb);
    draw_hline(bot,     left, right + 1, fb);
    draw_vline(left,    top,  bot + 1,   fb);
    draw_vline(right,   top,  bot + 1,   fb);
    // tip bump on the right
    fill_rect(right + 1, top + 4, right + 5, bot - 3, fb);
    // fill bar (inset by 2px from outer rect)
    int max_w = W - 4;
    int fill_w = (pct < 0 ? 0 : pct > 100 ? 100 : pct) * max_w / 100;
    if (fill_w > 0) fill_rect(left + 2, top + 2, left + 2 + fill_w, bot - 1, fb);

    // Lightning bolt overlay when charging — drawn in white-on-fill so it
    // shows up against the dark fill bar; on the empty portion it draws as
    // a black outline. Centered vertically in the icon body.
    if (g_batt_charging) {
        int cx = left + W / 2;
        int cy = (top + bot) / 2;
        // Two diagonal strokes shaped like ⚡ — coordinates relative to (cx,cy).
        // Each "stroke" is a short filled wedge for visibility at this scale.
        // Erase a 2×2 rectangle around each bolt pixel so the bolt reads on
        // any fill level.
        const int8_t bolt[][2] = {
            { 1, -5}, { 0, -3}, {-1, -1}, { 1, -1}, { 0,  1},
            {-1,  3}, { 0,  5},
        };
        // First erase (white) under the bolt path so it's visible.
        for (auto &p : bolt) {
            int px = cx + p[0], py = cy + p[1];
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int x = px + dx, y = py + dy;
                    if (x <= left + 1 || x >= right - 1) continue;
                    if (y <= top + 1  || y >= bot - 1)   continue;
                    int idx = (y * EPD_WIDTH + x) / 2;
                    fb[idx] |= (x & 1) ? 0x0F : 0xF0;
                }
        }
        // Then draw the bolt itself in black.
        for (auto &p : bolt) {
            int px = cx + p[0], py = cy + p[1];
            if (px <= left + 1 || px >= right - 1) continue;
            if (py <= top + 1  || py >= bot - 1)   continue;
            int idx = (py * EPD_WIDTH + px) / 2;
            fb[idx] &= (px & 1) ? 0x0F : 0xF0;
        }
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
    g_pt_layout_start_ms = millis();

    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    // (no header strip — body uses the full vertical space; the top 80px is
    // still the tap-back-to-library zone, just invisibly so.)
    int target_w = EPD_WIDTH - 2 * BOOK_MARGIN_X - BODY_RIGHT_RESERVE;
    render_book_page_text(BOOK_MARGIN_X, target_w, BOOK_TOP_RESERVE + 30, framebuffer);

    // Dog-ear in the top-right corner if this page is bookmarked. Sits
    // outside the body's text band so it never conflicts with letters.
    if (is_current_bookmarked()) draw_dogear(36, framebuffer);

    // 3-column footer rendered in the smaller font so it occupies less vertical
    // space and reads as secondary metadata: [battery%] [Ch X/Y · NN%] [p A/B].
    // book_progress_pct counts chapters completed + the fraction of pages read
    // in the current chapter, so the last page of the last chapter is 100%.
    int book_progress_pct = 0;
    if (total_spine > 0 && !chapter_pages.empty()) {
        int chap_pages = (int)chapter_pages.size();
        int read_pages = chap_pages * current_spine + (current_page_in_chapter + 1);
        int total_pages_est = chap_pages * total_spine;
        if (total_pages_est > 0) {
            book_progress_pct = (read_pages * 100 + total_pages_est / 2) / total_pages_est;
            if (book_progress_pct > 100) book_progress_pct = 100;
        }
    }
    const GFXfont *small = (const GFXfont *)&firasans_small;
    int32_t footer_y = EPD_HEIGHT - 12;
    // divider just above the footer text — match the body's effective right
    // edge (target_w + BOOK_MARGIN_X) so the line aligns with where text
    // actually ends, not the raw margin point.
    int divider_x0 = BOOK_MARGIN_X;
    int divider_x1 = EPD_WIDTH - BOOK_MARGIN_X - BODY_RIGHT_RESERVE;
    draw_hline(EPD_HEIGHT - 42, divider_x0, divider_x1, framebuffer);
    // Thin book-progress bar at the very bottom — replaces the verbose
    // "NN% read" text we used to put next to the chapter label. Filled
    // portion = book_progress_pct of the body width.
    {
        int track_y = EPD_HEIGHT - 2;
        int fill_w = ((divider_x1 - divider_x0) * book_progress_pct) / 100;
        if (fill_w > 0)
            draw_hline(track_y, divider_x0, divider_x0 + fill_w, framebuffer);
    }
    update_charging_state();   // sample voltage trend on each render
    int batt_pct = read_battery_percent();

    // Layout: chapter (left) | page X/Y (center) | battery icon + % (right).
    char left[80], center[24], right[24];
    std::string toc_label = get_toc_label_for(current_spine);
    if (!toc_label.empty()) {
        if (toc_label.size() > 36) toc_label = toc_label.substr(0, 33) + "...";
        snprintf(left, sizeof(left), "%s", toc_label.c_str());
    } else {
        snprintf(left, sizeof(left), "Sec %d/%d",
                 current_spine + 1, total_spine);
    }
    (void)book_progress_pct;  // retained for the (book-end) screen elsewhere
    snprintf(center, sizeof(center), "p %d / %d",
             current_page_in_chapter + 1, (int)chapter_pages.size());
    snprintf(right, sizeof(right), "%d%%", batt_pct);

    // Left: chapter
    int32_t lx = BOOK_MARGIN_X, ly = footer_y;
    writeln(small, left, &lx, &ly, framebuffer);

    // Center: page
    int32_t cw, ch_ = 0, cmx = 0, cmy = 0, cmx1, cmy1;
    get_text_bounds(small, center, &cmx, &cmy, &cmx1, &cmy1, &cw, &ch_, NULL);
    int32_t cx_ = (EPD_WIDTH - cw) / 2, cy_ = footer_y;
    writeln(small, center, &cx_, &cy_, framebuffer);

    // Right: battery icon + percent (right-aligned). writeln() advances its
    // x argument to *past* the rendered text, so save the start X first —
    // the icon sits 42 px to the left of that, not 42 px past the end.
    int32_t rw, rh_ = 0, rmx = 0, rmy = 0, rmx1, rmy1;
    get_text_bounds(small, right, &rmx, &rmy, &rmx1, &rmy1, &rw, &rh_, NULL);
    int32_t text_start_x = EPD_WIDTH - BOOK_MARGIN_X - BODY_RIGHT_RESERVE - rw;
    int32_t rx_text = text_start_x, ry = footer_y;
    writeln(small, right, &rx_text, &ry, framebuffer);
    draw_battery_icon(text_start_x - 42, footer_y - 9, batt_pct, framebuffer);

    flush_framebuffer(/*force_full=*/false, "book");

    // EPD is now electrically idle — safe window to peek at GPIO 0 (STR_100).
    // We latch a flag rather than acting here so loop() consumes it.
    if (sample_str100_button()) {
        Serial.println("[STR_100] press detected (post-render)");
        str100_pressed = true;
    }
}

// "You finished the book!" screen shown after the last page. From here a tap
// goes back to the library; a long button-press still does AP/sleep.
static void render_book_end() {
    Serial.println("[BOOK] reached end-of-book");
    Serial.flush();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    int32_t cx = LIST_X, cy = 200;
    writeln((GFXfont *)&FiraSans, "You finished the book.",
            &cx, &cy, framebuffer);

    cx = LIST_X; cy = 280;
    char info[80];
    if (selected >= 0 && selected < (int)books.size()) {
        std::string title = books[selected];
        size_t dot = title.rfind(".epub");
        if (dot != std::string::npos) title.erase(dot);
        if (title.size() > 50) title = title.substr(0, 47) + "...";
        snprintf(info, sizeof(info), "%s", title.c_str());
        writeln((GFXfont *)&FiraSans, info, &cx, &cy, framebuffer);
    }

    // (no footer hints on the end screen — feels nicer to land on)

    flush_framebuffer(/*force_full=*/true, "book_end");
}

// Advance/go-back a page; cross chapter boundaries by loading next/prev spine item.
static void book_next_page() {
    if (current_page_in_chapter + 1 < (int)chapter_pages.size()) {
        ++current_page_in_chapter;
        render_book_page();
        save_position();
    } else if (current_spine + 1 < total_spine) {
        if (load_chapter(current_spine + 1)) { render_book_page(); save_position(); }
    } else {
        // last page of last chapter — show the end screen
        app_mode = MODE_BOOK_END;
        render_book_end();
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

// Pill-shaped button at the visual bottom-right corner of the screen, used
// to switch between the library and the TODO view. The tap zone is fixed
// (`corner_right` in the loop's touch handler) — this just renders it.
// Render order: text first (so glyph anti-alias edges land on white), then
// gray fill that *only* paints the still-white background pixels, then the
// outline. This avoids white halos around each letter.
static void draw_corner_button(const char *label) {
    const GFXfont *small = (const GFXfont *)&firasans_small;
    int32_t mx = 0, my = 0, mx1, my1, mw, mh;
    get_text_bounds(small, label, &mx, &my, &mx1, &my1, &mw, &mh, NULL);
    int pad_x = 18, pad_y = 11;
    int btn_w = mw + 2 * pad_x;
    int btn_h = mh + 2 * pad_y;
    int btn_x1 = EPD_WIDTH - 30;
    int btn_y1 = EPD_HEIGHT - 18;
    int btn_x0 = btn_x1 - btn_w;
    int btn_y0 = btn_y1 - btn_h;
    int32_t lx = btn_x0 + pad_x, ly = btn_y1 - pad_y - 2;
    writeln(small, label, &lx, &ly, framebuffer);
    fill_rect_rounded_gray(btn_x0, btn_y0, btn_x1, btn_y1, 8,
                           0xE, /*preserve_non_white=*/true, framebuffer);
    draw_rect_rounded(btn_x0, btn_y0, btn_x1, btn_y1, 8, framebuffer);
}

// Draw a 1-bit packed bitmap at the visual bottom-right of the screen,
// down-sampled by `stride` (so 200×180 with stride=5 → 40×36). Used as
// the icon-only corner button for switching between library and todo
// views — replaces `draw_corner_button` for those two screens.
static void draw_corner_icon(const uint8_t *bits, int w, int h, int stride) {
    int sw = w / stride, sh = h / stride;
    int x_right = EPD_WIDTH - 30;
    int y_bottom = EPD_HEIGHT - 18;
    int sx = x_right - sw, sy = y_bottom - sh;
    int row_bytes = (w + 7) / 8;
    for (int y = 0; y < h; y += stride) {
        for (int x = 0; x < w; x += stride) {
            if (!(bits[y * row_bytes + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int dy = sy + y / stride, dx = sx + x / stride;
            if (dy < 0 || dy >= EPD_HEIGHT || dx < 0 || dx >= EPD_WIDTH) continue;
            int idx = (dy * EPD_WIDTH + dx) / 2;
            framebuffer[idx] &= (dx & 1) ? 0x0F : 0xF0;
        }
    }
}

// Draw a 1-bit logo at half-scale at the top of a screen, with a title
// rendered next to it in the 32 pt face. Used for the library / TODO
// page headers so they share visual style.
static void draw_screen_header(const uint8_t *bits, int w, int h,
                               const char *title) {
    int sx = LIST_X, sy = 20;
    int row_bytes = (w + 7) / 8;
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            if (!(bits[y * row_bytes + (x >> 3)] & (0x80 >> (x & 7)))) continue;
            int dy = sy + y / 2, dx = sx + x / 2;
            if (dy < 0 || dy >= EPD_HEIGHT || dx < 0 || dx >= EPD_WIDTH) continue;
            int idx = (dy * EPD_WIDTH + dx) / 2;
            framebuffer[idx] &= (dx & 1) ? 0x0F : 0xF0;
        }
    }
    int32_t hx = LIST_X + (w / 2) + 24, hy = 88;
    writeln((GFXfont *)&freesans_title, title, &hx, &hy, framebuffer);
}

static void render_book_list() {
    Serial.printf("render_book_list: sel=%d page_first=%d total=%u\n",
                  selected, page_first, (unsigned)books.size());
    Serial.flush();
    dump_library_to_serial();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    const GFXfont *body  = (GFXfont *)&FiraSans;
    const GFXfont *small = (const GFXfont *)&firasans_small;

    // Header: 1/2-scale logo + "tiny-reader" title in the larger 32pt face.
    // Optional right-aligned page indicator on the same baseline.
    {
        int sx = LIST_X, sy = 20;
        int row_bytes = (LOGO_W + 7) / 8;
        for (int y = 0; y < LOGO_H; y += 2) {
            for (int x = 0; x < LOGO_W; x += 2) {
                if (LOGO_BITMAP[y * row_bytes + (x >> 3)] & (0x80 >> (x & 7))) {
                    int dy = sy + y / 2, dx = sx + x / 2;
                    if (dy < 0 || dy >= EPD_HEIGHT || dx < 0 || dx >= EPD_WIDTH)
                        continue;
                    int idx = (dy * EPD_WIDTH + dx) / 2;
                    framebuffer[idx] &= (dx & 1) ? 0x0F : 0xF0;
                }
            }
        }
    }
    const GFXfont *title = (const GFXfont *)&freesans_title;
    int32_t hx = LIST_X + (LOGO_W / 2) + 24, hy = 88;
    writeln(title, "tiny-reader", &hx, &hy, framebuffer);

    // Page indicator (top-right) only when the library spans >1 page.
    if (!books.empty()) {
        int total_pages = (books.size() + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
        int cur_page = page_first / LINES_PER_PAGE;
        if (total_pages > 1) {
            char pageinfo[32];
            snprintf(pageinfo, sizeof(pageinfo), "page %d / %d",
                     cur_page + 1, total_pages);
            int32_t mx = 0, my = 0, mx1, my1, mw, mh;
            get_text_bounds(small, pageinfo, &mx, &my, &mx1, &my1, &mw, &mh, NULL);
            int32_t rx = EPD_WIDTH - LIST_X - mw, ry = 88;
            writeln(small, pageinfo, &rx, &ry, framebuffer);
        }
    }

    if (books.empty()) {
        int32_t cx = LIST_X, cy = LIST_Y + 100;
        writeln(body, "No books on the SD card.",
                &cx, &cy, framebuffer);

        // QR code on the right — encodes WiFi credentials so a phone scan
        // joins the AP automatically. Then user opens 192.168.4.1 to upload.
        const int qr_scale = 2;   // 132*2 = 264 px on screen
        int qr_w = QR_WIFI_W * qr_scale;
        int qr_h = QR_WIFI_H * qr_scale;
        int qr_x = EPD_WIDTH - LIST_X - qr_w;
        int qr_y = LIST_Y + 100;
        int row_bytes = (QR_WIFI_W + 7) / 8;
        for (int y = 0; y < QR_WIFI_H; ++y) {
            for (int x = 0; x < QR_WIFI_W; ++x) {
                if (!(QR_WIFI_BITMAP[y * row_bytes + (x >> 3)] &
                      (0x80 >> (x & 7)))) continue;
                for (int dy = 0; dy < qr_scale; ++dy)
                    for (int dx = 0; dx < qr_scale; ++dx) {
                        int px = qr_x + x * qr_scale + dx;
                        int py = qr_y + y * qr_scale + dy;
                        if (px < 0 || px >= EPD_WIDTH ||
                            py < 0 || py >= EPD_HEIGHT) continue;
                        int idx = (py * EPD_WIDTH + px) / 2;
                        framebuffer[idx] &= (px & 1) ? 0x0F : 0xF0;
                    }
            }
        }

        cx = LIST_X; cy += 70;
        writeln(small, "1. Press and hold the button to share over WiFi.",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy += 36;
        writeln(small, "2. Scan the QR (or join 'tiny-reader' / 'bv-birdy').",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy += 36;
        writeln(small, "3. Open http://192.168.4.1 and upload .epub files.",
                &cx, &cy, framebuffer);
    } else {
        // Per-row layout: small cover thumb on the left, cursor + title,
        // state indicator on the right. Cache is full-resolution
        // (THUMB_W × THUMB_H), but we render it at half size in the
        // library so rows stay compact (taller rows are reserved for
        // the deep-sleep screensaver, which uses the full thumb upscaled).
        const int BAR_H = 12;
        const int RIGHT_X = EPD_WIDTH - LIST_X;
        const int PILL_SIZE = 18;
        const int LIB_THUMB_W = THUMB_W / 2;       // 40
        const int LIB_THUMB_H = THUMB_H / 2;       // 55
        const int LIB_ROW_HEIGHT = 64;
        const int TEXT_LEFT = LIST_X + LIB_THUMB_W + 24;

        int row = 0;
        for (size_t i = page_first; i < books.size() && row < LINES_PER_PAGE; ++i, ++row) {
            bool sel = ((int)i == selected);
            int32_t baseline = LIST_Y + 110 + row * LIB_ROW_HEIGHT;

            // Cover thumbnail (cached 4-bit raw on SD as <bookname>.thumb).
            // Blit at 1/2 scale by sampling every 2nd pixel.
            String thumb_path = thumb_path_for_name(String(books[i].c_str()));
            File tf = SD.open(thumb_path, FILE_READ);
            int thumb_top = baseline - LIB_THUMB_H + 8;   // align with row baseline
            if (tf && tf.size() == (long)THUMB_BYTES) {
                static uint8_t thumb_buf[THUMB_BYTES];
                tf.read(thumb_buf, THUMB_BYTES);
                tf.close();
                for (int y = 0; y < LIB_THUMB_H; ++y) {
                    int sy = y * 2;
                    for (int x = 0; x < LIB_THUMB_W; ++x) {
                        int sx = x * 2;
                        int idx = (sy * THUMB_W + sx) / 2;
                        uint8_t nib = (sx & 1) ? (thumb_buf[idx] >> 4)
                                               : (thumb_buf[idx] & 0x0F);
                        int dx = LIST_X + x, dy = thumb_top + y;
                        if (dx < 0 || dx >= EPD_WIDTH ||
                            dy < 0 || dy >= EPD_HEIGHT) continue;
                        int fbidx = (dy * EPD_WIDTH + dx) / 2;
                        if (dx & 1)
                            framebuffer[fbidx] = (framebuffer[fbidx] & 0x0F) |
                                                 (nib << 4);
                        else
                            framebuffer[fbidx] = (framebuffer[fbidx] & 0xF0) |
                                                 nib;
                    }
                }
            } else if (tf) {
                tf.close();
            }

            // Cursor sprite (filled triangle) + title. The right edge of
            // the title must clear the right-side state column (time
            // estimate + progress bar = ~190 px before RIGHT_X).
            std::string title = books[i];
            size_t dot = title.rfind(".epub");
            if (dot != std::string::npos) title.erase(dot);
            if (title.size() > 28) title = title.substr(0, 25) + "...";
            if (sel) {
                draw_selection_marker(TEXT_LEFT, baseline - 28, 24, framebuffer);
            }
            int32_t tx = TEXT_LEFT + 36, ty = baseline;
            writeln(body, title.c_str(), &tx, &ty, framebuffer);

            // Right-side tri-state: not-started (hollow pill), reading
            // (progress bar + %), finished (filled pill).
            int sp = 0, pg = 0, pct = 0;
            bool has_pos = read_position_for_name(String(books[i].c_str()),
                                                  &sp, &pg, &pct);
            int pill_y0 = baseline - 22, pill_y1 = pill_y0 + PILL_SIZE;
            int pill_x1 = RIGHT_X, pill_x0 = pill_x1 - PILL_SIZE;
            if (!has_pos) {
                // not started — empty rounded pill
                draw_rect_rounded(pill_x0, pill_y0, pill_x1, pill_y1,
                                  PILL_SIZE / 2, framebuffer);
                continue;
            }
            if (pct >= 99) {
                // finished — solid filled pill
                fill_rect_rounded_gray(pill_x0, pill_y0, pill_x1, pill_y1,
                                       PILL_SIZE / 2, 0x0,
                                       /*preserve_non_white=*/false, framebuffer);
                continue;
            }
            // in progress: fixed-width bar + estimated time remaining.
            // Reading-time math: assume words ≈ file_size / 9 (typical EPUB
            // text-vs-tag ratio for English) and 250 WPM reading speed, so
            // total_minutes = file_size / 2250. Multiply by (1 - pct/100)
            // for remaining time. Rough but useful at-a-glance signal.
            const int BAR_W = 120;
            int bar_x1 = RIGHT_X;
            int bar_x0 = bar_x1 - BAR_W;
            int by0 = baseline - 22;
            int by1 = by0 + BAR_H;
            draw_hline(by0, bar_x0, bar_x1, framebuffer);
            draw_hline(by1, bar_x0, bar_x1, framebuffer);
            draw_vline(bar_x0, by0, by1, framebuffer);
            draw_vline(bar_x1, by0, by1, framebuffer);
            int inner_w = (bar_x1 - bar_x0) - 4;
            int fill_w = (pct * inner_w + 50) / 100;
            if (fill_w > 0)
                fill_rect(bar_x0 + 2, by0 + 2,
                          bar_x0 + 2 + fill_w, by1 - 1, framebuffer);
            String book_path = String("/") + books[i].c_str();
            File f = SD.open(book_path, FILE_READ);
            size_t fsize = f ? f.size() : 0;
            if (f) f.close();
            if (fsize > 0) {
                int total_min = (int)(fsize / 2250UL);
                int remaining = total_min * (100 - pct) / 100;
                char est[16];
                if (remaining <= 0) est[0] = 0;
                else if (remaining < 60)
                    snprintf(est, sizeof(est), "~%dm", remaining);
                else
                    snprintf(est, sizeof(est), "~%dh", (remaining + 30) / 60);
                if (est[0]) {
                    int32_t mx = 0, my = 0, mx1, my1, mw, mh;
                    get_text_bounds(small, est, &mx, &my, &mx1, &my1,
                                    &mw, &mh, NULL);
                    int32_t lx = bar_x0 - 12 - mw, ly = baseline;
                    writeln(small, est, &lx, &ly, framebuffer);
                }
            }
        }
    }

    draw_corner_icon(TODO_LOGO_BITMAP, TODO_LOGO_W, TODO_LOGO_H, 4);

    flush_framebuffer(/*force_full=*/true, "lib");

    // Same idle-window peek as render_book_page(). In library mode the latch
    // is a debug-log only (see loop()), but we still keep it warm so the user
    // gets feedback if the wiring works.
    if (sample_str100_button()) {
        Serial.println("[STR_100] press detected (library post-render)");
        str100_pressed = true;
    }
}

// Chapter jump: a vertical list of TOC chapter titles. Tap a row to load
// that chapter (using its real spine_index from the NCX) and return to
// MODE_BOOK. Tap top edge cancels. Falls back to a numeric grid if the
// book has no NCX TOC.
static const int CJ_ROW_HEIGHT = 50;
static const int CJ_ROWS_PER_PAGE = 7;   // 8 caused the last row's descender
                                         // to clip into the footer divider
static int cj_first = 0;   // first TOC entry shown (for paging if > 8)

static void render_chapter_jump() {
    Serial.printf("render_chapter_jump: cur_spine=%d toc=%u\n",
                  current_spine, (unsigned)g_toc_cache.size());
    Serial.flush();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    int32_t hx = LIST_X, hy = LIST_Y;
    char header[80];
    snprintf(header, sizeof(header), "Jump to chapter  (%u in TOC)",
             (unsigned)g_toc_cache.size());
    writeln((GFXfont *)&FiraSans, header, &hx, &hy, framebuffer);

    // Compute "current TOC entry" = largest spine_index <= current_spine
    int cur_toc = -1, best_si = -1;
    for (size_t i = 0; i < g_toc_cache.size(); ++i) {
        if (g_toc_cache[i].spine_index <= current_spine &&
            g_toc_cache[i].spine_index > best_si) {
            best_si = g_toc_cache[i].spine_index;
            cur_toc = (int)i;
        }
    }

    if (g_toc_cache.empty()) {
        int32_t cx = LIST_X, cy = LIST_Y + 120;
        writeln((GFXfont *)&FiraSans,
                "This book has no TOC.",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy = LIST_Y + 180;
        writeln((GFXfont *)&FiraSans,
                "Use left/right page taps to navigate.",
                &cx, &cy, framebuffer);
    } else {
        int total = (int)g_toc_cache.size();
        int total_pages = (total + CJ_ROWS_PER_PAGE - 1) / CJ_ROWS_PER_PAGE;
        int cur_page = cj_first / CJ_ROWS_PER_PAGE;
        if (total_pages > 1) {
            char pg[32];
            snprintf(pg, sizeof(pg), "page %d/%d", cur_page + 1, total_pages);
            int32_t pgx = EPD_WIDTH - 220, pgy = LIST_Y;
            writeln((GFXfont *)&FiraSans, pg, &pgx, &pgy, framebuffer);
        }

        int rows_shown = (CJ_ROWS_PER_PAGE < total - cj_first)
                       ? CJ_ROWS_PER_PAGE : (total - cj_first);
        for (int row = 0; row < rows_shown; ++row) {
            int idx = cj_first + row;
            int32_t cy = LIST_Y + 60 + row * CJ_ROW_HEIGHT;
            std::string title = g_toc_cache[idx].title;
            if (title.empty()) title = "(untitled)";
            if (title.size() > 50) title = title.substr(0, 47) + "...";
            if (idx == cur_toc) {
                draw_selection_marker(LIST_X, cy - 28, 24, framebuffer);
            }
            int32_t cx = LIST_X + 36;
            writeln((GFXfont *)&FiraSans, title.c_str(), &cx, &cy, framebuffer);
        }
    }

    // (footer hints intentionally removed)

    flush_framebuffer(/*force_full=*/true, "chap_jump");
}

// Bookmark list view: one row per saved bookmark, like chapter-jump but
// reading from g_bookmarks. Header shows count, each row shows the TOC
// chapter label (if any) + page number. Tap a row to jump.
static int bm_first = 0;     // first bookmark visible (for paging)
static const int BM_ROW_HEIGHT = 56;
static const int BM_ROWS_PER_PAGE = 7;

static std::string toc_label_for_spine(int spine_index) {
    std::string best;
    int best_si = -1;
    for (size_t i = 0; i < g_toc_cache.size(); ++i) {
        if (g_toc_cache[i].spine_index <= spine_index &&
            g_toc_cache[i].spine_index > best_si) {
            best_si = g_toc_cache[i].spine_index;
            best = g_toc_cache[i].title;
        }
    }
    return best;
}

static void render_bookmark_list() {
    Serial.printf("render_bookmark_list: %u bookmarks\n",
                  (unsigned)g_bookmarks.size());
    Serial.flush();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    int32_t cx = LIST_X, cy = LIST_Y;
    char header[64];
    snprintf(header, sizeof(header), "Bookmarks  (%u)",
             (unsigned)g_bookmarks.size());
    writeln((GFXfont *)&FiraSans, header, &cx, &cy, framebuffer);

    if (g_bookmarks.empty()) {
        cx = LIST_X; cy = LIST_Y + 120;
        writeln((GFXfont *)&FiraSans, "No bookmarks yet.",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy = LIST_Y + 180;
        writeln((const GFXfont *)&firasans_small,
                "Tap the top-right corner of a page to add one.",
                &cx, &cy, framebuffer);
    } else {
        int total = (int)g_bookmarks.size();
        int total_pages = (total + BM_ROWS_PER_PAGE - 1) / BM_ROWS_PER_PAGE;
        int cur_page = bm_first / BM_ROWS_PER_PAGE;
        if (total_pages > 1) {
            char pg[32];
            snprintf(pg, sizeof(pg), "page %d/%d", cur_page + 1, total_pages);
            int32_t pgx = EPD_WIDTH - 220, pgy = LIST_Y;
            writeln((GFXfont *)&FiraSans, pg, &pgx, &pgy, framebuffer);
        }
        int rows_shown = (BM_ROWS_PER_PAGE < total - bm_first)
                       ? BM_ROWS_PER_PAGE : (total - bm_first);
        for (int row = 0; row < rows_shown; ++row) {
            int idx = bm_first + row;
            int32_t cy_row = LIST_Y + 60 + row * BM_ROW_HEIGHT;
            int sp = g_bookmarks[idx].first;
            int pg = g_bookmarks[idx].second;
            std::string label = toc_label_for_spine(sp);
            if (label.empty()) {
                char buf[40];
                snprintf(buf, sizeof(buf), "Section %d", sp + 1);
                label = buf;
            }
            if (label.size() > 40) label = label.substr(0, 37) + "...";
            char info[12];
            snprintf(info, sizeof(info), "p %d", pg + 1);
            int32_t lx = LIST_X + 36, ly = cy_row;
            writeln((GFXfont *)&FiraSans, label.c_str(), &lx, &ly, framebuffer);
            // page number, right-aligned
            int32_t mx = 0, my = 0, mx1, my1, mw, mh;
            get_text_bounds((GFXfont *)&FiraSans, info, &mx, &my, &mx1,
                            &my1, &mw, &mh, NULL);
            int32_t rx = EPD_WIDTH - LIST_X - mw, ry = cy_row;
            writeln((GFXfont *)&FiraSans, info, &rx, &ry, framebuffer);
        }
    }

    flush_framebuffer(/*force_full=*/true, "bm_list");
}

// Tap → bookmark index, or -1 if outside.
static int bookmark_hit_test(int16_t x, int16_t y) {
    if (g_bookmarks.empty()) return -1;
    int screen_y = EPD_HEIGHT - 1 - y;
    int rows_top = LIST_Y + 60 - 30;
    int rows_bot = rows_top + BM_ROWS_PER_PAGE * BM_ROW_HEIGHT;
    if (screen_y < rows_top || screen_y >= rows_bot) return -1;
    int row = (screen_y - rows_top) / BM_ROW_HEIGHT;
    int idx = bm_first + row;
    if (idx < 0 || idx >= (int)g_bookmarks.size()) return -1;
    return idx;
}

// Returns the TOC index that was tapped, or -1 if the tap missed every row.
// (Caller looks up spine_index + title from g_toc_cache so we can do the
// title-text search needed for single-spine Gutenberg books.)
static int chapter_jump_hit_test(int16_t x, int16_t y) {
    if (g_toc_cache.empty()) return -1;
    int screen_y = EPD_HEIGHT - 1 - y;
    int rows_top = LIST_Y + 60 - 30;   // first row is ascender-above-baseline
    int rows_bot = rows_top + CJ_ROWS_PER_PAGE * CJ_ROW_HEIGHT;
    if (screen_y < rows_top || screen_y >= rows_bot) return -1;
    int row = (screen_y - rows_top) / CJ_ROW_HEIGHT;
    int idx = cj_first + row;
    if (idx < 0 || idx >= (int)g_toc_cache.size()) return -1;
    Serial.printf("[CJ] tap (%d,%d) -> row=%d toc[%d]='%s' spine=%d\n",
                  x, y, row, idx, g_toc_cache[idx].title.c_str(),
                  g_toc_cache[idx].spine_index);
    return idx;
}

// Convert a tap on the TODO list view into the item index, or -1 if outside
// any row. Touch Y is inverted relative to render Y, same fix as chapter_jump.
static int todo_hit_test(int16_t x, int16_t y) {
    int screen_y = EPD_HEIGHT - 1 - y;
    int rows_top = LIST_Y + 100 - 30;   // matches the new header offset
    int row = (screen_y - rows_top) / LINE_HEIGHT;
    const int max_rows = 6;
    if (row < 0 || row >= max_rows) return -1;
    if (row >= (int)g_todos.size()) return -1;
    return row;
}

// Tappable TODO list view. Tap a row → toggle done. Editing of *content*
// (text, add, remove) still happens in the hub web UI.
static void render_todo_list() {
    Serial.printf("render_todo_list: %u items\n", (unsigned)g_todos.size());
    Serial.flush();
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    // Header: notepad-checkmark logo + "tiny-todo" title (matches the
    // book-sparkle + "tiny-reader" header on the library page).
    draw_screen_header(TODO_LOGO_BITMAP, TODO_LOGO_W, TODO_LOGO_H, "tiny-todo");

    if (g_todos.empty()) {
        int32_t cx = LIST_X, cy = LIST_Y + 100;
        writeln((GFXfont *)&FiraSans, "No TODO items yet.",
                &cx, &cy, framebuffer);
        cx = LIST_X; cy += 50;
        writeln((const GFXfont *)&firasans_small,
                "Add some via the hub web UI.",
                &cx, &cy, framebuffer);
    } else {
        const int max_rows = 6;
        // Indent rows under the header so the checkbox doesn't pull the
        // eye left of the logo's leftmost inked pixel.
        const int ROW_INDENT = 28;
        for (size_t i = 0; i < g_todos.size() && (int)i < max_rows; ++i) {
            int baseline = LIST_Y + 100 + (int)i * LINE_HEIGHT;
            // Checkbox sprite to the left of the text.
            draw_checkbox(LIST_X + ROW_INDENT, baseline - 36, 32,
                          g_todos[i].done, framebuffer);
            int32_t cx = LIST_X + ROW_INDENT + 52, cy = baseline;
            std::string line = g_todos[i].text;
            if (line.size() > 54) line = line.substr(0, 51) + "...";
            int32_t text_x_start = cx;
            writeln((GFXfont *)&FiraSans, line.c_str(), &cx, &cy, framebuffer);
            // Strikethrough done items.
            if (g_todos[i].done) {
                int32_t mw = cx - text_x_start;
                int strike_y = baseline - 16;
                draw_hline(strike_y,
                           text_x_start, text_x_start + mw, framebuffer);
            }
        }
        if ((int)g_todos.size() > max_rows) {
            int32_t cx = LIST_X + ROW_INDENT + 52;
            int32_t cy = LIST_Y + 100 + max_rows * LINE_HEIGHT;
            char more[40];
            snprintf(more, sizeof(more), "... and %d more (see hub)",
                     (int)g_todos.size() - max_rows);
            writeln((GFXfont *)&FiraSans, more, &cx, &cy, framebuffer);
        }
    }

    draw_corner_icon(LOGO_BITMAP, LOGO_W, LOGO_H, 4);

    flush_framebuffer(/*force_full=*/true, "todo");
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
    load_bookmarks_for_current();

    // Try to resume from saved position. If the .pos sidecar also recorded
    // the chapter's page count at save time, scale `saved_pg` to the
    // current pagination — covers the "user changed density between
    // sessions" case so we land at roughly the same place rather than at
    // a literal "page N" of a totally different layout.
    int saved_ch = 0, saved_pg = 0, saved_chap_pages = 0;
    bool resumed = load_position(&saved_ch, &saved_pg, &saved_chap_pages);

    if (load_chapter(resumed ? saved_ch : 0)) {
        // Fresh open: skip front-matter (cover, copyright page, dedication,
        // ...) by jumping to the first TOC entry's spine, which is normally
        // chapter 1 of the actual story.
        if (!resumed && !g_toc_cache.empty()) {
            int first_toc_spine = g_toc_cache[0].spine_index;
            if (first_toc_spine > 0 && first_toc_spine < total_spine) {
                Serial.printf("[OPEN] skip front-matter, jump to TOC[0] sp=%d\n",
                              first_toc_spine);
                load_chapter(first_toc_spine);
            }
        }
        Serial.printf("Loaded book: spine=%d, chapter has %u pages%s\n",
                      total_spine, (unsigned)chapter_pages.size(),
                      resumed ? " (resumed)" : "");
        if (resumed) {
            int target = saved_pg;
            int new_pages = (int)chapter_pages.size();
            if (saved_chap_pages > 0 && new_pages > 0 &&
                saved_chap_pages != new_pages) {
                target = (int)((int64_t)saved_pg * new_pages / saved_chap_pages);
                Serial.printf("[POS_RESCALE] %d/%d -> %d/%d\n",
                              saved_pg, saved_chap_pages, target, new_pages);
            }
            if (target < 0) target = 0;
            if (target >= new_pages) target = new_pages - 1;
            current_page_in_chapter = target;
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
// Set by the WiFi event handler when a station connects/disconnects, consumed
// by loop() so the (slow) screen redraw runs on the main task, not from the
// WiFi event callback.
static volatile bool g_ap_clients_changed = false;
static bool g_ap_has_client = false;
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
                int sp = 0, pg = 0, pct = 0;
                bool has_pos = read_position_for_name(name, &sp, &pg, &pct);
                json += "{\"name\":\"" + esc + "\"";
                json += ",\"size\":" + String(f.size());
                json += ",\"spine\":" + String(sp);
                json += ",\"page\":" + String(pg);
                json += ",\"percent\":" + String(pct);
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

    // Header: 1/2-scale logo + "tiny-reader" title in the larger 32pt face.
    {
        int sx = 60, sy = 35;
        int row_bytes = (LOGO_W + 7) / 8;
        for (int y = 0; y < LOGO_H; y += 2) {
            for (int x = 0; x < LOGO_W; x += 2) {
                if (LOGO_BITMAP[y * row_bytes + (x >> 3)] & (0x80 >> (x & 7))) {
                    int dy = sy + y / 2, dx = sx + x / 2;
                    if (dy < 0 || dy >= EPD_HEIGHT || dx < 0 || dx >= EPD_WIDTH)
                        continue;
                    int idx = (dy * EPD_WIDTH + dx) / 2;
                    framebuffer[idx] &= (dx & 1) ? 0x0F : 0xF0;
                }
            }
        }
    }
    int32_t cx = 60 + (LOGO_W / 2) + 24, cy = 110;
    writeln((GFXfont *)&freesans_title, "tiny-reader", &cx, &cy, framebuffer);

    // SSID + URL on the left, WiFi-credentials QR on the right. Scanning
    // the QR makes the phone join the AP automatically; then the user
    // opens the URL in a browser.
    char line[200];
    cx = 60; cy = 240;
    snprintf(line, sizeof(line), "Connect to WiFi: %s", ap_ssid.c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    cx = 60; cy = 340;
    writeln((GFXfont *)&FiraSans, "Open in browser:", &cx, &cy, framebuffer);
    cx = 60; cy = 400;
    snprintf(line, sizeof(line), "http://%s",
             WiFi.softAPIP().toString().c_str());
    writeln((GFXfont *)&FiraSans, line, &cx, &cy, framebuffer);

    // QR (132×132 source, 2× upscale → 264×264) on the right.
    const int qr_scale = 2;
    int qr_w = QR_WIFI_W * qr_scale;
    int qr_h = QR_WIFI_H * qr_scale;
    int qr_x = EPD_WIDTH - qr_w - 60;
    int qr_y = (EPD_HEIGHT - qr_h) / 2;
    {
        int row_bytes = (QR_WIFI_W + 7) / 8;
        for (int y = 0; y < QR_WIFI_H; ++y) {
            for (int x = 0; x < QR_WIFI_W; ++x) {
                if (!(QR_WIFI_BITMAP[y * row_bytes + (x >> 3)] &
                      (0x80 >> (x & 7)))) continue;
                for (int dy = 0; dy < qr_scale; ++dy)
                    for (int dx = 0; dx < qr_scale; ++dx) {
                        int px = qr_x + x * qr_scale + dx;
                        int py = qr_y + y * qr_scale + dy;
                        if (px < 0 || px >= EPD_WIDTH ||
                            py < 0 || py >= EPD_HEIGHT) continue;
                        int idx = (py * EPD_WIDTH + px) / 2;
                        framebuffer[idx] &= (px & 1) ? 0x0F : 0xF0;
                    }
            }
        }
    }

    // Small "Connected" label centered under the QR — only when a station
    // is currently associated. Nothing shown when waiting.
    if (g_ap_has_client) {
        const GFXfont *small = (GFXfont *)&firasans_small;
        const char *msg = "Connected";
        int32_t bx = 0, by = 0, bx1 = 0, by1 = 0, bw = 0, bh = 0;
        get_text_bounds(small, msg, &bx, &by, &bx1, &by1, &bw, &bh, nullptr);
        int32_t lx = qr_x + (qr_w - bw) / 2;
        int32_t ly = qr_y + qr_h + 36;
        writeln(small, msg, &lx, &ly, framebuffer);
    }

    flush_framebuffer(/*force_full=*/true, "ap");
}

// Async handlers below take AsyncWebServerRequest *req.
static void ap_handle_root(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    AsyncWebServerResponse *r = req->beginResponse_P(200, "text/html; charset=utf-8", HUB_HTML);
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

static void ap_handle_todos_get(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    Serial.printf("[HTTP] GET /api/todos (n=%u)\n", (unsigned)g_todos.size());
    JsonDocument doc;
    JsonArray items = doc["items"].to<JsonArray>();
    for (const TodoItem &it : g_todos) {
        JsonObject o = items.add<JsonObject>();
        o["text"] = it.text;
        o["done"] = it.done;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void ap_handle_todos_post(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    Serial.printf("[HTTP] POST /api/todos — params: %d\n", req->params());
    if (!req->hasParam("items", true)) {
        req->send(400, "application/json",
                  "{\"error\":\"missing items field\"}");
        return;
    }
    String raw = req->getParam("items", true)->value();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, raw);
    if (err) {
        Serial.printf("[TODOS] POST parse failed: %s\n", err.c_str());
        req->send(400, "application/json",
                  "{\"error\":\"invalid json\"}");
        return;
    }
    if (!doc.is<JsonArray>()) {
        req->send(400, "application/json",
                  "{\"error\":\"items must be a JSON array\"}");
        return;
    }
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    if (arr.size() > TODOS_MAX_ITEMS) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"too many items (max %u)\"}",
                 (unsigned)TODOS_MAX_ITEMS);
        req->send(400, "application/json", buf);
        return;
    }
    std::vector<TodoItem> next;
    next.reserve(arr.size());
    for (JsonVariantConst v : arr) {
        const char *t = v["text"] | "";
        bool d = v["done"] | false;
        if (!t) t = "";
        size_t tlen = strlen(t);
        if (tlen > TODOS_MAX_TEXT) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "{\"error\":\"item too long (max %u chars)\"}",
                     (unsigned)TODOS_MAX_TEXT);
            req->send(400, "application/json", buf);
            return;
        }
        if (tlen == 0) continue;   // skip blanks silently
        TodoItem it;
        it.text.assign(t, tlen);
        it.done = d;
        next.push_back(std::move(it));
    }
    g_todos.swap(next);
    bool ok = save_todos();
    if (!ok) {
        req->send(500, "application/json",
                  "{\"error\":\"save failed\"}");
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%u}",
             (unsigned)g_todos.size());
    req->send(200, "application/json", buf);
}

static void ap_handle_books_json(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    String json = ap_build_books_json();
    Serial.printf("[HTTP] GET /api/books -> %d bytes\n", json.length());
    req->send(200, "application/json", json);
}

// /api/upload_chunk — chunked upload: each chunk is its own POST with raw
// body, so each TCP connection is fresh and the lwIP per-connection
// receive-window cap (~80 KB on this stack) doesn't accumulate.
//   query: name=<filename>  offset=<bytes>  final=1 (on last chunk)
//   body : raw chunk bytes
// The body callback only memcpy's into a PSRAM buffer (fast, never blocks
// AsyncTCP). The done handler does the SD write once per chunk.
static const size_t CHUNK_BUF_SIZE = 64 * 1024;
static uint8_t *g_chunk_buf = nullptr;
static size_t   g_chunk_used = 0;
static bool     g_chunk_overflow = false;
// Last time a chunk arrived. Read from loop() to suppress the AP idle timer
// while an upload is mid-stream — covers cases where ap_last_activity
// somehow lags (cross-task visibility, JS timing) and a long page sit.
static volatile uint32_t g_chunk_last_ms = 0;

static void ap_handle_chunk_body(AsyncWebServerRequest *req, uint8_t *data,
                                 size_t len, size_t index, size_t total) {
    ap_last_activity = millis();
    if (index == 0) {
        if (!g_chunk_buf) g_chunk_buf = (uint8_t *)ps_malloc(CHUNK_BUF_SIZE);
        g_chunk_used = 0;
        g_chunk_overflow = false;
    }
    if (g_chunk_buf && len) {
        if (g_chunk_used + len <= CHUNK_BUF_SIZE) {
            memcpy(g_chunk_buf + g_chunk_used, data, len);
            g_chunk_used += len;
        } else {
            g_chunk_overflow = true;
        }
    }
}

static void ap_handle_chunk_done(AsyncWebServerRequest *req) {
    ap_last_activity = millis();
    g_chunk_last_ms = ap_last_activity;
    if (!req->hasParam("name") || !req->hasParam("offset")) {
        req->send(400, "application/json", "{\"error\":\"missing name/offset\"}");
        return;
    }
    if (g_chunk_overflow) {
        req->send(413, "application/json", "{\"error\":\"chunk too large\"}");
        return;
    }
    String name   = req->getParam("name")->value();
    size_t offset = (size_t)req->getParam("offset")->value().toInt();
    bool   is_final = req->hasParam("final");
    String path = String("/") + name;

    // First chunk truncates an existing file; subsequent chunks append.
    if (offset == 0 && SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) {
        Serial.printf("[CHUNK] SD.open %s FAILED\n", path.c_str());
        req->send(500, "application/json", "{\"error\":\"sd open failed\"}");
        return;
    }
    size_t wrote = (g_chunk_used > 0) ? f.write(g_chunk_buf, g_chunk_used) : 0;
    f.flush();
    f.close();
    Serial.printf("[CHUNK] %s @%u +%u%s\n", path.c_str(),
                  (unsigned)offset, (unsigned)wrote,
                  is_final ? " (final)" : "");
    Serial.flush();

    // Keep-alive: do NOT force Connection: close. Closing per chunk burns
    // through lwIP's tiny TIME_WAIT pool (CONFIG_LWIP_MAX_ACTIVE_TCP=16,
    // 2*MSL=120s) after ~14 chunks. Reusing one connection avoids that.
    // The receive-window stall (80 KB cap) doesn't bite here because
    // browser pauses between chunks waiting for this response.
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
            int sp = 0, pg = 0, pct = 0;
            bool ok = read_position_for_name(name, &sp, &pg, &pct);
            char buf[100];
            snprintf(buf, sizeof(buf),
                     "{\"spine\":%d,\"page\":%d,\"percent\":%d,\"saved\":%s}",
                     sp, pg, pct, ok ? "true" : "false");
            req->send(200, "application/json", buf);
        } else if (req->method() == HTTP_POST) {
            int sp = 0, pg = 0, pct = 0, cp = 0;
            // Preserve existing percent + chapter_pages so a hub edit
            // doesn't lose the scaling info or the library progress %.
            int prev_sp = 0, prev_pg = 0;
            read_position_for_name(name, &prev_sp, &prev_pg, &pct, &cp);
            if (req->hasParam("spine", true)) sp = req->getParam("spine", true)->value().toInt();
            if (req->hasParam("page",  true)) pg = req->getParam("page",  true)->value().toInt();
            if (sp < 0) sp = 0;
            if (pg < 0) pg = 0;
            bool ok = write_position_for_name(name, sp, pg, pct, cp);
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
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[WIFI] STA connected");
            g_ap_clients_changed = true;
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("[WIFI] STA disconnected");
            g_ap_clients_changed = true;
            break;
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
    web_server->on("/api/todos", HTTP_GET, ap_handle_todos_get);
    web_server->on("/api/todos", HTTP_POST, ap_handle_todos_post);
    web_server->on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
        Serial.println("[HTTP] GET /ping");
        r->send(200, "text/plain", "pong");
    });
    // /api/upload_chunk — chunked uploader. Empty multipart-upload callback
    // because we use the raw-body path (chunk body is binary, not multipart).
    web_server->on("/api/upload_chunk", HTTP_POST, ap_handle_chunk_done,
                   [](AsyncWebServerRequest *, const String &, size_t,
                      uint8_t *, size_t, bool) {},
                   ap_handle_chunk_body);
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
    g_ap_has_client = false;
    g_ap_clients_changed = false;
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
        // INTENTIONALLY NOT DELETED. Deleting while a request callback is
        // still mid-flight (e.g. the user closed the page mid-upload, or
        // the idle timeout fires during a chunk POST) causes a
        // LoadProhibited NULL-pointer crash inside AsyncWebServer. The
        // ~few-KB leak per AP cycle is harmless on this 8 MB-PSRAM board.
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

// Render the centered logo + "tiny-reader" title into the framebuffer.
// Used by both the cold-boot/wake splash and the deep-sleep screen so
// the device shows the same identity face when off as when starting up.
static void render_splash_to_framebuffer() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    const GFXfont *body = (GFXfont *)&FiraSans;
    const char *title   = "tiny-reader";

    const int GAP_LOGO_TITLE = 40;
    int title_h = 50;   // FiraSans cap height ~ this
    int stack_h = LOGO_H + GAP_LOGO_TITLE + title_h;
    int stack_top = (EPD_HEIGHT - stack_h) / 2;

    int logo_x = (EPD_WIDTH - LOGO_W) / 2;
    int logo_y = stack_top;
    blit_1bit(logo_x, logo_y, LOGO_W, LOGO_H, LOGO_BITMAP, framebuffer);

    int32_t mx = 0, my = 0, mx1, my1, mw, mh;
    get_text_bounds(body, title, &mx, &my, &mx1, &my1, &mw, &mh, NULL);
    int32_t tx = (EPD_WIDTH - mw) / 2;
    int32_t ty = logo_y + LOGO_H + GAP_LOGO_TITLE + title_h;
    writeln(body, title, &tx, &ty, framebuffer);
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

    // ---- Boot splash ----
    // Logo + "tiny-reader", centered. Drawn once on cold boot or
    // wake-from-sleep, then overwritten by the first real render (library
    // or auto-resumed book). Same render is used as the deep-sleep screen
    // — see render_splash_to_framebuffer().
    {
        render_splash_to_framebuffer();
        epd_poweron();
        epd_clear();
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
        epd_poweroff();
    }
    uint32_t splash_started_ms = millis();

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

        // Spin up the background touch poller. Queue holds 8 events;
        // overflow drops oldest taps (better than blocking the polling
        // task and missing real-time press-edges).
        g_touch_queue = xQueueCreate(8, sizeof(TouchTap));
        xTaskCreatePinnedToCore(
            [](void *) {
                bool prev_down = false;
                int16_t last_x = 0, last_y = 0;
                while (true) {
                    int16_t xs[5], ys[5];
                    uint8_t n = touch.getPoint(xs, ys, 1);
                    bool down = (n > 0);
                    if (down) { last_x = xs[0]; last_y = ys[0]; }
                    if (down && !prev_down) {
                        TouchTap t = {last_x, last_y, millis()};
                        if (xQueueSend(g_touch_queue, &t, 0) != pdTRUE) {
                            TouchTap dummy;
                            xQueueReceive(g_touch_queue, &dummy, 0);
                            xQueueSend(g_touch_queue, &t, 0);
                        }
                    }
                    prev_down = down;
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
            },
            "touch_poll", 4096, nullptr, 5, nullptr, 0);
        Serial.println("[TOUCH] background poller started on core 0");
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
    load_todos();

    scan_sd_for_epubs();

    // Hold the splash visible for at least ~2 s of total wall-clock time
    // so the boot screen actually registers (most init steps above are
    // fast). Then clear the screen for the real render that follows.
    {
        const uint32_t SPLASH_MS = 2000;
        uint32_t shown = millis() - splash_started_ms;
        if (shown < SPLASH_MS) delay(SPLASH_MS - shown);
    }
    clear_and_flush();

    // If the last shutdown was from sleep while reading a book, auto-resume.
    // Also pull out the voltage-at-sleep so we can log dV/dt for an average
    // sleep-current estimate. We don't have wall-clock; deep-sleep wake gives
    // us esp_sleep_get_wakeup_cause() but not duration. Using the difference
    // in cell voltage divided by the elapsed seconds (as reported by the RTC
    // SLOW_CLK survives deep sleep) gives a rough mA estimate.
    bool resumed_from_sleep = false;
    {
        Preferences prefs;
        prefs.begin("tinyreader", true);   // read-only
        String last = prefs.getString("last_book", "");
        float v_at_sleep = prefs.getFloat("v_at_sleep", -1.0f);
        prefs.end();
        if (v_at_sleep > 0) {
            float v_now = read_battery_voltage();
            float dv = v_now - v_at_sleep;   // negative = drained
            // Approximate energy ratio per mV — for a 2000 mAh cell discharging
            // 4.2 V → 3.3 V (≈900 mV span), each mV ≈ 2.2 mAh consumed. So:
            //   est_mAh = -dv * 1000 * (2000 / 900)  (only meaningful if dv<0)
            // Average current = est_mAh / hours_slept. We don't know hours, so
            // just emit dv and the inferred draw if possible. Boot-second from
            // millis() approximates wake time (cleared on each boot).
            float boot_sec = millis() / 1000.0f;
            Serial.printf("[BAT] wake: v_at_sleep=%.3f v_now=%.3f dv=%+.3f "
                          "boot=%.1fs\n",
                          v_at_sleep, v_now, dv, boot_sec);
            if (dv < -0.001f) {
                // We can't tell sleep duration from inside firmware (RTC
                // resets unless we set rtc_time_set_us). Just note the drop.
                Serial.printf("[BAT] sleep drain est: %.1f%% capacity used\n",
                              -dv / 0.9f * 100.0f);
            }
        }
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
    // Capture voltage + RTC time at sleep entry so the next boot can compute
    // dV/dt and back-derive average sleep current. Stored in millivolts and
    // milliseconds-since-boot — RTC is reset by deep sleep, but with a known
    // boot epoch in seconds (esp_timer_get_time()) we can reconstruct duration
    // by subtracting at wake.
    {
        float v = read_battery_voltage();
        prefs.putFloat("v_at_sleep", v);
        prefs.putULong64("us_at_sleep", esp_timer_get_time());
        Serial.printf("[SLEEP] battery v=%.3fV stored for dV/dt\n", v);
    }
    prefs.end();

    // Show the same logo + "tiny-reader" splash as the boot screen, so the
    // device looks identical when off, sleeping, and starting up. e-paper
    // holds the image with zero power. Cover-as-screensaver was removed
    // — user preference is for the consistent identity face.
    render_splash_to_framebuffer();
    flush_framebuffer(/*force_full=*/true, "sleep");

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
            } else if (cmd == "todo") {
                app_mode = MODE_TODO;
                render_todo_list();
            } else if (cmd == "share") {
                enter_ap_mode();
            } else if (cmd == "stop_share" || cmd == "unshare") {
                exit_ap_mode();
            } else if (cmd == "sleep") {
                enter_deep_sleep();   // does not return
            } else if (cmd == "bat") {
                float v = read_battery_voltage();
                int p = read_battery_percent();
                Serial.printf("[BAT] v=%.3fV pct=%d charging=%d\n",
                              v, p, (int)g_batt_charging);
            } else if (cmd.startsWith("bat_log ")) {
                // bat_log <interval_sec> <duration_min> — periodic voltage
                // logger for offline analysis. Block-loops; use Ctrl-C to
                // abort, or wait for the duration.
                int sp = cmd.indexOf(' ', 8);
                int interval_sec = cmd.substring(8, sp > 0 ? sp : cmd.length()).toInt();
                int duration_min = sp > 0 ? cmd.substring(sp + 1).toInt() : 60;
                if (interval_sec < 1) interval_sec = 5;
                if (duration_min < 1) duration_min = 60;
                Serial.printf("[BAT] logging every %ds for %d min, "
                              "Ctrl-C to abort\n", interval_sec, duration_min);
                uint32_t end_at = millis() + duration_min * 60UL * 1000UL;
                while (millis() < end_at) {
                    float v = read_battery_voltage();
                    int p = read_battery_percent();
                    update_charging_state();
                    Serial.printf("[BAT] t=%us v=%.3f pct=%d ch=%d\n",
                                  (unsigned)(millis() / 1000), v, p,
                                  (int)g_batt_charging);
                    delay(interval_sec * 1000);
                }
                Serial.println("[BAT] log done");
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
            } else if (cmd.length() == 2 && cmd[0] == 'c' &&
                       cmd[1] >= '1' && cmd[1] <= '4') {
                // c1/c2/c3/c4 — tune clear-cycle count for live A/B testing.
                // Lower = faster page turns, more chance of accumulated ghost.
                g_clear_cycles = cmd[1] - '0';
                Serial.printf("[CFG] clear cycles -> %d\n", g_clear_cycles);
            } else if (cmd == "help") {
                Serial.println("commands: next prev back open <n> goto <ch> dump share stop_share sleep bat bat_log <s> <m> probe_buttons c1..c4 help");
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

    // Auto-sleep: after AUTO_SLEEP_MS of inactivity in any reading-related
    // mode, drop to deep sleep. AP mode is exempt — the user is actively
    // transferring files and may need the SSID/QR on screen for a while.
    // Wrap-safe: if g_last_interact_ms is "in the future" (cross-core
    // millis() race like the AP-idle path saw), treat as fresh activity.
    if (!ap_active && g_last_interact_ms != 0) {
        uint32_t idle_ms = (now >= g_last_interact_ms)
                           ? (now - g_last_interact_ms) : 0;
        if (idle_ms > AUTO_SLEEP_MS) {
            Serial.printf("[SLEEP] auto-sleep after %u ms idle\n",
                          (unsigned)idle_ms);
            enter_deep_sleep();   // does not return
        }
    }

    // STR_100 polling outside renders. The EPD is idle between renders
    // (every render ends with epd_poweroff), so it's safe to briefly flip
    // GPIO 0 to INPUT_PULLUP and sample. We poll every ~100 ms instead of
    // only at render time so press-while-idle actually registers. Skip
    // while AP mode is active — WiFi/AsyncTCP are tetchy about long bursts
    // of pin flipping during traffic, and STR_100 isn't bound there anyway.
    if (!ap_active && !str100_pressed) {
        static uint32_t str100_last_poll = 0;
        if (now - str100_last_poll >= 100) {
            str100_last_poll = now;
            if (sample_str100_button()) {
                Serial.println("[STR_100] press detected (loop poll)");
                str100_pressed = true;
            }
        }
    }

    // ---- AP / web-server mode ----
    if (ap_active) {
        if (dns_server) dns_server->processNextRequest();
        // Redraw the AP screen when station count flips between 0 and >0.
        // Event flag is set from the WiFi event task; we read the actual
        // count here on the main task so the (slow) e-paper redraw doesn't
        // run inside the WiFi callback.
        if (g_ap_clients_changed) {
            g_ap_clients_changed = false;
            bool has_client = WiFi.softAPgetStationNum() > 0;
            if (has_client != g_ap_has_client) {
                g_ap_has_client = has_client;
                render_ap_screen();
            }
        }
        // AsyncWebServer runs its own task; no handleClient() needed.
        // Idle/upload-fresh checks are wrap-safe: ap_last_activity and
        // g_chunk_last_ms are written by AsyncTCP-task callbacks. The
        // loop core can observe a value 1 ms ahead of its own millis()
        // (cross-core read), so a naive `now - ts` would underflow into
        // a 49-day apparent idle. If ts looks "future", treat as 0.
        uint32_t idle_ms = (now >= ap_last_activity)
                           ? (now - ap_last_activity) : 0;
        bool upload_active = (g_chunk_last_ms != 0) &&
                             ((now < g_chunk_last_ms) ||
                              (now - g_chunk_last_ms < 60000UL));
        if (!upload_active && idle_ms > ap_idle_timeout_ms) {
            Serial.printf("[AP] idle timeout (idle=%u ms, limit=%u)\n",
                          (unsigned)idle_ms,
                          (unsigned)ap_idle_timeout_ms);
            exit_ap_mode();
            return;
        }
        // To exit: any short press of the button. We trigger on release
        // (HIGH transition) rather than press so the user can let go and
        // see the device immediately leave AP. First 3 s after entering
        // ignore the button so the long-press that entered AP doesn't
        // auto-exit on its own release.
        if (now - ap_started_at > 3000) {
            static bool btn_was_low = false;
            static uint32_t btn_press_at = 0;
            bool now_low = (digitalRead(BUTTON_1) == LOW);
            if (now_low && !btn_was_low) {
                btn_press_at = now;
            } else if (!now_low && btn_was_low) {
                // 50 ms minimum to debounce contact bounce.
                if (now - btn_press_at > 50) {
                    Serial.println("[AP] button-press exit");
                    btn_was_low = false;
                    exit_ap_mode();
                    input_cooldown_until = millis() + 1000;
                    return;
                }
            }
            btn_was_low = now_low;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        return;
    }

    // touch — drained from the background poller's queue. Each TouchTap
    // is one press-edge captured by the poller, including taps that
    // happened while loop() was blocked inside an EPD waveform. Walk
    // every queued tap so a burst of fast taps during a long render
    // still gets through.
    if (touchOnline && g_touch_queue) {
        TouchTap tap;
        while (xQueueReceive(g_touch_queue, &tap, 0) == pdTRUE) {
            // Debounce against the tap's capture time, not millis() — a
            // tap queued during a 1 s render must not be discarded just
            // because the render itself outlasted any cooldown set from
            // the *previous* tap.
            if (tap.t_ms < input_cooldown_until) continue;
            int16_t xs[1] = { tap.x };
            int16_t ys[1] = { tap.y };
            g_last_interact_ms = tap.t_ms;
            {
                int zone = classify_tap(xs[0], ys[0]);
                Serial.printf("tap (%d,%d) zone=%d mode=%d\n",
                              xs[0], ys[0], zone, app_mode);
                Serial.flush();
                // Visual bottom-right corner: enters TODO view from
                // library, exits TODO back to library. Touch Y origin is
                // bottom-left of the screen, so visual bottom = low Y.
                bool corner_right = (xs[0] > EPD_WIDTH - 200 &&
                                     ys[0] < 100);
                if (app_mode == MODE_TODO) {
                    // Top edge or bottom-right corner exits.
                    if (ys[0] > EPD_HEIGHT - 80 || corner_right) {
                        Serial.println("[TAP] exit TODO -> library");
                        app_mode = MODE_LIBRARY;
                        render_book_list();
                    } else {
                        int row = todo_hit_test(xs[0], ys[0]);
                        if (row >= 0) {
                            g_todos[row].done = !g_todos[row].done;
                            Serial.printf("[TODO] toggle row %d -> %s\n",
                                          row, g_todos[row].done ? "done" : "open");
                            save_todos();
                            render_todo_list();
                        }
                    }
                } else if (app_mode == MODE_BOOKMARK_LIST) {
                    if (ys[0] > EPD_HEIGHT - 80) {
                        // Top strip → cancel back to book.
                        app_mode = MODE_BOOK;
                        render_book_page();
                    } else {
                        int idx = bookmark_hit_test(xs[0], ys[0]);
                        if (idx >= 0) {
                            int sp = g_bookmarks[idx].first;
                            int pg = g_bookmarks[idx].second;
                            Serial.printf("[BM] jump to (ch=%d p=%d)\n", sp, pg);
                            if (sp >= 0 && sp < total_spine && load_chapter(sp)) {
                                if (pg >= 0 && pg < (int)chapter_pages.size())
                                    current_page_in_chapter = pg;
                                app_mode = MODE_BOOK;
                                render_book_page();
                                save_position();
                            }
                        }
                    }
                } else if (app_mode == MODE_BOOK_END) {
                    if (xs[0] < EPD_WIDTH / 2 && ys[0] < EPD_HEIGHT - 80) {
                        app_mode = MODE_BOOK;
                        render_book_page();
                    } else {
                        app_mode = MODE_LIBRARY;
                        render_book_list();
                    }
                } else if (app_mode == MODE_CHAPTER_JUMP) {
                    if (ys[0] > EPD_HEIGHT - 80) {
                        // Top strip: paginate or cancel.
                        int total = (int)g_toc_cache.size();
                        int total_pages =
                            (total + CJ_ROWS_PER_PAGE - 1) / CJ_ROWS_PER_PAGE;
                        if (total_pages > 1 && xs[0] < 200) {
                            cj_first -= CJ_ROWS_PER_PAGE;
                            if (cj_first < 0) cj_first = 0;
                            render_chapter_jump();
                        } else if (total_pages > 1 &&
                                   xs[0] > EPD_WIDTH - 200) {
                            int next = cj_first + CJ_ROWS_PER_PAGE;
                            if (next < total) {
                                cj_first = next;
                                render_chapter_jump();
                            }
                        } else {
                            app_mode = MODE_BOOK;
                            render_book_page();
                        }
                    } else {
                        int toc_idx = chapter_jump_hit_test(xs[0], ys[0]);
                        if (toc_idx >= 0 && toc_idx < (int)g_toc_cache.size()) {
                            int sp = g_toc_cache[toc_idx].spine_index;
                            const std::string &title = g_toc_cache[toc_idx].title;
                            Serial.printf("[CJ] jump to toc[%d] sp=%d\n",
                                          toc_idx, sp);
                            if (sp >= 0 && sp < total_spine && load_chapter(sp)) {
                                // Single-spine Gutenberg books: many TOC entries
                                // share the same spine item; the actual chapter
                                // is an anchor inside it. Find the page where
                                // the chapter title text appears.
                                if (!title.empty() && chapter_pages.size() > 1) {
                                    for (size_t p = 0; p < chapter_pages.size(); ++p) {
                                        if (chapter_pages[p].find(title)
                                            != std::string::npos) {
                                            current_page_in_chapter = (int)p;
                                            Serial.printf("[CJ] title found on p=%u\n",
                                                          (unsigned)p);
                                            break;
                                        }
                                    }
                                }
                                app_mode = MODE_BOOK;
                                render_book_page();
                                save_position();
                            } else {
                                app_mode = MODE_BOOK;
                                render_book_page();
                            }
                        }
                    }
                } else if (app_mode == MODE_BOOK) {
                    // Touch coord origin is bottom-left of screen, so the
                    // user's *top* strip is high Y, not low Y.
                    bool top_strip = (ys[0] > EPD_HEIGHT - 80);
                    bool top_right_corner = (xs[0] > EPD_WIDTH - 200 &&
                                             ys[0] > EPD_HEIGHT - 100);
                    bool bottom_left_corner = (xs[0] < 100 && ys[0] < 80);
                    bool bottom_right_corner = (xs[0] > EPD_WIDTH - 200 &&
                                                ys[0] < 100);
                    if (top_right_corner) {
                        Serial.println("[TAP] top-right → toggle bookmark");
                        toggle_bookmark_on_current_page();
                        render_book_page();
                    } else if (bottom_right_corner) {
                        Serial.println("[TAP] bottom-right → bookmark list");
                        bm_first = 0;
                        app_mode = MODE_BOOKMARK_LIST;
                        render_bookmark_list();
                    } else if (top_strip) {
                        Serial.println("[TAP] top → back to library");
                        app_mode = MODE_LIBRARY;
                        render_book_list();
                    } else if (bottom_left_corner) {
                        // Bottom-left corner = open chapter jump list.
                        Serial.println("[TAP] bottom-left → chapter jump");
                        // Scroll to the page containing the current TOC entry,
                        // if any, so the user lands near where they are.
                        int cur_toc = -1, best_si = -1;
                        for (size_t i = 0; i < g_toc_cache.size(); ++i) {
                            if (g_toc_cache[i].spine_index <= current_spine &&
                                g_toc_cache[i].spine_index > best_si) {
                                best_si = g_toc_cache[i].spine_index;
                                cur_toc = (int)i;
                            }
                        }
                        cj_first = (cur_toc < 0) ? 0
                                 : (cur_toc / CJ_ROWS_PER_PAGE) * CJ_ROWS_PER_PAGE;
                        app_mode = MODE_CHAPTER_JUMP;
                        render_chapter_jump();
                    } else if (xs[0] < EPD_WIDTH / 2) book_prev_page();
                    else book_next_page();
                } else {  // MODE_LIBRARY
                    if (corner_right) {
                        Serial.println("[TAP] bottom-right -> TODO view");
                        app_mode = MODE_TODO;
                        render_todo_list();
                    } else if (zone == 1) move_selection(-1);
                    else if (zone == 3) move_selection(+1);
                    else if (zone == 2) open_selected();
                }
                // Debounce window measured from the originating tap, not
                // from "now" (which is post-render). 300 ms keeps stray
                // finger-jitter from registering twice but lets the next
                // queued tap dispatch immediately.
                input_cooldown_until = tap.t_ms + 300;
            }
            // (loop body of `while (queue receive)` ends here)
        }
    }

    // GPIO 21 (SENSOR_VN, BUTTON_1): short press in book mode = back to library;
    //                                short press in library = cycle selection;
    //                                long press 2-5 s on release = WiFi share mode;
    //                                very long press ≥ 5 s = deep sleep (immediate).
    {
        bool down = (digitalRead(BUTTON_1) == LOW);
        if (down) g_last_interact_ms = now;
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
                } else if (app_mode == MODE_CHAPTER_JUMP) {
                    // Cancel back to book
                    app_mode = MODE_BOOK;
                    render_book_page();
                    input_cooldown_until = now + 600;
                } else if (app_mode == MODE_BOOK || app_mode == MODE_TODO ||
                           app_mode == MODE_BOOK_END) {
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
    //
    // In book mode, STR_100 cycles text density (compact → medium → loose).
    // The current chapter is re-paginated and we keep the user roughly where
    // they were by scaling page index by the old/new page counts.
    if (str100_pressed) {
        str100_pressed = false;
        if (now >= input_cooldown_until) {
            if (app_mode == MODE_BOOK) {
                int prev_pages = (int)chapter_pages.size();
                int prev_page  = current_page_in_chapter;
                g_settings.density = (g_settings.density + 1) % 3;
                apply_density();
                save_settings();
                Serial.printf("[STR_100] density=%d, re-paginating ch %d\n",
                              g_settings.density, current_spine);
                if (load_chapter(current_spine)) {
                    int new_pages = (int)chapter_pages.size();
                    if (prev_pages > 0 && new_pages > 0) {
                        int np = (int)((int64_t)prev_page * new_pages / prev_pages);
                        if (np < 0) np = 0;
                        if (np >= new_pages) np = new_pages - 1;
                        current_page_in_chapter = np;
                    }
                    render_book_page();
                    save_position();
                }
                input_cooldown_until = millis() + 600;
            } else {
                Serial.println("[STR_100] consumed (no-op outside book mode)");
            }
        }
    }

    // Light sleep was tried here (esp_light_sleep_start with GPIO/timer
    // wake) but it interacts badly with USB-CDC enumeration during
    // flashing — esptool fails with OSError 71 when the chip is
    // sleep-cycling. Detection via `!Serial` wasn't reliable enough.
    // Leaving the busy-wait for now; deep sleep on long-press still works.
    vTaskDelay(pdMS_TO_TICKS(20));
}
