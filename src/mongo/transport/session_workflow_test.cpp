/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
// IWYU pragma: no_include "cxxabi.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session_workflow.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

const Status kClosedSessionError{ErrorCodes::SocketException, "Session is closed"};
const Status kNetworkError{ErrorCodes::HostUnreachable, "Someone is unreachable"};
const Status kShutdownError{ErrorCodes::ShutdownInProgress, "Something is shutting down"};
const Status kArbitraryError{ErrorCodes::InternalError, "Something happened"};

template <typename F>
struct FunctionTraits;
template <typename R, typename... A>
struct FunctionTraits<R(A...)> {
    using function_type = R(A...);
    using result_type = R;
    using tied_arguments_type = std::tuple<A&...>;
};

/** X-Macro defining the mocked event names and their function signatures */
#define EVENT_TABLE(X)                                                         \
    /* Session functions */                                                    \
    X(sessionWaitForData, Status())                                            \
    X(sessionSourceMessage, StatusWith<Message>())                             \
    X(sessionSinkMessage, Status(const Message&))                              \
    /* ServiceEntryPoint functions */                                          \
    X(sepHandleRequest, Future<DbResponse>(OperationContext*, const Message&)) \
    X(sepEndSession, void(const std::shared_ptr<Session>&))                    \
/**/

/**
 * Events generated by SessionWorkflow via virtual function calls to mock
 * objects. They are a means to observe and indirectly manipulate
 * SessionWorkflow's behavior to reproduce test scenarios.
 *
 * They are named for the mock object and function that emits them.
 */
#define X(e, sig) e,
enum class Event { EVENT_TABLE(X) };
#undef X

StringData toString(Event e) {
#define X(e, sig) #e ""_sd,
    return std::array{EVENT_TABLE(X)}[static_cast<size_t>(e)];
#undef X
}

std::ostream& operator<<(std::ostream& os, Event e) {
    return os << toString(e);
}

/**
 * Trait that maps the Event enum to a function type.
 * Captures and analyzes the function type of each mocked Event.
 * We'll use these to enable type erasure in the framework.
 */
template <Event>
struct EventTraits;
#define X(e, sig) \
    template <>   \
    struct EventTraits<Event::e> : FunctionTraits<sig> {};
EVENT_TABLE(X)
#undef X

template <Event e>
using EventSigT = typename EventTraits<e>::function_type;
template <Event e>
using EventTiedArgumentsT = typename EventTraits<e>::tied_arguments_type;
template <Event e>
using EventResultT = typename EventTraits<e>::result_type;

Message makeOpMsg() {
    static auto nextId = AtomicWord<int>{0};
    auto omb = OpMsgBuilder{};
    omb.setBody(BSONObjBuilder{}.append("id", nextId.fetchAndAdd(1)).obj());
    return omb.finish();
}

DbResponse makeResponse(Message m) {
    DbResponse response{};
    response.response = m;
    return response;
}

DbResponse setExhaust(DbResponse response) {
    response.shouldRunAgainForExhaust = true;
    return response;
}

Message setExhaustSupported(Message msg) {
    OpMsg::setFlag(&msg, OpMsg::kExhaustSupported);
    return msg;
}

Message setMoreToCome(Message msg) {
    OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
    return msg;
}

class MockExpectationSlot {
public:
    struct BasicExpectation {
        virtual ~BasicExpectation() = default;
        virtual Event event() const = 0;
    };

    template <Event e>
    struct Expectation : BasicExpectation {
        explicit Expectation(unique_function<EventSigT<e>> cb) : cb{std::move(cb)} {}
        Event event() const override {
            return e;
        }

        unique_function<EventSigT<e>> cb;
    };

    template <Event e>
    void push(unique_function<EventSigT<e>> cb) {
        stdx::lock_guard lk{_mutex};
        invariant(!_cb);
        _cb = std::make_unique<Expectation<e>>(std::move(cb));
        _cv.notify_one();
    }

