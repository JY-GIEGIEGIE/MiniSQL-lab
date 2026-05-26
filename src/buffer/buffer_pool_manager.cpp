#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

frame_id_t BufferPoolManager::TryToFindFreePage() {
  // find a free frame_id from free list or replacer, if both are empty, return INVALID_PAGE_ID
  frame_id_t frame_id;
  if (!free_list_.empty()) { // if there are free frames in free list, return the first one
    frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  } else if (replacer_->Victim(&frame_id)) { // free list is empty, try to find a victim page from replacer
    Page &page = pages_[frame_id];
    if (page.IsDirty()) { // if the victim page is dirty, flush it to disk
      FlushPage(page.GetPageId());
    }
    page_table_.erase(page.GetPageId()); // remove the victim page from page table
    return frame_id;
  }
  return INVALID_FRAME_ID;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  // 1.     Search the page table for the requested page (P).
  if (page_table_.find(page_id) != page_table_.end()) { // 1.1    If P exists, pin it and return it immediately.
    frame_id_t frame_id = page_table_[page_id];
    Page &page = pages_[frame_id];
    page.pin_count_++;
    replacer_->Pin(frame_id);
    return &page;
  }

  // 2. Page not in memory — find a free frame
  frame_id_t fm_id = TryToFindFreePage();
  if (fm_id == INVALID_FRAME_ID) {
    return nullptr;
  }
  // 3. Read the page from disk and set up the frame
  Page &page = pages_[fm_id];
  page.ResetMemory();
  disk_manager_->ReadPage(page_id, page.GetData());
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;
  page_table_[page_id] = fm_id;
  replacer_->Pin(fm_id);
  return &page;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  // 1. Find a free frame first — don't allocate from disk yet.
  frame_id_t fm_id = TryToFindFreePage();
  if (fm_id == INVALID_FRAME_ID) {
    return nullptr;
  }

  // 2. Now allocate a new page id from disk.
  page_id = AllocatePage();
  if (page_id == INVALID_PAGE_ID) {
    return nullptr;
  }

  // 3. Update P's metadata, zero out memory and add P to the page table.
  Page &page = pages_[fm_id];
  page.ResetMemory();
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;
  page_table_[page_id] = fm_id;
  replacer_->Pin(fm_id);
  // 4. Set the page ID output parameter. Return a pointer to P.
  return &page;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  // 1.   Search the page table for the requested page (P).
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    DeallocatePage(page_id);
    return true;
  }

  // 2.   If P exists, but has a non-zero pin-count, return false.
  frame_id_t frame_id = it->second;
  Page &page = pages_[frame_id];
  if (page.pin_count_ > 0) {
    return false;
  }

  // 3.   Remove from page table and replacer, return frame to free list.
  page_table_.erase(page_id);
  replacer_->Pin(frame_id);

  page.ResetMemory();
  page.page_id_ = INVALID_PAGE_ID;
  page.pin_count_ = 0;
  page.is_dirty_ = false;
  free_list_.push_back(frame_id);

  DeallocatePage(page_id);
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  if (page_table_.find(page_id) == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = page_table_[page_id];
  Page &page = pages_[frame_id];

  if (page.pin_count_ <= 0) {
    return false;
  }

  if (is_dirty) {
    page.is_dirty_ = true;
  }

  page.pin_count_--;
  if (page.pin_count_ == 0) {
    replacer_->Unpin(frame_id);
  }
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  if (page_table_.find(page_id) == page_table_.end()) { //
    return false;
  }
  frame_id_t frame_id = page_table_[page_id];
  Page &page = pages_[frame_id];
  disk_manager_->WritePage(page_id, page.GetData());
  page.is_dirty_ = false;
  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}