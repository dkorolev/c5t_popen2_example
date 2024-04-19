#pragma once

#include <chrono>
#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <set>

#include "bricks/util/singleton.h"
#include "bricks/strings/printf.h"
#include "bricks/strings/util.h"
#include "bricks/sync/waitable_atomic.h"
#include "bricks/file/file.h"
#include "bricks/time/chrono.h"

struct LifetimeTrackedInstance final {
  std::string description;
  std::string file_fullname;
  std::string file_basename;
  uint32_t line_as_number;
  std::string line_as_string;
  std::chrono::microseconds t_added;

  static std::string BaseName(std::string const& s) {
    char const* r = s.c_str();
    for (char const* p = r; p[0] && p[1]; ++p) {
      if (*p == current::FileSystem::GetPathSeparator()) {
        r = p + 1;
      }
    }
    return r;
  }

  LifetimeTrackedInstance() = default;
  LifetimeTrackedInstance(std::string desc,
                          std::string file,
                          uint32_t line,
                          std::chrono::microseconds t = current::time::Now())
      : description(std::move(desc)),
        file_fullname(std::move(file)),
        file_basename(BaseName(file_fullname)),
        line_as_number(line),
        line_as_string(current::ToString(line_as_number)),
        t_added(t) {}

  std::string ToShortString() const { return description + " @ " + file_basename + ':' + line_as_string; }
};

struct LifetimeManagerSingleton final {
  // The `TrackedInstances` waitable atomic keeps track of everything that needs to be terminated before `::exit()`.
  // If at least one tracked instance remains unfinished within the grace period, `::abort()` is performed instead.
  // As a nice benefit, for tracked instances it is also journaled when and from what FILE:LINE did they start.
  struct TrackedInstances final {
    uint64_t next_id_desc = 0u;  // Descending so that in the naturally sorted order the more recent items come first.
    std::map<uint64_t, LifetimeTrackedInstance> still_alive;
  };

  std::atomic_bool initialized_;

  mutable std::mutex logger_mutex_;
  std::function<void(std::string const&)> logger_ = nullptr;

  current::WaitableAtomic<std::atomic_bool> termination_initiated_;
  std::atomic_bool& termination_initiated_atomic_;

  current::WaitableAtomic<TrackedInstances> tracking_;

  std::vector<std::thread> threads_to_join_;
  std::mutex threads_to_join_mutex_;

  void Log(std::string const& s) const {
    std::lock_guard lock(logger_mutex_);
    if (logger_) {
      logger_(s);
    } else {
      std::cerr << "LIFETIME_MANAGER_LOG: " << s << std::endl;
    }
  }

  LifetimeManagerSingleton()
      : initialized_(false),
        termination_initiated_(false),
        termination_initiated_atomic_(*termination_initiated_.MutableScopedAccessor()) {}

  void LIFETIME_MANAGER_ACTIVATE_IMPL(std::function<void(std::string const&)> logger) {
    bool const was_initialized = initialized_;
    initialized_ = true;
    {
      std::lock_guard lock(logger_mutex_);
      logger_ = logger;
    }
    if (was_initialized) {
      Log("Called `LIFETIME_MANAGER_ACTIVATE()` twice, aborting.");
      ::abort();
    }
  }

  void AbortIfNotInitialized() const {
    if (!initialized_) {
      Log("Was not `LIFETIME_MANAGER_ACTIVATE()`, aborting.");
      ::abort();
    }
  }

  size_t TrackingAdd(std::string const& description, char const* file, size_t line) {
    AbortIfNotInitialized();
    return tracking_.MutableUse([=](TrackedInstances& trk) {
      uint64_t const id = trk.next_id_desc;
      --trk.next_id_desc;
      trk.still_alive[id] = LifetimeTrackedInstance(description, file, line);
      return id;
    });
  }

  void TrackingRemove(size_t id) {
    tracking_.MutableUse([=](TrackedInstances& trk) { trk.still_alive.erase(id); });
  }

  // To run "global" threads instead of `.detach()`-ing them: these threads will be `.join()`-ed upon termination.
  // This function is internal, and it assumes that the provided thread itself respects the termination signal.
  // (There is a mechanism to guard against this too, with the second possible `::abort()` clause, but still.)
  template <typename... ARGS>
  void EmplaceThreadImpl(ARGS&&... args) {
    AbortIfNotInitialized();
    termination_initiated_.ImmutableUse([&](bool already_terminating) {
      // It's OK to just not start the thread if already in the "terminating" mode.
      if (!already_terminating) {
        std::lock_guard lock(threads_to_join_mutex_);
        threads_to_join_.emplace_back(std::forward<ARGS>(args)...);
      }
    });
  }