    template <Event e>
    unique_function<EventSigT<e>> pop() {
        stdx::unique_lock lk{_mutex};
        _cv.wait(lk, [&] { return !!_cb; });
        auto h = std::exchange(_cb, {});
        invariant(h->event() == e, "Expecting {}, got {}"_format(h->event(), e));
        return std::move(static_cast<Expectation<e>&>(*h).cb);
    }

private:
    mutable Mutex _mutex;
    stdx::condition_variable _cv;
    std::unique_ptr<BasicExpectation> _cb;
};

/**
 * Fixture that mocks interactions with a `SessionWorkflow`.
 */
class SessionWorkflowTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto sc = getServiceContext();
        sc->setServiceEntryPoint(_makeServiceEntryPoint());
        sc->setSessionManager(_makeSessionManager(sc));
        initializeNewSession();
        invariant(sessionManager()->start());
        _threadPool->startup();
    }

    void tearDown() override {
        ScopeGuard guard = [&] {
            ServiceContextTest::tearDown();
        };
        // Normal shutdown is a noop outside of ASAN.
        invariant(sessionManager()->shutdownAndWait(Seconds{10}));
        _threadPool->shutdown();
        _threadPool->join();
    }

    void initializeNewSession() {
        _session = std::make_shared<CustomMockSession>(this);
    }

    /** Waits for the current Session and SessionWorkflow to end. */
    void joinSessions() {
        ASSERT(sessionManager()->waitForNoSessions(Seconds{1}));
    }

    /** Launches a SessionWorkflow for the current session. */
    void startSession() {
        LOGV2(6742613, "Starting session");
        sessionManager()->startSession(_session);
    }

    MockServiceEntryPoint* sep() {
        return checked_cast<MockServiceEntryPoint*>(getServiceContext()->getServiceEntryPoint());
    }

    MockSessionManager* sessionManager() {
        return checked_cast<MockSessionManager*>(getServiceContext()->getSessionManager());
    }

    /**
     * Installs an arbitrary one-shot mock handler callback for the next event.
     * The next incoming mock event will invoke this callback and destroy it.
     */
    template <Event e>
    void injectMockResponse(unique_function<EventSigT<e>> cb) {
        _expect.push<e>(std::move(cb));
    }

    /**
     * Wrapper around `injectMockResponse`. Installs a handler for the `expected`
     * mock event, that will return the specified `result`.
     * Returns a `Future` that is fulfilled when that mock event occurs.
     */
    template <Event e>
    Future<void> asyncExpect(EventResultT<e> r) {
        auto pf = std::make_shared<PromiseAndFuture<void>>();
        injectMockResponse<e>([r = std::move(r), pf](auto&&...) mutable {
            pf->promise.emplaceValue();
            return std::move(r);
        });
        return std::move(pf->future);
    }

    template <Event e>
    Future<void> asyncExpect() {
        auto pf = std::make_shared<PromiseAndFuture<void>>();
        injectMockResponse<e>(
            [pf](const EventTiedArgumentsT<e>& args) mutable { pf->promise.emplaceValue(); });
        return std::move(pf->future);
    }

    template <Event e>
    void expect(EventResultT<e> r) {
        asyncExpect<e>(std::move(r)).get();
    }

    template <Event e>
    void expect() {
        asyncExpect<e>().get();
    }

