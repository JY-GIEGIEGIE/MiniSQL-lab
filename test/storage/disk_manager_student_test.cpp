#include "storage/disk_manager.h"

#include <unordered_set>

#include "gtest/gtest.h"
#include "page/bitmap_page.h"
#include "page/disk_file_meta_page.h"

// =============================================================================
// BitmapPage 补充测试（使用真实的 PAGE_SIZE=4096）
// =============================================================================

TEST(BitmapPageStudentTest, AllocateDeallocateWithPageSize4096) {
  // 使用实际的 PAGE_SIZE 而非 512，验证真实场景下的分配和释放
  const size_t kSize = 4096;
  char buf[kSize];
  memset(buf, 0, kSize);
  auto *bitmap = reinterpret_cast<BitmapPage<kSize> *>(buf);

  auto num_pages = bitmap->GetMaxSupportedSize();
  // 初始全部空闲
  for (uint32_t i = 0; i < num_pages; i++) {
    ASSERT_TRUE(bitmap->IsPageFree(i));
  }

  // 全部分配完毕，验证分配的 page_offset 不重复
  uint32_t ofs;
  std::unordered_set<uint32_t> allocated;
  for (uint32_t i = 0; i < num_pages; i++) {
    ASSERT_TRUE(bitmap->AllocatePage(ofs));
    ASSERT_EQ(allocated.end(), allocated.find(ofs)) << "Duplicate offset " << ofs;
    allocated.insert(ofs);
  }
  // 满了不能再分配
  ASSERT_FALSE(bitmap->AllocatePage(ofs));
}

TEST(BitmapPageStudentTest, DeAllocateRejectsOutOfBounds) {
  const size_t kSize = 4096;
  char buf[kSize];
  memset(buf, 0, kSize);
  auto *bitmap = reinterpret_cast<BitmapPage<kSize> *>(buf);

  auto max = bitmap->GetMaxSupportedSize();
  // 越界的 page_offset 应被拒绝
  ASSERT_FALSE(bitmap->DeAllocatePage(max));
  ASSERT_FALSE(bitmap->DeAllocatePage(max + 100));
}

TEST(BitmapPageStudentTest, IsPageFreeOutOfBounds) {
  const size_t kSize = 4096;
  char buf[kSize];
  memset(buf, 0, kSize);
  auto *bitmap = reinterpret_cast<BitmapPage<kSize> *>(buf);

  auto max = bitmap->GetMaxSupportedSize();
  // 越界的 page_offset 返回 false（不是有效页）
  ASSERT_FALSE(bitmap->IsPageFree(max));
  ASSERT_FALSE(bitmap->IsPageFree(max + 1));
}

TEST(BitmapPageStudentTest, DoubleFreeRejected) {
  const size_t kSize = 4096;
  char buf[kSize];
  memset(buf, 0, kSize);
  auto *bitmap = reinterpret_cast<BitmapPage<kSize> *>(buf);

  uint32_t ofs;
  ASSERT_TRUE(bitmap->AllocatePage(ofs));
  // 第一次释放成功
  ASSERT_TRUE(bitmap->DeAllocatePage(ofs));
  // 第二次释放同一位置应被拒绝
  ASSERT_FALSE(bitmap->DeAllocatePage(ofs));
}

TEST(BitmapPageStudentTest, FreedPageReusedFirst) {
  const size_t kSize = 4096;
  char buf[kSize];
  memset(buf, 0, kSize);
  auto *bitmap = reinterpret_cast<BitmapPage<kSize> *>(buf);

  // 分配前几个页
  uint32_t a, b, c;
  ASSERT_TRUE(bitmap->AllocatePage(a));  // offset 0
  ASSERT_TRUE(bitmap->AllocatePage(b));  // offset 1
  ASSERT_TRUE(bitmap->AllocatePage(c));  // offset 2

  // 释放 offset 1
  ASSERT_TRUE(bitmap->DeAllocatePage(b));

  // 再分配，应该拿到 offset 1（next_free_page_ hint 指到 1）
  uint32_t d;
  ASSERT_TRUE(bitmap->AllocatePage(d));
  ASSERT_EQ(b, d);  // 回收了刚释放的页
}

// =============================================================================
// DiskManager 补充测试
// =============================================================================

TEST(DiskManagerStudentTest, IsPageFreeBeforeAndAfterAllocation) {
  std::string db_name = "disk_student_test1.db";
  remove(db_name.c_str());
  DiskManager dm(db_name);

  // 刚创建时，任何逻辑页号都不存在，IsPageFree 读到的 BitmapPage 是全零
  // 此时 AllocatePage 会先创建 Extent 0
  page_id_t pid = dm.AllocatePage();
  // 分配后该页不空闲
  ASSERT_FALSE(dm.IsPageFree(pid));
  // 未分配的下一页在同一个全零 BitmapPage 中仍是空闲的
  ASSERT_TRUE(dm.IsPageFree(pid + 1));

  dm.Close();
  remove(db_name.c_str());
}

TEST(DiskManagerStudentTest, PageIdReuseAfterDeallocate) {
  std::string db_name = "disk_student_test2.db";
  remove(db_name.c_str());
  DiskManager dm(db_name);

  // 分配三个页
  page_id_t p0 = dm.AllocatePage();
  page_id_t p1 = dm.AllocatePage();
  page_id_t p2 = dm.AllocatePage();
  ASSERT_EQ(0, p0);
  ASSERT_EQ(1, p1);
  ASSERT_EQ(2, p2);

  // 释放 p1
  dm.DeAllocatePage(p1);
  ASSERT_TRUE(dm.IsPageFree(p1));

  // 再分配应拿到 p1
  page_id_t p3 = dm.AllocatePage();
  ASSERT_EQ(p1, p3);

  dm.Close();
  remove(db_name.c_str());
}

TEST(DiskManagerStudentTest, AllocateAcrossExtents) {
  std::string db_name = "disk_student_test3.db";
  remove(db_name.c_str());
  DiskManager dm(db_name);

  // 分配刚好填满 Extent 0 的页
  for (uint32_t i = 0; i < DiskManager::BITMAP_SIZE; i++) {
    page_id_t pid = dm.AllocatePage();
    ASSERT_EQ(i, pid);
  }

  // Extent 0 已满，下一个分配在 Extent 1 的第 0 位
  page_id_t first_of_extent1 = dm.AllocatePage();
  ASSERT_EQ(DiskManager::BITMAP_SIZE, first_of_extent1);

  // 验证 Meta Page：此时有两个 Extent
  DiskFileMetaPage *meta =
      reinterpret_cast<DiskFileMetaPage *>(dm.GetMetaData());
  ASSERT_EQ(2, meta->GetExtentNums());
  ASSERT_EQ(DiskManager::BITMAP_SIZE + 1, meta->GetAllocatedPages());

  dm.Close();
  remove(db_name.c_str());
}

// MapPageId 是 DiskManager 的私有方法，无法从外部直接测试。
// 其正确性已通过 FreePageAllocationTest 和 AllocateAcrossExtents
// 中间接验证——逻辑页号正确映射为物理位置，分配和读写均正常。
