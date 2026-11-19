"""Optional timing diagnostics in generated host code. No timeout changes."""

import difflib


def instrument(work):
    """Add timing logs to generated host UART code and write a diagnostic diff.

    Production sources and existing timeout thresholds remain unchanged.
    """
    component = work / "build/src/esphome/components/desk_uart"
    path = component / "desk_uart.cpp"
    original = path.read_text()
    source = original
    old = "void DeskUART::loop() {"
    new = (
        old
        + """
  // Diagnostic host copy only: sample scheduling and UART freshness without
  // changing the decisions or their existing 750/3000 ms thresholds.
  const uint32_t timing_now = millis();
  static uint32_t timing_previous = 0, timing_report = 0;
  const uint32_t timing_gap = timing_previous ? timing_now - timing_previous : 0;
  timing_previous = timing_now;
  bool timing_rx_reported = false;
  if (timing_gap > 200 || timing_now - timing_report >= 250) {
    timing_report = timing_now;
    ESP_LOGI("uart_timing", "loop now=%u gap=%u height_at=%u age=%u seen=%d fresh=%d rx=%u errors=%u",
             timing_now, timing_gap, this->last_height_received_,
             timing_now - this->last_height_received_, this->height_seen_,
             this->monitor_.fresh(timing_now), unsigned(this->available()), unsigned(this->frame_errors_));
  }
"""
    )
    assert source.count(old) == 1
    source = source.replace(old, new)
    # Match only the condition so other invalidations added to this branch,
    # such as the RS01 reset-speed one, do not break the anchor.
    old = "if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750) {"
    new = (
        old
        + """
      if (!timing_rx_reported) {
        timing_rx_reported = true;
        ESP_LOGI("uart_timing", "invalidate site=rx now=%u height_at=%u age=%u loop_gap=%u rx=%u known=%d",
                 now, this->last_height_received_, now - this->last_height_received_, timing_gap,
                 unsigned(this->available()), this->panel_lock_.state_known());
      }"""
    )
    assert source.count(old) == 1
    source = source.replace(old, new)
    patched = [(path, original, source)]
    # The third anchor is in panel_lock_loop_(), in the panel-lock feature file.
    path = component / "panel_lock_controls.cpp"
    original = path.read_text()
    source = original
    old = """if (!fresh)
    this->invalidate_panel_lock_("No recent controller reply; panel-lock state unknown");"""
    new = """if (!fresh) {
    if (this->panel_lock_.state_known())
      ESP_LOGI("uart_timing", "invalidate site=panel_lock now=%u height_at=%u age=%u rx=%u seen=%d monitor_fresh=%d",
               now, this->last_height_received_, now - this->last_height_received_, unsigned(this->available()),
               this->height_seen_, this->monitor_.fresh(now));
    this->invalidate_panel_lock_("No recent controller reply; panel-lock state unknown");
  }"""
    assert source.count(old) == 1
    source = source.replace(old, new)
    patched.append((path, original, source))
    diff = []
    for path, original, source in patched:
        if path.is_symlink():
            path.unlink()
        path.write_text(source)
        diff += difflib.unified_diff(
            original.splitlines(True),
            source.splitlines(True),
            fromfile=f"production/{path.name}",
            tofile=f"generated-host/{path.name}",
        )
    (work / "timing-instrumentation.diff").write_text("".join(diff))