private:
    class CustomMockSession : public CallbackMockSession {
    public:
        explicit CustomMockSession(SessionWorkflowTest* fixture) {
            endCb = [this] {
                *_connected = false;
            };
            isConnectedCb = [this] {
                return *_connected;
            };
            waitForDataCb = [fixture] {
                return fixture->_onMockEvent<Event::sessionWaitForData>(std::tie());
            };
            sourceMessageCb = [fixture] {
                return fixture->_onMockEvent<Event::sessionSourceMessage>(std::tie());
            };
            sinkMessageCb = [fixture](const Message& m) {
                return fixture->_onMockEvent<Event::sessionSinkMessage>(std::tie(m));
            };
            // The async variants will just run the same callback on `_threadPool`.
            auto async = [fixture](auto cb) {
                return ExecutorFuture<void>(fixture->_threadPool).then(cb).unsafeToInlineFuture();
            };
            asyncWaitForDataCb = [=, cb = waitForDataCb] {
                return async([cb] { return cb(); });
            };
            asyncSourceMessageCb = [=, cb = sourceMessageCb](const BatonHandle&) {
                return async([cb] { return cb(); });
            };
            asyncSinkMessageCb = [=, cb = sinkMessageCb](Message m, const BatonHandle&) {
                return async([cb, m = std::move(m)]() mutable { return cb(std::move(m)); });
            };
        }

    private:
        synchronized_value<bool> _connected{true};  // Born in the connected state.
    };

    std::shared_ptr<ThreadPool> _makeThreadPool() {
        ThreadPool::Options options{};
        options.poolName = "SessionWorkflowTest";
        return std::make_shared<ThreadPool>(std::move(options));
    }

    std::unique_ptr<MockServiceEntryPoint> _makeServiceEntryPoint() {
        auto sep = std::make_unique<MockServiceEntryPoint>();
        sep->handleRequestCb = [this](OperationContext* opCtx, const Message& msg) {
            if (!gInitialUseDedicatedThread) {
                // Simulates an async command implemented under the borrowed
                // thread model. The returned future will be fulfilled while
                // holding the ClientStrand for opCtx.
                auto pf = PromiseAndFuture<DbResponse>();
                ExecutorFuture<void>(_threadPool)
                    .then([this,
                           strand = ClientStrand::get(opCtx->getClient()),
                           opCtx,
                           msg = std::move(msg),
                           p = std::move(pf.promise)]() mutable {
                        strand->run([&] {
                            p.setWith([&] {
                                return _onMockEvent<Event::sepHandleRequest>(std::tie(opCtx, msg));
                            });
                        });
                    })
                    .getAsync([](auto&&) {});
                return std::move(pf.future);
            }
            return _onMockEvent<Event::sepHandleRequest>(std::tie(opCtx, msg));
        };
        return sep;
    }

    std::unique_ptr<MockSessionManager> _makeSessionManager(ServiceContext* svcCtx) {
        auto manager = std::make_unique<MockSessionManager>(svcCtx);
        manager->onEndSessionCb = [this](const std::shared_ptr<Session>& session) {
            _onMockEvent<Event::sepEndSession>(std::tie(session));
        };
        return manager;
    }

    /**
     * Called by all mock functions to notify the main thread and get a value with which to respond.
     * The mock function call is identified by an `event`.  If there isn't already an expectation,
     * the mock object will wait for one to be injected via a call to `injectMockResponse`.
     */
    template <Event event>
    EventResultT<event> _onMockEvent(const EventTiedArgumentsT<event>& args) {
        LOGV2_DEBUG(6742616, 2, "Mock event arrived", "event"_attr = event);
        return std::apply(_expect.pop<event>(), args);
    }

    MockExpectationSlot _expect;
    std::shared_ptr<CustomMockSession> _session;
    std::shared_ptr<ThreadPool> _threadPool = _makeThreadPool();
};

TEST_F(SessionWorkflowTest, StartThenEndSession) {
    startSession();
    expect<Event::sessionSourceMessage>(kClosedSessionError);
    expect<Event::sepEndSession>();
    joinSessions();
}

TEST_F(SessionWorkflowTest, OneNormalCommand) {
    startSession();
    expect<Event::sessionSourceMessage>(makeOpMsg());
    expect<Event::sepHandleRequest>(makeResponse(makeOpMsg()));
    expect<Event::sessionSinkMessage>(Status::OK());
    expect<Event::sessionSourceMessage>(kClosedSessionError);
    expect<Event::sepEndSession>();
    joinSessions();
}

