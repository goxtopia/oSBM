#include "StarJumpGrace.hpp"
#include "../application/mobile/StarTouchHold.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(JumpGraceTest, RequiresGroundContactAndExpires) {
  JumpGrace grace;
  grace.setDuration(0.12f);
  grace.update(0.01f, false);
  EXPECT_FALSE(grace.available());
  grace.update(0.01f, true);
  grace.update(0.1f, false);
  EXPECT_TRUE(grace.available());
  grace.update(0.021f, false);
  EXPECT_FALSE(grace.available());
}

TEST(JumpGraceTest, JumpCannotRearmUntilLanding) {
  JumpGrace grace;
  grace.setDuration(0.12f);
  grace.update(0.01f, true);
  grace.consume();
  grace.update(0.01f, true); // stale onGround on the tick a jump starts
  EXPECT_FALSE(grace.available());
  grace.update(0.01f, false);
  EXPECT_FALSE(grace.available());
  grace.update(0.01f, true);
  EXPECT_TRUE(grace.available());
}

TEST(JumpGraceTest, DisableAndWorldResetClearGrace) {
  JumpGrace grace;
  grace.setDuration(0.12f);
  grace.update(0.01f, true);
  grace.setDuration(0.0f);
  EXPECT_FALSE(grace.available());
  grace.update(0.01f, true);
  EXPECT_FALSE(grace.available());
  grace.setDuration(0.12f);
  grace.update(0.01f, true);
  grace.reset();
  grace.update(0.01f, false);
  EXPECT_FALSE(grace.available());
}

TEST(TouchHoldTest, UsesConfiguredDurationAndToleratesJitter) {
  TouchHold hold;
  hold.begin(1000, 10.0f, 20.0f);
  hold.move(1200, 12.0f, 19.0f, 5.0f);
  EXPECT_FALSE(hold.ready(1449, 450));
  EXPECT_TRUE(hold.ready(1450, 450));
  EXPECT_FALSE(hold.ready(1450, 800));
}

TEST(TouchHoldTest, DragAndInvalidTargetRestartTheWait) {
  TouchHold hold;
  hold.begin(1000, 10.0f, 20.0f);
  hold.move(1400, 30.0f, 20.0f, 5.0f);
  EXPECT_FALSE(hold.ready(1450, 450));
  EXPECT_TRUE(hold.ready(1850, 450));
  hold.begin(1850, 30.0f, 20.0f); // target left range / became UI
  EXPECT_FALSE(hold.ready(2000, 450));
}
