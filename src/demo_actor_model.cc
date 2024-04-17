/*
TESTS:
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
*/

// NOTE(dkorolev): This code is super ugly, but that's what we have today.

#include <iostream>
#include <chrono>
#include <thread>

#include "bricks/exception.h"
#include "lib_c5t_lifetime_manager.h"
#include "lib_c5t_actor_model.h"

#include "lib_demo_actor_model_extra.h"

#include "bricks/dflags/dflags.h"
#include "blocks/http/api.h"

DEFINE_uint16(port, 5555, "");

int main(int argc, char** argv) {
  ParseDFlags(&argc, &argv);

  // The actor model requires the lifetime manager to be active.
  LIFETIME_MANAGER_ACTIVATE([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });

  auto const topic_timer = Topic<TimerEvent>("timer");
  auto const topic_input = Topic<InputEvent>("input");

  auto& http = HTTP(current::net::BarePort(FLAGS_port));

  current::WaitableAtomic<bool> time_to_stop_http_server_and_die(false);
  auto scope = http.Register("/kill", [&time_to_stop_http_server_and_die](Request r) {
    r("Gone.\n");
    // TODO(dkorolev): This is ugly, but since HTTP is not friendly with lifetime management (yet), it's also safe.
    time_to_stop_http_server_and_die.SetValue(true);
  });

  scope += http.Register("/status", [&](Request r) {
    std::ostringstream oss;
    LIFETIME_TRACKED_DEBUG_DUMP([&oss](LifetimeTrackedInstance const& t) {
      oss << current::strings::Printf("- %s @ %s:%d, up %.3lfs",
                                      t.description.c_str(),
                                      t.file_basename.c_str(),
                                      t.line_as_number,
                                      1e-6 * (current::time::Now() - t.t_added).count())
          << std::endl;
    });
    r(oss.str());
  });

  StartTimerThread(topic_timer);

  scope += http.Register("/", [&](Request r) {
    // TODO: TOPICS ARE NOT COPYABLE!
    LIFETIME_TRACKED_THREAD("chunked socket", ([topic_timer, topic_input, moved_r = std::move(r)]() mutable {
                              current::WaitableAtomic<bool> shutdown(false);
                              struct ChunksSender final {
                                current::WaitableAtomic<bool>& shutdown;
                                Request r;
                                decltype(std::declval<Request>().SendChunkedResponse()) rs;

                                ChunksSender(current::WaitableAtomic<bool>& shutdown, Request r0)
                                    : shutdown(shutdown), r(std::move(r0)), rs(r.SendChunkedResponse()) {
                                  Send("Yo\n");
                                }

                                void Send(std::string s) {
                                  try {
                                    rs(s);
                                  } catch (current::Exception const&) {
                                    shutdown.SetValue(true);
                                  }
                                }

                                void OnEvent(TimerEvent const& te) { Send(current::ToString(te.i) + '\n'); }
                                void OnEvent(InputEvent const& ie) { Send(ie.s + '\n'); }
                                void OnBatchDone() {}
                                void OnShutdown() {}
                              };
                              ActorSubscriberScope const s1 =
                                  SubscribeTo<ChunksSender>(shutdown, std::move(moved_r))(topic_timer)(topic_input);

                              auto const s2 = LIFETIME_NOTIFY_OF_SHUTDOWN([&]() { shutdown.SetValue(true); });
                              shutdown.Wait([](bool b) { return b; });
                            }));
  });

  LIFETIME_TRACKED_THREAD("stdin!", [&topic_input]() {
    // NOTE(dkorolev): This thread does not terminate by itself, and will be cause an `::abort()`.
    while (true) {
      std::cout << "Enter whatever: " << std::flush;
      std::string s;
      std::getline(std::cin, s);
      EmitTo<InputEvent>(topic_input, s);
      std::cout << "Line sent to all chunk HTTP listeners: " << s << std::endl;
    }
  });

  // NOTE(dkorolev): No `http.Join()`, since HTTP is not lifetime-management-friendly yet.
  time_to_stop_http_server_and_die.Wait([](bool b) { return b; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  LIFETIME_MANAGER_EXIT(0);
}
