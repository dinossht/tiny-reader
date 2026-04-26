# tiny-reader backlog

Loose ideas not yet built. Numbered for stable reference from code comments.

## Open

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
- **#15** — STR_100 (GPIO 0) button: post-render sample + loop-poll. Now
  cycles text density in book mode.