TEST_F(SessionWorkflowTest, OnClientDisconnectCalledOnCleanup) {
    int disconnects = 0;
    sessionManager()->derivedOnClientDisconnectCb = [&](Client*) {
        ++disconnects;
    };
    startSession();
    ASSERT_EQ(disconnects, 0);
    expect<Event::sessionSourceMessage>(kClosedSessionError);
    expect<Event::sepEndSession>();
    joinSessions();
    ASSERT_EQ(disconnects, 1);
}

/** Repro of one formerly troublesome scenario generated by the StepRunner test below. */
TEST_F(SessionWorkflowTest, MoreToComeDisconnectAtSource3) {
    startSession();
    // One more-to-come command, yields an empty response per wire protocol
    expect<Event::sessionSourceMessage>(setMoreToCome(makeOpMsg()));
    expect<Event::sepHandleRequest>(makeResponse({}));
    // Another message from session, this time a normal RPC.
    expect<Event::sessionSourceMessage>(makeOpMsg());
    expect<Event::sepHandleRequest>(makeResponse(makeOpMsg()));
    expect<Event::sessionSinkMessage>(Status::OK());
    // Client disconnects while we're waiting for their next command.
    expect<Event::sessionSourceMessage>(kShutdownError);
    expect<Event::sepEndSession>();
    joinSessions();
}

/**
 * Check the behavior of an interrupted "getMore" exhaust command.
 * SessionWorkflow looks specifically for the "getMore" command name to trigger
 * this cleanup.
 */
TEST_F(SessionWorkflowTest, CleanupFromGetMore) {
    initializeNewSession();
    startSession();

    auto makeGetMoreRequest = [](int64_t cursorId) {
        OpMsgBuilder omb;
        omb.setBody(BSONObjBuilder{}
                        .append("getMore", cursorId)
                        .append("collection", "testColl")
                        .append("$db", "testDb")
                        .obj());
        return setExhaustSupported(omb.finish());
    };

    auto makeGetMoreResponse = [] {
        DbResponse response;
        OpMsgBuilder omb;
        omb.setBody(BSONObjBuilder{}.append("id", int64_t{0}).obj());
        response.response = omb.finish();
        return response;
    };

    // Produce the condition of having an active `getMore` exhaust command.
    expect<Event::sessionSourceMessage>(makeGetMoreRequest(123));
    expect<Event::sepHandleRequest>(setExhaust(makeGetMoreResponse()));

    expect<Event::sessionSinkMessage>(Status::OK());

    // Test thread waits on this to ensure the callback is run by the ServiceEntryPoint (and
    // therefore popped) before another callback is pushed.
    auto pf = std::make_shared<PromiseAndFuture<void>>();

    // Simulate a client disconnect during handleRequest. The cleanup of
    // exhaust resources happens when the session disconnects. After the simulated
    // client disconnect, expect the SessionWorkflow to issue a fire-and-forget "killCursors".
    injectMockResponse<Event::sepHandleRequest>(
        [promise = std::move(pf->promise)](OperationContext* opCtx, const Message& msg) mutable {
            promise.emplaceValue();
            // Simulate the opCtx being marked as killed due to client disconnect.
            opCtx->markKilled(ErrorCodes::ClientDisconnect);
            return Status(ErrorCodes::ClientDisconnect,
                          "ClientDisconnect as part of testing session cleanup.");
        });
    pf->future.get();

    PromiseAndFuture<Message> killCursors;
    injectMockResponse<Event::sepHandleRequest>(
        [p = std::move(killCursors.promise)](OperationContext*, const Message& msg) mutable {
            p.emplaceValue(msg);
            return makeResponse({});
        });
    ASSERT_EQ(OpMsgRequest::parse(killCursors.future.get()).getCommandName(), "killCursors"_sd);

    // Because they're fire-and-forget commands, we will only observe `handleRequest`
    // calls to the SEP for the cleanup "killCursors", and the next thing to happen
    // will be the end of the session.
    expect<Event::sepEndSession>();
    joinSessions();
}

