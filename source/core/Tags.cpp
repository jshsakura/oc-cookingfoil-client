#include "core/Tags.hpp"
#include <mutex>
#include <set>
namespace foil {
namespace {
// Never emptied: a catalog has a few hundred different lists at most.
std::mutex &poolLock() {
  static std::mutex lock;
  return lock;
}
const Tags::List &intern(const Tags::List &words) {
  static std::set<Tags::List> pool;
  std::lock_guard<std::mutex> held(poolLock());
  return *pool.insert(words).first;
}
const std::string &internWord(const std::string &text) {
  static std::set<std::string> pool;
  std::lock_guard<std::mutex> held(poolLock());
  return *pool.insert(text).first;
}
} // namespace
Word::Word() : text_(&internWord({})) {}
Word::Word(const std::string &text) : text_(&internWord(text)) {}
Tags::Tags() : list_(&intern({})) {}
Tags::Tags(const List &words) : list_(&intern(words)) {}
} // namespace foil
