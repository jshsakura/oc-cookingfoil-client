#pragma once
// Every thread the app starts runs on the drawing thread's core: libnx puts
// a new thread on the process's default core and leaves it there. A
// download that stays there shares one core with the screen, its TLS and
// its writes with every frame, however many connections it opens. These
// cores are the ones the install engine already uses for its own threads.
namespace foil::platform {
constexpr int WriterCore = 1;  // the card and the install: ncm writes, zstd, AES
constexpr int NetworkCore = 2; // curl and the TLS of every connection
// Moves the thread it is made on to `core` while it lives, when the process
// may use that core, and puts it back where it was after. Nothing off the
// console.
class OnCore {
public:
  explicit OnCore(int core);
  ~OnCore();
  OnCore(const OnCore &) = delete;
  OnCore &operator=(const OnCore &) = delete;

private:
  bool moved_ = false;
  int core_ = 0;
  unsigned long long mask_ = 0;
};
} // namespace foil::platform
