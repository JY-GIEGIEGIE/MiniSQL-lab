#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages){}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  if (lru_list_.empty()) { // if the list is empty, return false
    return false;
  }
  *frame_id = lru_list_.front(); // get the front element of the list
  lru_list_.pop_front(); // remove the front element from the list
  lru_set_.erase(*frame_id); // remove the front element from the set
  return true;
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  if (lru_set_.find(frame_id) != lru_set_.end()) { // if frame_id is in the set, remove it from the list and set
    lru_list_.remove(frame_id);
    lru_set_.erase(frame_id);
  }
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  if (lru_set_.find(frame_id) == lru_set_.end()) { // if frame_id is not in the set, add it to the list and set
    lru_list_.push_back(frame_id);
    lru_set_.insert(frame_id);
  }
}

/**
 * TODO: Student Implement
 */
size_t LRUReplacer::Size() {
  return lru_list_.size();
}