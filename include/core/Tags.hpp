#pragma once
#include <initializer_list>
#include <string>
#include <vector>
namespace foil {
// A short list of words many games share (their languages, their genres),
// kept once for the whole catalog: each game holds a pointer to it. A list
// of its own per game was a third of the catalog's memory.
class Tags {
public:
  using List = std::vector<std::string>;
  Tags();
  Tags(const List &words);
  Tags(std::initializer_list<std::string> words) : Tags(List(words)) {}
  Tags &operator=(std::initializer_list<std::string> words) { return *this = Tags(words); }
  List::const_iterator begin() const { return list_->begin(); }
  List::const_iterator end() const { return list_->end(); }
  size_t size() const { return list_->size(); }
  bool empty() const { return list_->empty(); }
  const std::string &operator[](size_t i) const { return (*list_)[i]; }
  const std::string &front() const { return list_->front(); }
  void clear() { *this = Tags(); }
  bool operator==(const Tags &other) const { return list_ == other.list_; }

private:
  const List *list_;
};
// A short text many games repeat (their kind, region, publisher, price),
// kept once like Tags: a pointer instead of a string of its own.
class Word {
public:
  Word();
  explicit Word(const std::string &text);
  explicit Word(const char *text) : Word(std::string(text)) {}
  Word &operator=(const std::string &text) { return *this = Word(text); }
  Word &operator=(const char *text) { return *this = Word(text); }
  operator const std::string &() const { return *text_; }
  const std::string &str() const { return *text_; }
  const char *c_str() const { return text_->c_str(); }
  bool empty() const { return text_->empty(); }
  size_t size() const { return text_->size(); }
  void clear() { *this = Word(); }
  friend bool operator==(const Word &a, const Word &b) { return a.text_ == b.text_; }
  friend bool operator!=(const Word &a, const Word &b) { return a.text_ != b.text_; }
  friend bool operator==(const Word &a, const std::string &b) { return *a.text_ == b; }
  friend bool operator!=(const Word &a, const std::string &b) { return *a.text_ != b; }
  friend bool operator==(const std::string &a, const Word &b) { return a == *b.text_; }
  friend bool operator!=(const std::string &a, const Word &b) { return a != *b.text_; }
  friend bool operator==(const Word &a, const char *b) { return *a.text_ == b; }
  friend bool operator!=(const Word &a, const char *b) { return *a.text_ != b; }
  friend std::string operator+(const std::string &a, const Word &b) { return a + *b.text_; }
  friend std::string operator+(const Word &a, const std::string &b) { return *a.text_ + b; }
  friend std::string operator+(const char *a, const Word &b) { return a + *b.text_; }
  friend std::string operator+(const Word &a, const char *b) { return *a.text_ + b; }

private:
  const std::string *text_;
};
} // namespace foil
