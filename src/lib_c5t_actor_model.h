#pragma once

// NOTE(dkorolev): This code is super ugly, but that's what we have today.

#if 0
MULTIPLE TYPES PER TOPIC!

BETTER SYNTAX!

TODO NEED THE REGISTRY OF TYPES PER TOPIC!
TODO TESTS:
- create topic
  - dispatch events
  - nothing gest there
  - add one subscriber
  - dispatch
  - something gets there!
  - stop the subscriber scope
  - dispatch
  - does not get there!

- send to multiple
- unsubscribe some of them
-

- semantics: emit to  multiple destinations

- add some DEBUG WAIT FOR ALL QUEUES TO COMPLETE!
- add DISPATCHBATCOMPLETE!
- add DISPATCHTERMINATING!
#endif

#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>

#include "lib_c5t_lifetime_manager.h"

enum class TopicID : uint64_t {};
class TopicIDGenerator final {
 private:
  std::atomic_uint64_t next_topic_id = 0ull;

 public:
  TopicID GetNextUniqueTopicID() { return static_cast<TopicID>(next_topic_id++); }
};

struct ConstructTopicScope final {};

// When `<T>` is important to pass along, `TopicKey<T>` is easy to use instead of `TopicID`.
// It's always sufficient to use `TopicID`, but then the templated type `<T>` must be provided explicitly.
template <class T>
class TopicKey final {
 private:
  TopicID id_;
  TopicKey() = delete;

 public:
  TopicKey(ConstructTopicScope) : id_(current::Singleton<TopicIDGenerator>().GetNextUniqueTopicID()) {}
  TopicID GetTopicID() const { return id_; }
  operator TopicID() const { return GetTopicID(); }
};

template <class T>
TopicKey<T> Topic(std::string name = "") {
  // TODO(dkorolev): Tons of things incl. registry, counters, telemetry, etc.
  static_cast<void>(name);
  return TopicKey<T>((ConstructTopicScope()));
}

enum class EventsSubscriberID : uint64_t {};

class SubscribersCleanupLogic {
 public:
  virtual ~SubscribersCleanupLogic() = default;
  virtual void CleanupSubscriberByID(EventsSubscriberID) = 0;
};

class TopicsSubcribersAllTypesSingleton final : public SubscribersCleanupLogic {
 private:
  std::atomic_uint64_t ids_used_;

  std::mutex mutex_;
  std::unordered_map<std::type_index, SubscribersCleanupLogic*> cleanups_per_type_;
  std::unordered_map<EventsSubscriberID, std::unordered_set<std::type_index>> types_per_ids_;

 public:
  TopicsSubcribersAllTypesSingleton() : ids_used_(0ull) {}

  EventsSubscriberID AllocateNextID() { return static_cast<EventsSubscriberID>(++ids_used_); }

  template <typename T>
  void RegisterTypeForSubscriber(EventsSubscriberID sid, SubscribersCleanupLogic& respective_singleton_instance) {
    std::lock_guard lock(mutex_);
    auto t = std::type_index(typeid(T));
    auto& p = cleanups_per_type_[t];
    if (!p) {
      p = &respective_singleton_instance;
    }
    types_per_ids_[sid].insert(t);
  }

  void CleanupSubscriberByID(EventsSubscriberID sid) override {
    std::lock_guard lock(mutex_);
    for (auto& e : types_per_ids_[sid]) {
      cleanups_per_type_[e]->CleanupSubscriberByID(sid);
    }
  }
};

template <typename T>
class TopicsSubcribersPerTypeSingleton final : public SubscribersCleanupLogic {
 private:
  std::atomic_uint64_t ids_used_;
  std::mutex mutex_;

  std::unordered_map<EventsSubscriberID, std::unordered_set<TopicID>> s_;
  std::unordered_map<TopicID, std::unordered_map<EventsSubscriberID, std::function<void(std::shared_ptr<T>)>>> m_;

 public:
  TopicsSubcribersPerTypeSingleton() : ids_used_(0ull) {}

  EventsSubscriberID AllocateNextID() { return static_cast<EventsSubscriberID>(++ids_used_); }

  void AddLink(EventsSubscriberID sid, TopicID tid, std::function<void(std::shared_ptr<T>)> f) {
    std::lock_guard lock(mutex_);
    s_[sid].insert(tid);
    m_[tid][sid] = std::move(f);
  }

