#pragma once

#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <set>

#include "bricks/util/singleton.h"
#include "bricks/strings/util.h"
#include "bricks/sync/waitable_atomic.h"

// TODO(dkorolev): Combine `TrackedInstances` with what needs to be notified of termination.
struct LifetimeManagerSingleton final {
  // The `TrackedInstances` waitable atomic keeps track of everything that needs to be terminated before `::exit()`.
  // If at least one tracked instance remains unfinished within the grace period, `::abort()` is performed instead.
  // As a nice benefit, for tracked instances it is also journaled when and from what FILE:LINE did they start.
  struct TrackedInstances final {
    uint64_t next_id_desc = 0u;  // Descending so that in the naturally sorted order the more recent items come first.
    std::map<uint64_t, std::string> still_alive;
  };

  // The `EnsureExactlyOnce` waitable atomic is only used by `SubscribeToTerminationEvent` scopes.
  // It can not be merged with `TrackedInstances`, since `TrackedInstances` MUST be waited at termination,
  // while `SubscribeToTerminationEvent` subscribers SHOULD "just" be notified not more than once each.
  struct EnsureExactlyOnce final {
    uint64_t next_exactly_once_id = 0u;
    std::set<uint64_t> still_active;
  };

  std::atomic_bool initialized_;

  mutable std::mutex logger_mutex_;
  std::function<void(std::string const&)> logger_ = nullptr;

  current::WaitableAtomic<std::atomic_bool> termination_initiated_;
  std::atomic_bool& termination_initiated_atomic_;

  current::WaitableAtomic<TrackedInstances> tracking_;
  current::WaitableAtomic<EnsureExactlyOnce> exactly_once_subscribers_;

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
        termination_initiated_atomic_(*termination_initiated_.MutableScopedAccessor()) {
  }

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

  size_t TrackingAdd(std::string const& text, char const* file, size_t line) {
    AbortIfNotInitialized();
    return tracking_.MutableUse([=](TrackedInstances& trk) {
      uint64_t const id = trk.next_id_desc;
      --trk.next_id_desc;
      trk.still_alive[id] = text + " @ " + file + ':' + current::ToString(line);
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
    termination_initiated_.MutableUse([&](bool already_terminating) {
      // It's OK to just not start the thread if already in the "terminating" mode.
      if (!already_terminating) {
        std::lock_guard lock(threads_to_join_mutex_);
        threads_to_join_.emplace_back(std::forward<ARGS>(args)...);
      }
    });
  }

  [[nodiscard]] current::WaitableAtomicSubscriberScope SubscribeToTerminationEvent(std::function<void()> f) {
    AbortIfNotInitialized();
    // Ensures that `f()` will only be called once, possibly from the very call to `SubscribeToTerminationEvent()`.
    size_t const unique_id = exactly_once_subscribers_.MutableUse([](EnsureExactlyOnce& safety) {
      uint64_t const id = safety.next_exactly_once_id;
      safety.still_active.insert(id);
      ++safety.next_exactly_once_id;
      return id;
    });
    auto const call_f_exactly_once = [this, unique_id, f]() {
      if (exactly_once_subscribers_.MutableUse([unique_id](EnsureExactlyOnce& safety) {
            auto it = safety.still_active.find(unique_id);
            if (it == std::end(safety.still_active)) {
              return false;
            } else {
              safety.still_active.erase(it);
              return true;
            }
          })) {
        f();
      }
    };
    auto result = termination_initiated_.Subscribe(call_f_exactly_once);
    // Here it is safe to use `termination_initiated_atomic_`, since the guarantee provided is "at least once",
    // and, coupled with the `still_active` guard, it becomes "exactly once" for `f` to be called.
    if (termination_initiated_atomic_) {
      call_f_exactly_once();
    }
    return result;
  }

  void DumpActive(std::function<void(std::string const&)> f0 = nullptr) const {
    AbortIfNotInitialized();
    std::function<void(std::string const&)> f = f0 != nullptr ? f0 : [this](std::string const& s) { Log(s); };
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
      bool ok = false;
      tracking_.WaitFor(
          [&ok](TrackedInstances const& trk) {
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
            Log("Offender: " + s);
          }
        });
        Log("");
        Log("`ExitForReal()` time to `abort()`.");
        ::abort();
      }
    }
  }
};

#define LIFETIME_MANAGER_SINGLETON_IMPL() current::Singleton<LifetimeManagerSingleton>()
#define LIFETIME_MANAGER_ACTIVATE(logger) LIFETIME_MANAGER_SINGLETON_IMPL().LIFETIME_MANAGER_ACTIVATE_IMPL(logger)

