# tiny-reader backlog

Loose ideas not yet built. Numbered for stable reference from code comments.
Sources for the typography/UX/e-paper items: research surveys of KOReader,
foliate-js, atomic14/diy-esp32-epub-reader, Plato, Inkplate, epdiy.

## Open — high impact

- **#17 — Bold / italic rendering.** Body fonts at 22 pt regular / bold /
  italic / bold-italic are already generated (`freesans_body{,_bold,_italic,
  _bolditalic}.h`). Plumb through: (a) extend `strip_xhtml` to emit a parallel
  `runs[]` array of `{start_offset, length, flags}` where flags is BOLD/ITALIC;
  (b) update `wrap_text` and `render_book_page_text` to switch the font per run.
  Big visual jump.
- **#18 — Partial refresh + diff framebuffer.** Switch render exit from
  `epd_draw_grayscale_image` to `epd_hl_update_screen` with `MODE_GL16` for
  page turns and `MODE_GC16` every 6th turn. Expect ~10× faster turns. Risky
  if the rendering core path has any bug; do in isolation, with a fallback
  path. Reference: `epdiy/src/highlevel.c:219-339`.
- **#19 — Locations index in `.pos` sidecar.** Pre-walk each book once, emit
  a stable marker every ~1500 chars. Gives a "page X of Y" that survives
  density changes. Port from foliate-js `progress.js` (~100 lines).

## Open — medium impact

- **#20 — Dogear bookmark.** Reserve a top-right tap zone for "toggle
  bookmark on this page"; render a folded-corner triangle. Persist a list of
  bookmarks in the `.pos` sidecar (extend format). Add a Bookmarks tab in
  chapter-jump.
- **#21 — AlgoHyph hyphenation** (vowel-consonant heuristic, ~120 lines, no
  data file). Lets justified text wrap cleaner without huge word-gaps. From
  crengine `hyphman.cpp:1083-1180`.
- **#22 — Cover thumbnails in library.** Extract cover from EPUB manifest
  (`<meta name="cover">` → manifest item), decode JPEG with TJpgDec or PNG
  with PNGdec (atomic14 already vendors both), render at ~140×200 left of
  title. Cache the decoded thumb on SD as a sidecar so we don't re-decode.
- **#23 — Light sleep between page turns.** `esp_light_sleep_start()` with
  GPIO wake on touch INT and BUTTON_1. Drops idle ~30 mA → ~1 mA. Tricky:
  must guard against AsyncTCP being on the same core, PSRAM re-init, EPD
  power state. Disable while AP active.
- **#24 — Diagonal-swipe → forced full refresh.** Refactor touch from press-
  to release-triggered, detect dx/dy delta with one threshold for "tap" vs.
  "swipe". On diagonal swipe, force a `MODE_GC16` clean. Solves "screen
  looks ghosty" complaints. From KOReader gestures plugin.

## Open — polish

- **#25 — Empty-state with hub URL + QR code.** If `books.empty()`, render
  a small QR pointing at `http://192.168.4.1` + a friendly "no books yet"
  hint. Tiny QR encoder (~3 KB).
- **#26 — Cover-as-screensaver.** When entering deep sleep, blit the
  current book's cover and turn the EPD off. E-paper holds the image for
  free. Replaces the splash on next wake. Depends on #22.
- **#27 — Reading-time estimate.** "~3 h left" on the library row, computed
  from words/page × words-per-minute (e.g., 250 WPM). Show only for books
  in progress.
- **#28 — Bundled `quickstart.epub`.** Ship a 4-page welcome book in flash;
  open it automatically the first time the library is empty. From KOReader.
- **#29 — Footer indicator: thin progress line.** Below the chapter/page/
  battery row, a 1-px line spanning the screen with the filled portion =
  current book progress. From KOReader.

## Won't do (researched, not worth)

- **Knuth-Plass line breaking** — quality bump small once we have hyphenation,
  complexity high.
- **Drop caps** — work-to-impact bad without fully styled rendering.
- **stb_truetype runtime fonts** — only worth it for CJK / dynamic sizing,
  neither needed.
- **Streaming EPUB parsing** — spine items are 20-200 KB, no point with 8 MB
  PSRAM.
- **Liang hyphenation patterns** — 31 KB data file + complex matching;
  AlgoHyph wins on effort.
- **Dark mode / inversion** — e-paper inversion ghosts heavily, slow refresh;
  not pleasant on this hardware.
- **Highlights** — needs long-press + drag selection on touch + render
  cost; impractical on e-paper.

## Open — battery / hardware

- **#16 — Battery / power estimation.** No current sensor on the board (LilyGo
  V2.3 has only a voltage divider on GPIO14). Possible follow-ups:
  - Sleep-current estimate: log battery voltage at deep-sleep entry and
    again on wake; compute `dV / dt`, multiply by approximate battery
    capacity to get average µA during sleep.
  - Voltage-over-time logger via serial command (every N seconds, until
    stopped) — useful to compare AP vs. reading vs. idle.
  - Better SoC mapping: replace the linear `analogRead → percent` with a
    LiPo discharge-curve lookup table.

## Done (kept for context)

- **#11** — Hub-managed TODO list persisted on SD.
- **#15** — STR_100 (GPIO 0) button: post-render sample + loop-poll. Cycles
  text density in book mode.
- **#30** — Smart quotes / em-dash / ellipsis (font regen + entity table +
  ASCII substitution).
- **#31** — First-line paragraph indent.
- **#32** — Tri-state library pill (hollow / bar / filled).
- **#33** — Bigger logo + 32 pt title font in headers.
