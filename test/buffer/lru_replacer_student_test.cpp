#include "buffer/lru_replacer.h"

#include "gtest/gtest.h"

// =============================================================================
// LRUReplacer 补充测试（边界条件与错误路径）
// =============================================================================

TEST(LRUReplacerStudentTest, VictimOnEmptyReturnsFalse) {
  LRUReplacer replacer(5);
  frame_id_t frame_id;
  // 没有任何 Unpin 过的 frame，Victim 应返回 false
  ASSERT_FALSE(replacer.Victim(&frame_id));
  ASSERT_EQ(0, replacer.Size());
}

TEST(LRUReplacerStudentTest, PinFrameNotInListIsNoOp) {
  LRUReplacer replacer(5);
  // Pin 一个从未 Unpin 过的 frame，应该是空操作，不崩溃
  replacer.Pin(10);
  replacer.Pin(100);
  ASSERT_EQ(0, replacer.Size());
}

TEST(LRUReplacerStudentTest, UnpinAlreadyUnpinnedFrame) {
  LRUReplacer replacer(5);
  replacer.Unpin(3);
  ASSERT_EQ(1, replacer.Size());
  // 重复 Unpin —— 防御性忽略，Size 不变
  replacer.Unpin(3);
  ASSERT_EQ(1, replacer.Size());
}

TEST(LRUReplacerStudentTest, PinThenUnpinThenVictim) {
  LRUReplacer replacer(5);
  replacer.Unpin(0);
  replacer.Unpin(1);
  ASSERT_EQ(2, replacer.Size());

  // Pin 一个在队列中的 frame
  replacer.Pin(0);
  ASSERT_EQ(1, replacer.Size());

  // Unpin 回来后放在队尾
  replacer.Unpin(0);
  ASSERT_EQ(2, replacer.Size());

  // 此时顺序：1（最老）、0（最新）
  frame_id_t victim;
  ASSERT_TRUE(replacer.Victim(&victim));
  ASSERT_EQ(1, victim);  // 最老的被淘汰

  ASSERT_TRUE(replacer.Victim(&victim));
  ASSERT_EQ(0, victim);

  // 队列空了
  ASSERT_EQ(0, replacer.Size());
  ASSERT_FALSE(replacer.Victim(&victim));
}

TEST(LRUReplacerStudentTest, VictimExhaustsList) {
  LRUReplacer replacer(3);
  replacer.Unpin(5);
  replacer.Unpin(6);
  replacer.Unpin(7);

  frame_id_t f;
  ASSERT_TRUE(replacer.Victim(&f));
  ASSERT_EQ(5, f);
  ASSERT_EQ(2, replacer.Size());

  ASSERT_TRUE(replacer.Victim(&f));
  ASSERT_EQ(6, f);
  ASSERT_EQ(1, replacer.Size());

  ASSERT_TRUE(replacer.Victim(&f));
  ASSERT_EQ(7, f);
  ASSERT_EQ(0, replacer.Size());

  ASSERT_FALSE(replacer.Victim(&f));  // 空
}

TEST(LRUReplacerStudentTest, SizeTracksCorrectly) {
  LRUReplacer replacer(10);
  ASSERT_EQ(0, replacer.Size());

  replacer.Unpin(1);
  ASSERT_EQ(1, replacer.Size());

  replacer.Unpin(2);
  ASSERT_EQ(2, replacer.Size());

  replacer.Pin(1);
  ASSERT_EQ(1, replacer.Size());

  replacer.Pin(2);
  ASSERT_EQ(0, replacer.Size());

  // Pin 一个不在队列中的
  replacer.Pin(99);
  ASSERT_EQ(0, replacer.Size());
}

TEST(LRUReplacerStudentTest, FrameIdZeroWorks) {
  LRUReplacer replacer(3);
  // frame_id = 0 是合法值，应正常工作
  replacer.Unpin(0);
  ASSERT_EQ(1, replacer.Size());

  frame_id_t f;
  ASSERT_TRUE(replacer.Victim(&f));
  ASSERT_EQ(0, f);
}
