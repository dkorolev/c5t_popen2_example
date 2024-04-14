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
  struct TrackedInstances final {
    // For printing / reporting.
    // TODO(dkorolev): Unify and merge with `EnsureExactlyOnce`!
    uint64_t next_id_desc = 0u;
    std::map<uint64_t, std::string> still_alive;
  };

  struct EnsureExactlyOnce final {
    // To notify of termination exactly once.
    // TODO(dkorolev): Unify and merge with `TrackedInstances`.
    uint64_t next_id_desc = 0u;
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
  template <typename... ARGS>
  void EmplaceThread(ARGS&&... args) {
    AbortIfNotInitialized();
    if (!termination_initiated_atomic_) {
      // It's OK to just not start the thread if already in the "terminating" mode.
      std::lock_guard lock(threads_to_join_mutex_);
      threads_to_join_.emplace_back(std::forward<ARGS>(args)...);
    }
  }

  [[nodiscard]] current::WaitableAtomicSubscriberScope SubscribeToTerminationEvent(std::function<void()> f) {
    AbortIfNotInitialized();
    // Ensures that `f()` will only be called once, possibly right from the very call to `SubscribeToTerminationEvent()`
    // .
    size_t const unique_id = exactly_once_subscribers_.MutableUse([](EnsureExactlyOnce& safety) {
      uint64_t const id = safety.next_id_desc;
      safety.still_active.insert(id);
      --safety.next_id_desc;
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
    bool const previous_value = termination_initiated_.MutableUse([](std::atomic_bool& b) {
      bool const retval = b.load();
      b = true;
      return retval;
    });

    if (previous_value) {
      Log("Ignoring a consecutive call to `ExitForReal()`.");
    } else {
      Log("`ExitForReal()` called for the first time, initating termination sequence.");
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
        Log("`ExitForReal()` termination sequence successful, joining the threads.");
        std::vector<std::thread> threads_to_join = [this]() {
          std::lock_guard lock(threads_to_join_mutex_);
          return std::move(threads_to_join_);
        }();
        for (auto& t : threads_to_join) {
          t.join();
        }
        Log("`ExitForReal()` termination sequence successful, all done.");
        ::exit(exit_code);
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

#define LIFETIME_MANAGER_SINGLETON() current::Singleton<LifetimeManagerSingleton>()
#define LIFETIME_MANAGER_ACTIVATE(logger) LIFETIME_MANAGER_SINGLETON().LIFETIME_MANAGER_ACTIVATE_IMPL(logger)

// O(1), just `.load()`-s the atomic.
#define LIFETIME_SHUTTING_DOWN LIFETIME_MANAGER_SINGLETON().termination_initiated_atomic_

// Returns the `[[nodiscard]]`-ed scope for the lifetime of the passed-in lambda being registered.
#define LIFETIME_NOTIFY_OF_SHUTDOWN(f) LIFETIME_MANAGER_SINGLETON().SubscribeToTerminationEvent(f)

// TODO(dkorolev): Refactor this.
#define LIFETIME_TRACKED_DEBUG_DUMP() LIFETIME_MANAGER_SINGLETON().DumpActive()

inline void LIFETIME_MANAGER_EXIT(int exit_code = 0,
                                  std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
  LIFETIME_MANAGER_SINGLETON().ExitForReal(exit_code, graceful_delay);
}

// This is a bit of a "singleton instance" creator.
// Not recommended to use overall, as it would create one thread per instance,
// as opposed to "a single thread to own them all". But okay for the test and for quick experiments.
// TODO(dkorolev): The same semantics could be used to leverage that "single thread to own them all".
template <class T, class... ARGS>
T& CreateLifetimeTrackedInstance(char const* file, int line, std::string const& text, ARGS&&... args) {
  current::WaitableAtomic<T*> result(nullptr);
  // Construct in a dedicated thread, so that when it's time to destruct the destructors do not block one another!
  auto& s = LIFETIME_MANAGER_SINGLETON();
  s.EmplaceThread([&]() {
    size_t const id = [&]() {
      T instance(std::forward<ARGS>(args)...);
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

// TODO(dkorolev): May make this into a variadic macro to allow passing arguments to this thread in a C+-native way.
#define LIFETIME_TRACKED_THREAD(desc, body)                    \
  LIFETIME_MANAGER_SINGLETON().EmplaceThread([&]() {           \
    auto& s = LIFETIME_MANAGER_SINGLETON();                    \
    size_t const id = s.TrackingAdd(desc, __FILE__, __LINE__); \
    body();                                                    \
    s.TrackingRemove(id);                                      \
  })

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
        auto const scope = LIFETIME_MANAGER_SINGLETON().SubscribeToTerminationEvent([&ctx]() { ctx.Kill(); });
        moved_cb_code(ctx);
      },
      env);
}

#define LIFETIME_TRACKED_POPEN2(txt, ...) LIFETIME_TRACKED_POPEN2_IMPL(txt, __FILE__, __LINE__, __VA_ARGS__)

#endif  // C5T_POPEN2_H_INCLUDED
