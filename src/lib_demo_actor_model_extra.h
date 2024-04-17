#pragma once

#include "lib_c5t_actor_model.h"

struct TimerEvent final {
  uint32_t i;

  TimerEvent() = delete;
  TimerEvent(TimerEvent const&) = delete;
  TimerEvent& operator=(TimerEvent const&) = delete;

  TimerEvent(uint32_t i) : i(i) {}
};

struct InputEvent final {
  std::string s;

  InputEvent() = delete;
  InputEvent(InputEvent const&) = delete;
  InputEvent& operator=(InputEvent const&) = delete;

  InputEvent(std::string s) : s(std::move(s)) {}
};

void StartTimerThread(TopicKey<TimerEvent> topic_timer);
