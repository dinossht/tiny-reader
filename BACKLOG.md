# tiny-reader backlog

Loose ideas not yet built. Numbered for stable reference from code comments.
Sources for the typography/UX/e-paper items: research surveys of KOReader,
foliate-js, atomic14/diy-esp32-epub-reader, Plato, Inkplate, epdiy.

## Open

- **#24 — Diagonal-swipe → forced full refresh.** Refactor touch from press-
  to release-triggered, detect dx/dy delta with one threshold for "tap" vs.
  "swipe". On diagonal swipe, force a `MODE_GC16` clean. Solves "screen
  looks ghosty" complaints. From KOReader gestures plugin.
- **#28 — Bundled `quickstart.epub`.** Ship a 4-page welcome book in flash;
  open it automatically the first time the library is empty. From KOReader.
- **#16 — Battery / power estimation.** No current sensor on the board (LilyGo
  V2.3 has only a voltage divider on GPIO14). Possible follow-ups:
  - Sleep-current estimate: log battery voltage at deep-sleep entry and
    again on wake; compute `dV / dt`, multiply by approximate battery
    capacity to get average µA during sleep.
  - Voltage-over-time logger via serial command (every N seconds, until
    stopped) — useful to compare AP vs. reading vs. idle.
  - Better SoC mapping: replace the linear `analogRead → percent` with a
    LiPo discharge-curve lookup table.

## Deferred (tried or scoped, not worth right now)

- **#18 — Partial refresh + diff framebuffer.** Would need vendoring ~1000
  lines of epdiy's hi-level API + waveform infrastructure
  (`epd_hl_update_screen`, mode tables, diff-framebuffer logic). Our LilyGo
  lib only exposes the low-level `epd_draw_grayscale_image`, which always
  does a full update. Tried "skip epd_clear() on page turns" as a poor-man's
  substitute — content layers on top, looks terrible without a true diff
  path. Park until someone's willing to port epdiy hi-level. Reference:
  `epdiy/src/highlevel.c:219-339`.
- **#23 — Light sleep between page turns.** Tried, reverted: USB-CDC
  enumeration during sleep cycles caused esptool `OSError 71` during
  flashing, and `!Serial` detection wasn't reliable enough to gate it.
  Re-attempt would need a hard-wired "disable light sleep" jumper or a
  long-press to disarm during dev.

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

## Done (kept for context)

- **#11** — Hub-managed TODO list persisted on SD.
- **#15** — STR_100 (GPIO 0) button: cycles text density in book mode.
- **#17** — Bold / italic rendering through XHTML strip + 4 font variants
  (regular / bold / italic / bold-italic), inline style markers in stripped
  text.
- **#19** — Locations index in `.pos` sidecar; "page X of Y" survives
  density changes (rescaled on reload when chapter_pages differs).
- **#20** — Dogear bookmark: top-right tap toggles, folded-corner sprite,
  bottom-right opens a Bookmarks list. Persisted in `<book>.bm`.
- **#21** — AlgoHyph algorithmic hyphenation (vowel-consonant heuristic,
  ~50 lines, no data file).
- **#22** — Cover thumbnails: vendored TJpgDec, decoded EPUB cover JPEG,
  cached at 80×110 4-bit on SD as `<book>.thumb`, blitted at half-scale in
  the library.
- **#25** — Empty-library state with WiFi-credentials QR + 3-step instructions.
- **#26** — Cover-as-screensaver: deep-sleep blits the current book's cached
  thumb 4× upscaled with the title underneath.
- **#27** — Reading-time estimate ("~Xh") on each library row.
- **#29** — Footer thin progress line: 1-px filled bar at the bottom of the
  page showing book-level progress.
- **#30** — Smart quotes / em-dash / ellipsis (font regen with U+2010–U+2027
  glyph range + entity table + ASCII `--`/`...` substitution).
- **#31** — First-line paragraph indent.
- **#32** — Tri-state library pill (hollow / bar / filled).
- **#33** — Bigger logo + 32 pt title font in headers.
- **#34** — AP-on-demand share screen with WiFi-credentials QR + live
  "Connected" status indicator below the QR.
- **#35** — Compact-density font typography fix: regenerated
  `freesans_body_small` with the smart-quote interval so curly apostrophes
  no longer get silently dropped at compact density.
