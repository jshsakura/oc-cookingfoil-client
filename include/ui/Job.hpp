#pragma once
// One worker thread at a time: `run` does the work off the UI thread, and
// what the work returns runs on the UI thread at the next `poll`. A busy job
// refuses another until that poll; `what` names the one running.
#include "core/Log.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <utility>
namespace foil::ui {
class Job {
public:
  using Finish = std::function<void()>;
  Job() = default;
  Job(const Job &) = delete;
  Job &operator=(const Job &) = delete;
  ~Job() { join(); }
  bool busy() const { return thread_.joinable(); }
  bool is(const char *what) const { return busy() && what_ == what; }
  // False, and nothing runs, while another is still to be polled. `crashed`
  // runs on the UI thread in place of the finish when the work throws.
  bool run(const char *what, std::function<Finish()> work, Finish crashed) {
    if (busy()) return false;
    what_ = what;
    thread_ = std::thread([this, what, work = std::move(work), crashed = std::move(crashed)] {
      Finish finish;
      if (!guarded(what, [&] { finish = work(); })) finish = crashed;
      finish_ = std::move(finish);
      done_ = true;
    });
    return true;
  }
  void poll() {
    if (!done_.exchange(false)) return;
    thread_.join();
    what_.clear();
    if (finish_) std::exchange(finish_, nullptr)(); // may run the next one
  }
  // Waits for the work and drops what it returned (the app is closing).
  void join() {
    if (thread_.joinable()) thread_.join();
    finish_ = nullptr;
    done_ = false;
    what_.clear();
  }

private:
  std::thread thread_;
  std::atomic<bool> done_{false};
  Finish finish_;
  std::string what_;
};
} // namespace foil::ui