class StepRunnerSessionWorkflowTest : public SessionWorkflowTest {
public:
    /**
     * Concisely encode the ways this test might respond to mock events.
     * The OK Result contents depend on which Event it's responding to.
     */
#define ACTION_TABLE(X)                                                       \
    X(basic)      /* OK result for a basic (request and response) command. */ \
    X(exhaust)    /* OK result for a exhuast command. */                      \
    X(moreToCome) /* OK result for a fire-and-forget command. */              \
    /**/                                                                      \
    X(errTerminate)  /* External termination via the ServiceEntryPoint. */    \
    X(errDisconnect) /* Socket disconnection by peer. */                      \
    X(errNetwork)    /* Unspecified network failure (host unreachable). */    \
    X(errShutdown)   /* System shutdown. */                                   \
    X(errArbitrary)  /* An arbitrary miscellaneous error. */

#define X(e) e,
    enum class Action { ACTION_TABLE(X) };
#undef X

    friend StringData toString(Action action) {
#define X(a) #a ""_sd,
        return std::array{ACTION_TABLE(X)}[static_cast<size_t>(action)];
#undef X
    }

    /**
     * Given a list of steps, performs a series of tests exercising that list.
     *
     * The `run()` function performs a set of variations on the steps, failing
     * further and further along the way, with different errors tried at each
     * step.
     *
     * It first sets a baseline by running all the steps without injecting
     * failure. Then it checks each failure condition for each step in the
     * sequence. For example, if we have steps[NS] and failure conditions
     * fails[NF], it will run these pseudocode trials:
     *
     *   // First, no errors.
     *   { steps[0](OK); steps[1](OK); ... steps[NS-1](OK); }
     *
     *   // Inject each kind of failure at steps[0].
     *   { steps[0](fails[0]); }
     *   { steps[0](fails[1]); }
     *   ... and so on for fails[NF].
     *
     *   // Now let steps[0] succeed, but inject each kind of failure at steps[1].
     *   { steps[0](OK); steps[1](fails[0]); }
     *   { steps[0](OK); steps[1](fails[1]); }
     *   ... and so on for fails[NF].
     *
     *   // And so on the NS steps....
     */
    class RunAllErrorsAtAllSteps {
    public:
        /** The set of failures is hardcoded. */
        static constexpr std::array fails{Action::errTerminate,
                                          Action::errDisconnect,
                                          Action::errNetwork,
                                          Action::errShutdown,
                                          Action::errArbitrary};

        /** Encodes a response to `event` by taking `action`. */
        struct Step {
            Event event;
            Action action = Action::basic;
        };

        // The final step is assumed to have `errDisconnect` as an action,
        // yielding an implied `kEnd` step.
        RunAllErrorsAtAllSteps(SessionWorkflowTest* fixture, std::deque<Step> steps)
            : _fixture{fixture}, _steps{[&, at = steps.size() - 1] {
                  return _appendTermination(std::move(steps), at, Action::errDisconnect);
              }()} {}

        /**
         * Run all of the trials specified by the constructor.
         */
        void run() {
            const std::deque<Step> baseline(_steps.begin(), _steps.end());
            LOGV2(5014106, "Running one entirely clean run");
            _runSteps(baseline);
            // Incrementally push forward the step where we fail.
            for (size_t failAt = 0; failAt + 1 < baseline.size(); ++failAt) {
                LOGV2(6742614, "Injecting failures", "failAt"_attr = failAt);
                for (auto fail : fails)
                    _runSteps(_appendTermination(baseline, failAt, fail));
            }
        }