  [[nodiscard]] current::WaitableAtomicSubscriberScope SubscribeToTerminationEvent(std::function<void()> f0) {
    AbortIfNotInitialized();
    // Ensures that `f0()` will only be called once, possibly from the very call to `SubscribeToTerminationEvent()`.
    auto const f = [this, called = std::make_shared<current::WaitableAtomic<bool>>(false), f1 = std::move(f0)]() {
      // Guard against spurious wakeups.
      if (termination_initiated_atomic_ ||
          termination_initiated_.ImmutableUse([](std::atomic_bool const& b) { return b.load(); })) {
        // Guard against calling the user-provided `f0()` more than once.
        if (called->MutableUse([](bool& called_flag) {
              if (called_flag) {
                return false;
              } else {
                called_flag = true;
                return true;
              }
            })) {
          f1();
        }
      }
    };
    auto result = termination_initiated_.Subscribe(f);
    // Here it is safe to use `termination_initiated_atomic_`, since the guarantee provided is "at least once",
    // and, coupled with the `still_active` guard, it becomes "exactly once" for `f` to be called.
    if (termination_initiated_atomic_) {
      f();
    }
    return result;
  }

  void DumpActive(std::function<void(LifetimeTrackedInstance const&)> f0 = nullptr) const {
    AbortIfNotInitialized();
    std::function<void(LifetimeTrackedInstance const&)> f =
        f0 != nullptr ? f0 : [this](LifetimeTrackedInstance const& s) { Log(s.ToShortString()); };
    tracking_.ImmutableUse([&f](TrackedInstances const& trk) {
      for (auto const& [_, s] : trk.still_alive) {
        f(s);
      }
    });
  }

  void WaitUntilTimeToDie() const {
    // This function is only useful when called from a thread in the scope of important data was created.
    // Generally, this is the way to create lifetime-manager-friendly singleton instances:
    // 1) Spawn a thread.
    // 2) Create everything in it, preferably as `Owned<WaitableAtomic<...>>`.
    // 3) At the end of this thread wait until it is time to die.
    // 4) Once it is time to die, everything this thread has created will be destroyed, gracefully or forcefully.
    AbortIfNotInitialized();
    termination_initiated_.Wait([](bool die) { return die; });
  }

  void DoExitForReal(int exit_code = 0, std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
    auto const t0 = current::time::Now();
    std::map<uint64_t, LifetimeTrackedInstance> original_still_alive = tracking_.ImmutableScopedAccessor()->still_alive;
    std::vector<uint64_t> still_alive_ids;
    for (auto const& e : original_still_alive) {
      still_alive_ids.push_back(e.first);
    }
    bool ok = false;
    tracking_.WaitFor(
        [this, &ok, &original_still_alive, &still_alive_ids, t0](TrackedInstances const& trk) {
          std::vector<uint64_t> next_still_alive_ids;
          auto const t1 = current::time::Now();
          for (uint64_t id : still_alive_ids) {
            auto const cit = trk.still_alive.find(id);
            if (cit == std::end(trk.still_alive)) {
              auto const& e = original_still_alive[id];
              // NOTE(dkorolev): The order of `Gone after`-s may not be exactly the order of stuff terminating.
              // TODO(dkorolev): May well tweak this one day.
              Log(current::strings::Printf("Gone after %.3lfs: %s @ %s:%d",
                                           1e-6 * (t1 - t0).count(),
                                           e.description.c_str(),
                                           e.file_basename.c_str(),
                                           e.line_as_number));
            } else {
              next_still_alive_ids.push_back(id);
            }
          }
          still_alive_ids = std::move(next_still_alive_ids);
          if (trk.still_alive.empty()) {
            ok = true;
            return true;
          } else {
            return false;
          }
        },
        graceful_delay);
    if (ok) {
      Log("`ExitForReal()` termination sequence successful, joining the presumably-done threads.");
      std::vector<std::thread> threads_to_join = [this]() {
        std::lock_guard lock(threads_to_join_mutex_);
        return std::move(threads_to_join_);
      }();
      current::WaitableAtomic<bool> threads_joined_successfully(false);
      std::thread threads_joiner([&threads_to_join, &threads_joined_successfully]() {
        for (auto& t : threads_to_join) {
          t.join();
        }
        threads_joined_successfully.SetValue(true);
      });
      bool need_to_abort_because_threads_are_not_all_joined = true;
      threads_joined_successfully.WaitFor(
          [&need_to_abort_because_threads_are_not_all_joined](bool b) {
            if (b) {
              need_to_abort_because_threads_are_not_all_joined = false;
              return true;
            } else {
              return false;
            }
          },
          graceful_delay);
      if (!need_to_abort_because_threads_are_not_all_joined) {
        Log("`ExitForReal()` termination sequence successful, all threads joined.");
        threads_joiner.join();
        Log("`ExitForReal()` termination sequence successful, all done.");
        ::exit(exit_code);
      } else {
        Log("");
        Log("`ExitForReal()` uncooperative threads remain, time to `abort()`.");
        ::abort();
      }
    } else {
      Log("");
      Log("`ExitForReal()` termination sequence unsuccessful, still has offenders.");
      tracking_.ImmutableUse([this](TrackedInstances const& trk) {
        for (auto const& [_, s] : trk.still_alive) {
          Log("Offender: " + s.ToShortString());
        }
      });
      Log("");
      Log("`ExitForReal()` time to `abort()`.");
      ::abort();
    }
  }

