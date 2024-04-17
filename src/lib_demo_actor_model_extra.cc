#include "lib_demo_actor_model_extra.h"

void StartTimerThread(TopicKey<TimerEvent> topic_timer) {
  LIFETIME_TRACKED_THREAD("timer", [topic_timer]() mutable {
    int i = 0;
    while (!LIFETIME_SHUTTING_DOWN) {
      ++i;
      EmitTo<TimerEvent>(topic_timer, i);
      LIFETIME_SLEEP_FOR(std::chrono::milliseconds(1000));
    }
  });
}