  void CleanupSubscriberByID(EventsSubscriberID sid) override {
    std::lock_guard lock(mutex_);
    for (TopicID tid : s_[sid]) {
      m_[tid].erase(sid);
    }
    s_.erase(sid);
  }

  void PublishEvent(TopicID tid, std::shared_ptr<T> event) {
    std::lock_guard lock(mutex_);
    for (auto const& e : m_[tid]) {
      // NOTE(dkorolev): This `.second` should just quickly add a `shared_ptr` to the queue.
      // TODO(dkorolev): Maybe make it more explicit from the code, since a lambda is ambiguous.
      e.second(event);
    }
  }
};

struct ConstructTopicsSubscriberScope final {};
struct ConstructTopicsSubscriberScopeImpl final {};

class ActorSubscriberScopeImpl {
 public:
  virtual ~ActorSubscriberScopeImpl() = default;
};

template <class C>
class ActorSubscriberScopeFor;

template <class C>
class ActorSubscriberScopeForImpl final : public ActorSubscriberScopeImpl {
 private:
  friend class ActorSubscriberScopeFor<C>;
  EventsSubscriberID const unique_id;
  struct Queue final {
    bool done = false;
    std::vector<std::function<void()>> fifo;
  };
  current::WaitableAtomic<Queue> wa;
  std::unique_ptr<C> worker;
  std::thread thread;

  ActorSubscriberScopeForImpl() = delete;
  ActorSubscriberScopeForImpl(ActorSubscriberScopeForImpl const&) = delete;
  ActorSubscriberScopeForImpl& operator=(ActorSubscriberScopeForImpl const&) = delete;
  ActorSubscriberScopeForImpl(ActorSubscriberScopeForImpl&&) = delete;
  ActorSubscriberScopeForImpl& operator=(ActorSubscriberScopeForImpl&&) = delete;

  void Thread() {
    auto const scope_term = LIFETIME_NOTIFY_OF_SHUTDOWN([this]() { wa.MutableUse([](Queue& q) { q.done = true; }); });
    while (true) {
      using r_t = std::pair<std::vector<std::function<void()>>, bool>;
      r_t const w = wa.Wait([](Queue const& q) { return q.done || !q.fifo.empty(); },
                            [](Queue& q) -> r_t {
                              if (q.done) {
                                return {{}, true};
                              } else {
                                std::vector<std::function<void()>> foo;
                                std::swap(q.fifo, foo);
                                return {foo, false};
                              }
                            });
      if (w.second) {
        worker->OnShutdown();
        break;
      } else {
        for (auto& f : w.first) {
          try {
            f();
          } catch (current::Exception const&) {
            // TODO
          } catch (std::exception const&) {
            // TODO
          }
        }
        worker->OnBatchDone();
      }
    }
  }

  template <typename E>
  void EnqueueEvent(std::shared_ptr<E> e) {
    wa.MutableUse([this, &e](Queue& q) { q.fifo.push_back([this, e2 = std::move(e)]() { worker->OnEvent(*e2); }); });
  }

 public:
  ActorSubscriberScopeForImpl(ConstructTopicsSubscriberScopeImpl, EventsSubscriberID id, std::unique_ptr<C> worker)
      : unique_id(id), worker(std::move(worker)), thread([this]() { Thread(); }) {}

  ~ActorSubscriberScopeForImpl() override {
    current::Singleton<TopicsSubcribersAllTypesSingleton>().CleanupSubscriberByID(unique_id);
    wa.MutableUse([](Queue& q) { q.done = true; });
    thread.join();
  }
};

template <class C>
class ActorSubscriberScopeFor final {
 private:
  friend class ActorSubscriberScope;
  friend class NullableActorSubscriberScope;

  ActorSubscriberScopeFor() = delete;
  ActorSubscriberScopeFor(ActorSubscriberScopeFor const&) = delete;
  ActorSubscriberScopeFor& operator=(ActorSubscriberScopeFor const&) = delete;

  std::unique_ptr<ActorSubscriberScopeForImpl<C>> impl_;