  void ExitForReal(int exit_code = 0, std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
    bool const previous_value = termination_initiated_.MutableUse([](std::atomic_bool& already_terminating) {
      bool const retval = already_terminating.load();
      already_terminating = true;
      return retval;
    });

    if (previous_value) {
      Log("Ignoring a consecutive call to `ExitForReal()`.");
    } else {
      Log("`ExitForReal()` called, initating termination sequence.");
      DoExitForReal(exit_code, graceful_delay);
    }
  }

  ~LifetimeManagerSingleton() {
    // Should die organically!
    bool const previous_value = termination_initiated_.MutableUse([](std::atomic_bool& already_terminating) {
      bool const retval = already_terminating.load();
      already_terminating = true;
      return retval;
    });

    if (!previous_value) {
      Log("");
      Log("The program is terminating organically.");
      DoExitForReal();
    }
  }
};

#define LIFETIME_MANAGER_SINGLETON_IMPL() current::Singleton<LifetimeManagerSingleton>()
#define LIFETIME_MANAGER_ACTIVATE(logger) LIFETIME_MANAGER_SINGLETON_IMPL().LIFETIME_MANAGER_ACTIVATE_IMPL(logger)

// O(1), just `.load()`-s the atomic.
#define LIFETIME_SHUTTING_DOWN LIFETIME_MANAGER_SINGLETON_IMPL().termination_initiated_atomic_

// Returns the `[[nodiscard]]`-ed scope for the lifetime of the passed-in lambda being registered.
template <class F>
[[nodiscard]] inline current::WaitableAtomicSubscriberScope LIFETIME_NOTIFY_OF_SHUTDOWN(F&& f) {
  return LIFETIME_MANAGER_SINGLETON_IMPL().SubscribeToTerminationEvent(std::forward<F>(f));
}

// Waits forever. Useful for "singleton" threads and in `popen2()` runners for what should run forever.
inline void LIFETIME_SLEEP_UNTIL_SHUTDOWN() { LIFETIME_MANAGER_SINGLETON_IMPL().WaitUntilTimeToDie(); }

// Use in place of `std::this_thread::sleep_for(...)`. Also returns `false` if it's time to die.
template <class DT>
inline bool LIFETIME_SLEEP_FOR(DT&& dt) {
  LIFETIME_MANAGER_SINGLETON_IMPL().termination_initiated_.WaitFor([](std::atomic_bool const& b) { return b.load(); },
                                                                   std::forward<DT>(dt));
  return !LIFETIME_SHUTTING_DOWN;
}

#define LIFETIME_TRACKED_DEBUG_DUMP(...) LIFETIME_MANAGER_SINGLETON_IMPL().DumpActive(__VA_ARGS__)

inline void LIFETIME_MANAGER_EXIT(int code = 0, std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
  LIFETIME_MANAGER_SINGLETON_IMPL().ExitForReal(code, graceful_delay);
}

// This is a bit of a "singleton instance" creator.
// Not recommended to use overall, as it would create one thread per instance,
// as opposed to "a single thread to own them all". But okay for the test and for quick experiments.
// TODO(dkorolev): One day the same semantics could be used to leverage that "single thread to own them all".
template <class T, class... ARGS>
T& CreateLifetimeTrackedInstance(char const* file, int line, std::string const& text, ARGS&&... args) {
  current::WaitableAtomic<T*> result(nullptr);
  // Construct in a dedicated thread, so that when it's time to destruct the destructors do not block one another!
  auto& mgr = LIFETIME_MANAGER_SINGLETON_IMPL();
  mgr.EmplaceThreadImpl([&]() {
    size_t const id = [&]() {
      T instance(std::forward<ARGS>(args)...);
      // Must ensure the thread registers its lifetime and respects the termination signal.
      size_t const id = mgr.TrackingAdd(text, file, line);
      result.SetValue(&instance);
      mgr.WaitUntilTimeToDie();
      return id;
    }();
    mgr.TrackingRemove(id);
  });
  result.Wait([](T const* ptr) { return ptr != nullptr; });
  return *result.GetValue();
}