    private:
        /**
         * Returns a new steps sequence, formed by copying the specified `q`, and
         * modifying the copy to be terminated with a `fail` at the `failAt` index.
         */
        std::deque<Step> _appendTermination(std::deque<Step> q, size_t failAt, Action fail) const {
            LOGV2(6742617, "appendTermination", "fail"_attr = fail, "failAt"_attr = failAt);
            invariant(failAt < q.size());
            q.erase(q.begin() + failAt + 1, q.end());
            q.back().action = fail;
            q.push_back({Event::sepEndSession});
            return q;
        }

        template <typename T>
        void _dumpTransitions(const T& q) {
            BSONArrayBuilder bab;
            for (auto&& t : q) {
                BSONObjBuilder{bab.subobjStart()}
                    .append("event", toString(t.event))
                    .append("action", toString(t.action));
            }
            LOGV2(6742615, "Run transitions", "transitions"_attr = bab.arr());
        }

        template <Event e, std::enable_if_t<std::is_void_v<EventResultT<e>>, int> = 0>
        void _setExpectation() {
            _fixture->expect<e>();
        }

        // The scenario generator will try to inject an error status into
        // functions That don't report errors, so that injected Status must be ignored.
        template <Event e, std::enable_if_t<std::is_void_v<EventResultT<e>>, int> = 0>
        void _setExpectation(Status) {
            _fixture->expect<e>();
        }

        template <Event e, std::enable_if_t<!std::is_void_v<EventResultT<e>>, int> = 0>
        void _setExpectation(EventResultT<e> r) {
            _fixture->expect<e>(std::move(r));
        }

        template <Event event>
        void injectStep(const Action& action) {
            LOGV2_DEBUG(6872301, 3, "Inject step", "event"_attr = event, "action"_attr = action);
            switch (action) {
                case Action::errTerminate: {
                    // Has a side effect of simulating a ServiceEntryPoint shutdown
                    // before responding with a shutdown error.
                    auto pf = std::make_shared<PromiseAndFuture<void>>();
                    _fixture->injectMockResponse<event>([this, pf](auto&&...) {
                        _fixture->sessionManager()->endAllSessionsNoTagMask();
                        pf->promise.emplaceValue();
                        if constexpr (std::is_void_v<EventResultT<event>>) {
                            return;
                        } else {
                            return kShutdownError;
                        }
                    });
                    pf->future.get();
                } break;
                case Action::errDisconnect:
                    _setExpectation<event>(kClosedSessionError);
                    break;
                case Action::errNetwork:
                    _setExpectation<event>(kNetworkError);
                    break;
                case Action::errShutdown:
                    _setExpectation<event>(kShutdownError);
                    break;
                case Action::errArbitrary:
                    _setExpectation<event>(kArbitraryError);
                    break;
                case Action::basic:
                case Action::exhaust:
                case Action::moreToCome:
                    if constexpr (event == Event::sepEndSession) {
                        _setExpectation<event>();
                        return;
                    } else if constexpr (event == Event::sessionWaitForData ||
                                         event == Event::sessionSinkMessage) {
                        _setExpectation<event>(Status::OK());
                        return;
                    } else if constexpr (event == Event::sessionSourceMessage) {
                        Message m = makeOpMsg();
                        if (action == Action::exhaust)
                            m = setExhaustSupported(m);
                        _setExpectation<event>(StatusWith{std::move(m)});
                        return;
                    } else if constexpr (event == Event::sepHandleRequest) {
                        switch (action) {
                            case Action::basic:
                                _setExpectation<event>(StatusWith{makeResponse(makeOpMsg())});
                                return;
                            case Action::exhaust:
                                _setExpectation<event>(
                                    StatusWith{setExhaust(makeResponse(makeOpMsg()))});
                                return;
                            case Action::moreToCome:
                                _setExpectation<event>(StatusWith{DbResponse{}});
                                return;
                            default:
                                MONGO_UNREACHABLE;
                        }
                        break;
                    }
                    break;
            }
        }