 public:
  ActorSubscriberScopeFor(ConstructTopicsSubscriberScope, std::unique_ptr<C> worker)
      : impl_(std::make_unique<ActorSubscriberScopeForImpl<C>>(
            ConstructTopicsSubscriberScopeImpl(),
            current::Singleton<TopicsSubcribersAllTypesSingleton>().AllocateNextID(),
            std::move(worker))) {}

  ActorSubscriberScopeFor(ActorSubscriberScopeFor&& rhs) = default;

  // TODO: This syntax is ugly, but it makes it easier to not bother with templates just yet.
  template <typename T>
  [[nodiscard]] ActorSubscriberScopeFor SubscribeToType(TopicID tid) {
    auto& s = current::Singleton<TopicsSubcribersPerTypeSingleton<T>>();
    ActorSubscriberScopeForImpl<C>& impl = *impl_;
    current::Singleton<TopicsSubcribersAllTypesSingleton>().RegisterTypeForSubscriber<T>(impl.unique_id, s);
    s.AddLink(impl.unique_id, tid, [&impl](std::shared_ptr<T> e) { impl.template EnqueueEvent<T>(std::move(e)); });
    return std::move(*this);
  }

  // TODO: This syntax is ugly, but it makes it easier to not bother with templates just yet.
  template <typename T>
  [[nodiscard]] ActorSubscriberScopeFor operator()(TopicKey<T> tkey) {
    return SubscribeToType<T>(tkey);
  }
};

// A type-erased `ActorSubscriberScopeFor<T>`.
class ActorSubscriberScope final {
 private:
  ActorSubscriberScope() = delete;
  ActorSubscriberScope(ActorSubscriberScope const&) = delete;
  ActorSubscriberScope& operator=(ActorSubscriberScope const&) = delete;

  std::unique_ptr<ActorSubscriberScopeImpl> type_erased_impl_;

 public:
  ActorSubscriberScope(ActorSubscriberScope&&) = default;
  ActorSubscriberScope& operator=(ActorSubscriberScope&&) = default;

  template <class T>
  ActorSubscriberScope(ActorSubscriberScopeFor<T>&& rhs) : type_erased_impl_(std::move(rhs.impl_)) {}

  template <class T>
  ActorSubscriberScope& operator=(ActorSubscriberScopeFor<T>&& rhs) {
    type_erased_impl_ = std::move(rhs.impl);
    return *this;
  }
};

class NullableActorSubscriberScope final {
 private:
  NullableActorSubscriberScope(NullableActorSubscriberScope const&) = delete;
  NullableActorSubscriberScope& operator=(NullableActorSubscriberScope const&) = delete;

  std::unique_ptr<ActorSubscriberScopeImpl> type_erased_impl_;

 public:
  NullableActorSubscriberScope() = default;
  NullableActorSubscriberScope(NullableActorSubscriberScope&&) = default;
  NullableActorSubscriberScope& operator=(NullableActorSubscriberScope&&) = default;

  template <class T>
  NullableActorSubscriberScope(ActorSubscriberScopeFor<T>&& rhs) : type_erased_impl_(std::move(rhs.impl_)) {}

  template <class T>
  NullableActorSubscriberScope& operator=(ActorSubscriberScopeFor<T>&& rhs) {
    type_erased_impl_ = std::move(rhs.impl_);
    return *this;
  }

  NullableActorSubscriberScope& operator=(std::nullptr_t) {
    type_erased_impl_ = nullptr;
    return *this;
  }
};

template <class C>
[[nodiscard]] ActorSubscriberScopeFor<C> SubscribeWorkerTo(std::unique_ptr<C> worker) {
  return ActorSubscriberScopeFor<C>(ConstructTopicsSubscriberScope(), std::move(worker));
}

template <class C, typename... ARGS>
[[nodiscard]] ActorSubscriberScopeFor<C> SubscribeTo(ARGS&&... args) {
  return SubscribeWorkerTo<C>(std::make_unique<C>(std::forward<ARGS>(args)...));
}

template <class T, class... ARGS>
void EmitEventTo(TopicID tid, std::shared_ptr<T> event) {
  current::Singleton<TopicsSubcribersPerTypeSingleton<T>>().PublishEvent(tid, std::move(event));
}

template <class T, class... ARGS>
void EmitTo(TopicID tid, ARGS&&... args) {
  EmitEventTo(tid, std::make_shared<T>(std::forward<ARGS>(args)...));
}