#define LIFETIME_TRACKED_INSTANCE(type, ...) CreateLifetimeTrackedInstance<type>(__FILE__, __LINE__, __VA_ARGS__)

// TODO(dkorolev): Maybe make this into a variadic macro to allow passing arguments to this thread in a C+-native way.
// NOTE(dkorolev): Ensure that the thread body registers its lifetime to the singleton manager,
//                 to eliminate the risk of this thread being `.join()`-ed before it is fully done.
// NOTE(dkorolev): The `ready_to_go` part is essential because otherwise the lambda capture list may not intiailize yet!
// TODO(dkorolev): Why and how so though? I better investigate this deeper before using `std::move`-d lambda captures!
#define LIFETIME_TRACKED_THREAD(desc0, body)                                                   \
  do {                                                                                         \
    current::WaitableAtomic<bool> ready_to_go(false);                                          \
    std::string desc(desc0);                                                                   \
    LIFETIME_MANAGER_SINGLETON_IMPL().EmplaceThreadImpl(                                       \
        [moved_desc = std::move(desc), moved_body = std::move(body), &ready_to_go]() mutable { \
          auto& mgr = LIFETIME_MANAGER_SINGLETON_IMPL();                                       \
          size_t const id = mgr.TrackingAdd(moved_desc, __FILE__, __LINE__);                   \
          ready_to_go.SetValue(true);                                                          \
          moved_body();                                                                        \
          mgr.TrackingRemove(id);                                                              \
        });                                                                                    \
    ready_to_go.Wait([](bool b) { return b; });                                                \
  } while (false)

// TODO(dkorolev): This `#ifdef` is ugly, and it will get fixed once we standardize our `cmake`-based builds.
// NOTE(dkorolev): `LIFETIME_TRACKED_POPEN2` extrends the "vanilla" `popen2()` in two ways.
//
// 1) The user provides the "display name" for the inner graceful "task manager" to report what is running, and
// 2) The lifetime managers takes the liberty to send SIGTERM to the child process once termination is initated.
//
// It is still up to the user to exit from the callback function that can `write()` into the child process.
// The user can, of course, send SIGTERM to the child process via the "native" `popen2()`-provided means.
// It is guaranteed that SIGTERM will only be sent to the child process once.

// NOTE(dkorolev): This `T_POPEN2_RUNTIME` is not a useful template type per se, it is only here to ensure
//                 that the function is not compiled until used. This way, if `C5T/popen` is neither included
//                 nor used, there are no build warnings/errors whatsoever.
template <class T_POPEN2_RUNTIME>
inline int LIFETIME_TRACKED_POPEN2_IMPL(
    std::string const& text,
    char const* file,
    size_t line,
    std::vector<std::string> const& cmdline,
    std::function<void(const std::string&)> cb_line,
    std::function<void(T_POPEN2_RUNTIME&)> cb_code = [](T_POPEN2_RUNTIME&) {},
    std::vector<std::string> const& env = {}) {
  auto& mgr = LIFETIME_MANAGER_SINGLETON_IMPL();
  size_t const id = mgr.TrackingAdd(text, file, line);
  std::shared_ptr<std::atomic_bool> popen2_done = std::make_shared<std::atomic_bool>(false);
  int const retval = popen2(
      cmdline,
      cb_line,
      [copy_popen_done = popen2_done, &mgr, moved_cb_code = std::move(cb_code)](T_POPEN2_RUNTIME& ctx) {
        // NOTE(dkorolev): On `popen2()` level it's OK to call `.Kill()` multiple times, only one will go through.
        auto const scope =
            mgr.SubscribeToTerminationEvent([&ctx, &mgr, captured_popen_done = std::move(copy_popen_done)]() {
              if (!captured_popen_done->load()) {
                ctx.Kill();
              }
            });
        moved_cb_code(ctx);
      },
      env);
  popen2_done->store(true);
  mgr.TrackingRemove(id);
  return retval;
}

#define LIFETIME_TRACKED_POPEN2(text, ...) LIFETIME_TRACKED_POPEN2_IMPL<Popen2Runtime>(text, __FILE__, __LINE__, __VA_ARGS__)
