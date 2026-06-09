#include "buffer/buffer_pool_manager.h"

#include <cstring>

#include "gtest/gtest.h"

// =============================================================================
// BufferPoolManager 补充测试（DeletePage、错误路径、脏页淘汰验证）
// =============================================================================

TEST(BufferPoolManagerStudentTest, DeletePageRemovesFromMemoryAndDisk) {
  const std::string db_name = "bpm_student_delete.db";
  const size_t pool_size = 5;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  // 创建三个页
  page_id_t pid0, pid1, pid2;
  auto *p0 = bpm->NewPage(pid0);
  auto *p1 = bpm->NewPage(pid1);
  auto *p2 = bpm->NewPage(pid2);
  ASSERT_NE(nullptr, p0);
  ASSERT_NE(nullptr, p1);
  ASSERT_NE(nullptr, p2);

  // 写入识别数据
  const char marker = 0x5A;
  memset(p0->GetData(), marker, PAGE_SIZE);
  memset(p1->GetData(), marker + 1, PAGE_SIZE);
  memset(p2->GetData(), marker + 2, PAGE_SIZE);

  // Unpin 全部，让它们可淘汰
  ASSERT_TRUE(bpm->UnpinPage(pid2, true));
  ASSERT_TRUE(bpm->UnpinPage(pid1, true));
  ASSERT_TRUE(bpm->UnpinPage(pid0, true));

  // 删除 pid1
  ASSERT_TRUE(bpm->DeletePage(pid1));

  // pid1 在磁盘上应标记为空闲
  ASSERT_TRUE(dm->IsPageFree(pid1));

  // pid1 不应再在内存中 —— FlushPage 返回 false
  ASSERT_FALSE(bpm->FlushPage(pid1));

  // pid0 和 pid2 不受影响
  ASSERT_FALSE(dm->IsPageFree(pid0));
  ASSERT_FALSE(dm->IsPageFree(pid2));

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, DeletePageInUseRejected) {
  const std::string db_name = "bpm_student_delete_inuse.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  page_id_t pid;
  auto *p = bpm->NewPage(pid);
  ASSERT_NE(nullptr, p);
  // 刚创建，pin_count = 1，不能删除
  ASSERT_FALSE(bpm->DeletePage(pid));

  bpm->UnpinPage(pid, false);
  // pin_count = 0，可以删除
  ASSERT_TRUE(bpm->DeletePage(pid));

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, NewPageAfterDeleteReusesFrame) {
  const std::string db_name = "bpm_student_reuse.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  // 用满三个槽位
  page_id_t pid0, pid1, pid2;
  bpm->NewPage(pid0);
  bpm->NewPage(pid1);
  bpm->NewPage(pid2);

  // 释放中间那个，归还 free_list_
  bpm->UnpinPage(pid1, false);
  ASSERT_TRUE(bpm->DeletePage(pid1));

  // 再 NewPage —— 应从 free_list_ 拿到刚归还的槽位，而非淘汰
  // 磁盘上 pid1 已被释放，AllocatePage 会回收该页号
  page_id_t pid3;
  auto *p3 = bpm->NewPage(pid3);
  ASSERT_NE(nullptr, p3);
  ASSERT_EQ(pid1, pid3);  // 回收了刚释放的页号

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, FetchNonExistentPageWhenPoolFull) {
  const std::string db_name = "bpm_student_fetch_full.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  // 用满缓冲池，全部 pin 着
  page_id_t pid0, pid1, pid2;
  bpm->NewPage(pid0);
  bpm->NewPage(pid1);
  bpm->NewPage(pid2);

  // 尝试 FetchPage 一个不存在的页 —— 缓冲池满 + 无 Victim 候选
  page_id_t fake_page = 99999;
  auto *result = bpm->FetchPage(fake_page);
  ASSERT_EQ(nullptr, result);  // 必须返回空

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, UnpinInvalidPageIdReturnsFalse) {
  const std::string db_name = "bpm_student_unpin_invalid.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  // Unpin 一个从未存在过的页号
  ASSERT_FALSE(bpm->UnpinPage(42, false));

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, UnpinWhenPinCountIsZeroReturnsFalse) {
  const std::string db_name = "bpm_student_unpin_zero.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  page_id_t pid;
  bpm->NewPage(pid);
  // 第一次 Unpin：pin_count 从 1 到 0，成功
  ASSERT_TRUE(bpm->UnpinPage(pid, false));
  // 第二次 Unpin：pin_count 已经是 0，返回 false
  ASSERT_FALSE(bpm->UnpinPage(pid, false));

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, FlushNonExistentPageReturnsFalse) {
  const std::string db_name = "bpm_student_flush_invalid.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  ASSERT_FALSE(bpm->FlushPage(777));

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, DirtyPageWrittenOnEviction) {
  const std::string db_name = "bpm_student_dirty_evict.db";
  const size_t pool_size = 2;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  // 创建 page 0，写入标记，Unpin（标记脏）
  page_id_t pid0;
  auto *p0 = bpm->NewPage(pid0);
  ASSERT_EQ(0, pid0);
  const char marker = 0x7F;
  memset(p0->GetData(), marker, PAGE_SIZE);

  // 保存一份原始数据的副本——因为 p0 指向的 frame 后续可能被淘汰复用
  char saved_data[PAGE_SIZE];
  memcpy(saved_data, p0->GetData(), PAGE_SIZE);

  ASSERT_TRUE(bpm->UnpinPage(pid0, true));   // is_dirty = true

  // 创建 page 1，也 Unpin
  page_id_t pid1;
  bpm->NewPage(pid1);
  ASSERT_TRUE(bpm->UnpinPage(pid1, false));

  // 现在缓冲池满（2 槽位），两个都 pin=0
  // 创建 page 2 —— 会淘汰 Victim（最久的是 pid0，脏页）
  page_id_t pid2;
  auto *p2 = bpm->NewPage(pid2);
  ASSERT_NE(nullptr, p2);

  // 此时 pid0 已被淘汰（脏页应已落盘），重新 Fetch 验证数据
  auto *fetched = bpm->FetchPage(pid0);
  ASSERT_NE(nullptr, fetched);
  // 用之前保存的副本做比较，而非 p0（p0 指向的 frame 可能已被复用）
  ASSERT_EQ(0, std::memcmp(fetched->GetData(), saved_data, PAGE_SIZE));
  ASSERT_EQ(marker, fetched->GetData()[0]);
  ASSERT_EQ(marker, fetched->GetData()[PAGE_SIZE / 2]);
  ASSERT_EQ(marker, fetched->GetData()[PAGE_SIZE - 1]);

  bpm->UnpinPage(pid0, false);

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, FlushPageClearsDirtyFlag) {
  const std::string db_name = "bpm_student_flush_dirty.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  page_id_t pid;
  auto *p = bpm->NewPage(pid);
  const char data = 0x3C;
  memset(p->GetData(), data, PAGE_SIZE);
  ASSERT_TRUE(bpm->UnpinPage(pid, true));  // 标记脏

  // Flush 后脏标记应清除
  ASSERT_TRUE(bpm->FlushPage(pid));

  // 重新 Fetch，若此前标记未清除，Victim 时会重复写盘——这里不验证重复写盘，
  // 只验证 FlushPage 本身不崩溃且数据正确
  auto *fetched = bpm->FetchPage(pid);
  ASSERT_NE(nullptr, fetched);
  ASSERT_EQ(data, fetched->GetData()[0]);

  bpm->UnpinPage(pid, false);

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}

TEST(BufferPoolManagerStudentTest, CheckAllUnpinnedCatchesPinnedPages) {
  const std::string db_name = "bpm_student_check_unpinned.db";
  const size_t pool_size = 3;
  remove(db_name.c_str());

  auto *dm = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(pool_size, dm);

  page_id_t pid;
  bpm->NewPage(pid);
  // 还没 Unpin，pin_count = 1，CheckAllUnpinned 应返回 false
  ASSERT_FALSE(bpm->CheckAllUnpinned());

  bpm->UnpinPage(pid, false);
  // pin_count = 0，应返回 true
  ASSERT_TRUE(bpm->CheckAllUnpinned());

  dm->Close();
  remove(db_name.c_str());
  delete bpm;
  delete dm;
}