        void injectStep(const Step& t) {
            // The event table is expanded to generate the cases of a switch,
            // effectively transforming the runtime value `t.event` into a
            // template parameter. The `sig` is unused in the expansion.
            switch (t.event) {
#define X(e, sig)                       \
    case Event::e:                      \
        injectStep<Event::e>(t.action); \
        break;
                EVENT_TABLE(X)
#undef X
            }
        }

        /** Start a new session, run the `steps` sequence, and join the session. */
        void _runSteps(std::deque<Step> q) {
            _dumpTransitions(q);
            _fixture->initializeNewSession();
            _fixture->startSession();
            for (; !q.empty(); q.pop_front())
                injectStep(q.front());
            _fixture->joinSessions();
        }

        SessionWorkflowTest* _fixture;
        std::deque<Step> _steps;
    };

    void runSteps(std::deque<RunAllErrorsAtAllSteps::Step> steps) {
        RunAllErrorsAtAllSteps{this, steps}.run();
    }

    std::deque<RunAllErrorsAtAllSteps::Step> defaultLoop() const {
        return {
            {Event::sessionSourceMessage},
            {Event::sepHandleRequest},
            {Event::sessionSinkMessage},
            {Event::sessionSourceMessage},
        };
    }

    std::deque<RunAllErrorsAtAllSteps::Step> exhaustLoop() const {
        return {
            {Event::sessionSourceMessage, Action::exhaust},
            {Event::sepHandleRequest, Action::exhaust},
            {Event::sessionSinkMessage},
            {Event::sepHandleRequest},
            {Event::sessionSinkMessage},
            {Event::sessionSourceMessage},
        };
    }

    std::deque<RunAllErrorsAtAllSteps::Step> moreToComeLoop() const {
        return {
            {Event::sessionSourceMessage, Action::moreToCome},
            {Event::sepHandleRequest, Action::moreToCome},
            {Event::sessionSourceMessage},
            {Event::sepHandleRequest},
            {Event::sessionSinkMessage},
            {Event::sessionSourceMessage},
        };
    }
};

class SessionWorkflowWithDedicatedThreadsTest : public StepRunnerSessionWorkflowTest {
    ScopedValueOverride<bool> _svo{gInitialUseDedicatedThread, true};
};

TEST_F(SessionWorkflowWithDedicatedThreadsTest, DefaultLoop) {
    runSteps(defaultLoop());
}

TEST_F(SessionWorkflowWithDedicatedThreadsTest, ExhaustLoop) {
    runSteps(exhaustLoop());
}

TEST_F(SessionWorkflowWithDedicatedThreadsTest, MoreToComeLoop) {
    runSteps(moreToComeLoop());
}

class SessionWorkflowWithBorrowedThreadsTest : public StepRunnerSessionWorkflowTest {
public:
    /**
     * Under the borrowed thread model, the steps are the same as for dedicated thread model,
     * except that Session sourceMessage events are preceded by Session waitForData events.
     */
    std::deque<RunAllErrorsAtAllSteps::Step> convertStepsToBorrowed(
        std::deque<RunAllErrorsAtAllSteps::Step> q) {
        for (auto iter = q.begin(); iter != q.end(); ++iter)
            if (iter->event == Event::sessionSourceMessage)
                iter = std::next(q.insert(iter, {Event::sessionWaitForData}));
        return q;
    }

private:
    ScopedValueOverride<bool> _svo{gInitialUseDedicatedThread, false};
};

TEST_F(SessionWorkflowWithBorrowedThreadsTest, DefaultLoop) {
    runSteps(convertStepsToBorrowed(defaultLoop()));
}

TEST_F(SessionWorkflowWithBorrowedThreadsTest, ExhaustLoop) {
    runSteps(convertStepsToBorrowed(exhaustLoop()));
}

TEST_F(SessionWorkflowWithBorrowedThreadsTest, MoreToComeLoop) {
    runSteps(convertStepsToBorrowed(moreToComeLoop()));
}

}  // namespace
}  // namespace mongo::transport
