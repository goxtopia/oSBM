#pragma once

#include <cstdint>

namespace Star {

// Pending long presses tolerate finger jitter but restart after a drag.
class TouchHold {
public:
  void begin(int64_t nowMs, float x, float y) {
    m_startMs = nowMs;
    m_x = x;
    m_y = y;
  }

  void move(int64_t nowMs, float x, float y, float tolerance) {
    float dx = x - m_x, dy = y - m_y;
    if (dx * dx + dy * dy > tolerance * tolerance)
      begin(nowMs, x, y);
  }

  bool ready(int64_t nowMs, int64_t durationMs) const {
    return nowMs - m_startMs >= durationMs;
  }

private:
  int64_t m_startMs = 0;
  float m_x = 0.0f, m_y = 0.0f;
};

}
