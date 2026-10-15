#pragma once

#include <algorithm>

namespace Star {

// A jump can spend the grace period once. A successful jump must leave the
// ground before ground contact can arm it again (onGround can lag a tick).
class JumpGrace {
public:
  void setDuration(float duration) {
    m_duration = std::max(0.0f, duration);
    m_remaining = std::min(m_remaining, m_duration);
  }

  void update(float dt, bool grounded) {
    if (!grounded)
      m_waitForAir = false;
    if (grounded && !m_waitForAir)
      m_remaining = m_duration;
    else
      m_remaining = std::max(0.0f, m_remaining - dt);
  }

  bool available() const { return m_remaining > 0.0f; }

  void consume() {
    m_remaining = 0.0f;
    m_waitForAir = true;
  }

  void reset() {
    m_remaining = 0.0f;
    m_waitForAir = false;
  }

private:
  float m_duration = 0.0f;
  float m_remaining = 0.0f;
  bool m_waitForAir = false;
};

}
