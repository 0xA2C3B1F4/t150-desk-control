"""Optional timing diagnostics in generated host code. No timeout changes."""
import difflib


def instrument(work):
    path = work / "build/src/esphome/components/desk_uart/desk_uart.cpp"
    original = path.read_text()
    source = original
    old = "void DeskUART::loop() {"
    new = old + '''
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
'''
    assert source.count(old) == 1
    source = source.replace(old, new)
    old = '''if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750) {
      this->invalidate_child_lock_("CL01 UART unavailable; state unknown");'''
    new = '''if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750) {
      if (!timing_rx_reported) {
        timing_rx_reported = true;
        ESP_LOGI("uart_timing", "invalidate site=rx now=%u height_at=%u age=%u loop_gap=%u rx=%u known=%d",
                 now, this->last_height_received_, now - this->last_height_received_, timing_gap,
                 unsigned(this->available()), this->child_lock_.state_known());
      }
      this->invalidate_child_lock_("CL01 UART unavailable; state unknown");'''
    assert source.count(old) == 1
    source = source.replace(old, new)
    old = 'if (!fresh) this->invalidate_child_lock_("CL01 UART unavailable; state unknown");'
    new = '''if (!fresh) {
    if (this->child_lock_.state_known())
      ESP_LOGI("uart_timing", "invalidate site=child_lock now=%u height_at=%u age=%u rx=%u seen=%d monitor_fresh=%d",
               now, this->last_height_received_, now - this->last_height_received_, unsigned(this->available()),
               this->height_seen_, this->monitor_.fresh(now));
    this->invalidate_child_lock_("CL01 UART unavailable; state unknown");
  }'''
    assert source.count(old) == 1
    source = source.replace(old, new)
    if path.is_symlink():
        path.unlink()
    path.write_text(source)
    (work / "timing-instrumentation.diff").write_text("".join(difflib.unified_diff(
        original.splitlines(True), source.splitlines(True), fromfile="production/desk_uart.cpp",
        tofile="generated-host/desk_uart.cpp")))
