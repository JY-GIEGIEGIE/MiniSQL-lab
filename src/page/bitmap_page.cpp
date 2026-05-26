#include "page/bitmap_page.h"

#include "glog/logging.h"

template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  if (page_allocated_ >= GetMaxSupportedSize()) {
    return false;
  }
  for (size_t i = 0; i < MAX_CHARS; i++) {
    if (bytes[i] != 0xFF) {  // If not all bits are 1, there is a free page.
      for (size_t j = 0; j < 8; j++) {
        if ((bytes[i] & (1 << j)) == 0) {  // If the jth bit is 0, it is a free page.
          bytes[i] |= (1 << j);              // Set the bit to 1 to mark it as allocated.
          page_offset = i * 8 + j;           // Calculate the page offset.
          page_allocated_++;                  // Increment the count of allocated pages.
          next_free_page_ = page_offset + 1;  // Update hint for next allocation.
          return true;
        }
      }
    }
  }
  return false;  // No free page found, should not reach here if page_allocated_ is accurate.
}

template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  if (page_offset >= GetMaxSupportedSize()) { // Check if the page_offset is valid
    return false;
  }
  size_t byte_index = page_offset / 8;
  size_t bit_index = page_offset % 8;
  if (IsPageFreeLow(byte_index, bit_index)) {
    return false;
  }
  uint8_t mask = 1 << bit_index;        // only the object bit is 1, others are 0
  uint8_t inverted_mask = ~mask;        // only the object bit is 0, others are 1
  bytes[byte_index] = bytes[byte_index] & inverted_mask; // Clear the bit to mark the page as free
  page_allocated_--;
  if (page_offset < next_free_page_) {  // Point hint back to this newly freed page.
    next_free_page_ = page_offset;
  }
  return true;
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  if (page_offset >= GetMaxSupportedSize()) {
    return false;
  }
  return IsPageFreeLow(page_offset / 8, page_offset % 8);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  return (bytes[byte_index] & (1 << bit_index)) == 0;
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;