// O(1), just `.load()`-s the atomic.
#define LIFETIME_SHUTTING_DOWN LIFETIME_MANAGER_SINGLETON_IMPL().termination_initiated_atomic_

// Returns the `[[nodiscard]]`-ed scope for the lifetime of the passed-in lambda being registered.
#define LIFETIME_NOTIFY_OF_SHUTDOWN(f) LIFETIME_MANAGER_SINGLETON_IMPL().SubscribeToTerminationEvent(f)

// TODO(dkorolev): Refactor this.
#define LIFETIME_TRACKED_DEBUG_DUMP(...) LIFETIME_MANAGER_SINGLETON_IMPL().DumpActive(__VA_ARGS__)

inline void LIFETIME_MANAGER_EXIT(int code = 0, std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
  LIFETIME_MANAGER_SINGLETON_IMPL().ExitForReal(code, graceful_delay);
}

// This is a bit of a "singleton instance" creator.
// Not recommended to use overall, as it would create one thread per instance,
// as opposed to "a single thread to own them all". But okay for the test and for quick experiments.
// TODO(dkorolev): The same semantics could be used to leverage that "single thread to own them all".
template <class T, class... ARGS>
T& CreateLifetimeTrackedInstance(char const* file, int line, std::string const& text, ARGS&&... args) {
  current::WaitableAtomic<T*> result(nullptr);
  // Construct in a dedicated thread, so that when it's time to destruct the destructors do not block one another!
  auto& s = LIFETIME_MANAGER_SINGLETON_IMPL();
  s.EmplaceThreadImpl([&]() {
    size_t const id = [&]() {
      T instance(std::forward<ARGS>(args)...);
      // Must ensure the thread registers its lifetime and respects the termination signal.
      size_t const id = s.TrackingAdd(text, file, line);
      result.SetValue(&instance);
      s.WaitUntilTimeToDie();
      return id;
    }();
    s.TrackingRemove(id);
  });
  result.Wait([](T const* ptr) { return ptr != nullptr; });
  return *result.GetValue();
}

#define LIFETIME_TRACKED_INSTANCE(type, ...) CreateLifetimeTrackedInstance<type>(__FILE__, __LINE__, __VA_ARGS__)

// TODO(dkorolev): Maybe make this into a variadic macro to allow passing arguments to this thread in a C+-native way.
// NOTE(dkorolev): Ensure that the thread body registers its lifetime to the singleton manager,
//                 to eliminate the risk of this thread being `.join()`-ed before it is fully done.
#define LIFETIME_TRACKED_THREAD(desc, body)                    \
  LIFETIME_MANAGER_SINGLETON_IMPL().EmplaceThreadImpl([&]() {  \
    auto& s = LIFETIME_MANAGER_SINGLETON_IMPL();               \
    size_t const id = s.TrackingAdd(desc, __FILE__, __LINE__); \
    body();                                                    \
    s.TrackingRemove(id);                                      \
  })

// TODO(dkorolev): This `#ifdef` is ugly, and it will get fixed once we standardize our `cmake`-based builds.
#ifdef C5T_POPEN2_H_INCLUDED

// NOTE(dkorolev): `LIFETIME_TRACKED_POPEN2` extrends the "vanilla" `popen2()` in two ways.
//
// 1) The user provides the "display name" for the inner graceful "task manager" to report what is running, and
// 2) The lifetime managers takes the liberty to send SIGTERM to the child process once termination is initated.
//
// It is still up to the user to exit from the callback function that can `write()` into the child process.
// The user can, of course, send SIGTERM to the child process via the "native" `popen2()`-provided means.
// It is guaranteed that SIGTERM will only be sent to the child process once.

inline int LIFETIME_TRACKED_POPEN2_IMPL(
    std::string const& txt,
    char const* file,
    size_t line,
    std::vector<std::string> const& cmdline,
    std::function<void(const std::string&)> cb_line,
    std::function<void(Popen2Runtime&)> cb_code = [](Popen2Runtime&) {},
    std::vector<std::string> const& env = {}) {
  return popen2(
      cmdline,
      cb_line,
      [moved_cb_code = std::move(cb_code)](Popen2Runtime& ctx) {
        // NOTE(dkorolev): On `popen2()` level it's OK to call `.Kill()` multiple times, only one will go through.
        auto const scope = LIFETIME_MANAGER_SINGLETON_IMPL().SubscribeToTerminationEvent([&ctx]() { ctx.Kill(); });
        moved_cb_code(ctx);
      },
      env);
}

#define LIFETIME_TRACKED_POPEN2(txt, ...) LIFETIME_TRACKED_POPEN2_IMPL(txt, __FILE__, __LINE__, __VA_ARGS__)

#endif  // C5T_POPEN2_H_INCLUDED
