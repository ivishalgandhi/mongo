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

#pragma once

#include <list>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "mongo/executor/network_interface.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;

namespace executor {

class NetworkConnectionHook;

/**
 * Mock network implementation for use in unit tests.
 *
 * To use, construct a new instance on the heap, and keep a pointer to it.  Pass
 * the pointer to the instance into the TaskExecutor constructor, transferring
 * ownership.  Start the executor's run() method in a separate thread, schedule the
 * work you want to test into the executor, then while the test is still going, iterate
 * through the ready network requests, servicing them and advancing time as needed.
 *
 * The mock has a fully virtualized notion of time and the the network.  When the
 * executor under test schedules a network operation, the startCommand
 * method of this class adds an entry to the _unscheduled queue for immediate consideration.
 * The test driver loop, when it examines the request, may schedule a response, ask the
 * interface to redeliver the request at a later virtual time, or to swallow the virtual
 * request until the end of the simulation.  The test driver loop can also instruct the
 * interface to run forward through virtual time until there are operations ready to
 * consider, via runUntil.
 *
 * The thread acting as the "network" and the executor run thread are highly synchronized
 * by this code, allowing for deterministic control of operation interleaving.
 */
class NetworkInterfaceMock : public NetworkInterface {
public:
    class NetworkOperation;
    using NetworkOperationList = std::list<NetworkOperation>;
    using NetworkOperationIterator = NetworkOperationList::iterator;

    /**
     * This struct encapsulates the original Request as well as response data and metadata.
     */
    struct NetworkResponse {
        NetworkOperationIterator noi;
        Date_t when;
        TaskExecutor::ResponseStatus response;
    };
    using NetworkResponseList = std::list<NetworkResponse>;

    NetworkInterfaceMock();
    virtual ~NetworkInterfaceMock();

    ////////////////////////////////////////////////////////////////////////////////
    //
    // NetworkInterface methods
    //
    ////////////////////////////////////////////////////////////////////////////////

    void appendConnectionStats(ConnectionPoolStats* stats) const override {}

    std::string getDiagnosticString() override;

    Counters getCounters() const override {
        return Counters();
    }

    void startup() override;
    void shutdown() override;
    bool inShutdown() const override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    std::string getHostName() override;
    Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                        RemoteCommandRequestOnAny& request,
                        RemoteCommandCompletionFn&& onFinish,
                        const BatonHandle& baton = nullptr) override;
    Status startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               RemoteCommandRequestOnAny& request,
                               RemoteCommandOnReplyFn&& onReply,
                               const BatonHandle& baton = nullptr) override;

    /**
     * If the network operation is in the _unscheduled or _processing queues, moves the operation
     * into the _scheduled queue with ErrorCodes::CallbackCanceled. If the operation is already in
     * the _scheduled queue, does nothing. The latter simulates the case where cancelCommand() is
     * called after the task has already completed, but its callback has not yet been run.
     */
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton = nullptr) override;

    Status setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                    Date_t when,
                    unique_function<void(Status)> action) override;

    void cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) override;

    Status schedule(unique_function<void(Status)> action) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort&) override {}

    void testEgress(const HostAndPort&, transport::ConnectSSLMode, Milliseconds, Status) override {}

    ////////////////////////////////////////////////////////////////////////////////
    //
    // Methods for simulating network operations and the passage of time.
    //
    // Methods in this section are to be called by the thread currently simulating
    // the network.
    //
    ////////////////////////////////////////////////////////////////////////////////

    /**
     * RAII-style class for entering and exiting network.
     */
    class InNetworkGuard;

    void setConnectionHook(std::unique_ptr<NetworkConnectionHook> hook);

    void setEgressMetadataHook(std::unique_ptr<rpc::EgressMetadataHook> metadataHook);

    /**
     * Causes the currently running (non-executor) thread to assume the mantle of the network
     * simulation thread.
     *
     * Call this before calling any of the other methods in this section.
     */
    void enterNetwork();

    /**
     * Causes the currently running thread to drop the mantle of "network simulation thread".
     *
     * Call this before calling any methods that might block waiting for the
     * executor thread.
     *
     * It is safe to call exitNetwork() even if enterNetwork() has not been called - it will just
     * be a no-op.
     */
    void exitNetwork();

    /**
     * Returns true if there are unscheduled network requests to be processed.
     *
     * This will not notice exhaust operations that have not yet finished but have processed all of
     * their available responses.
     */
    bool hasReadyRequests();

    /**
     * Gets the next unscheduled request to process, blocking until one is available.
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    NetworkOperationIterator getNextReadyRequest();

    /**
     * Gets the first unscheduled request. There must be at least one unscheduled request in the
     * queue. Equivalent to getNthUnscheduledRequest(0).
     */
    NetworkOperationIterator getFrontOfUnscheduledQueue();

    /**
     * Get the nth (starting at 0) unscheduled request. Assumes there are at least n+1 unscheduled
     * requests in the queue.
     */
    NetworkOperationIterator getNthUnscheduledRequest(size_t n);

    /**
     * Schedules "response" in response to "noi" at virtual time "when".
     */
    void scheduleResponse(NetworkOperationIterator noi,
                          Date_t when,
                          const TaskExecutor::ResponseStatus& response);

    /**
     * Schedules a successful "response" to "noi" at virtual time "when".
     * "noi" defaults to next ready request.
     * "when" defaults to now().
     * Returns the "request" that the response was scheduled for.
     */
    RemoteCommandRequest scheduleSuccessfulResponse(const BSONObj& response);
    RemoteCommandRequest scheduleSuccessfulResponse(const RemoteCommandResponse& response);
    RemoteCommandRequest scheduleSuccessfulResponse(NetworkOperationIterator noi,
                                                    const RemoteCommandResponse& response);
    RemoteCommandRequest scheduleSuccessfulResponse(NetworkOperationIterator noi,
                                                    Date_t when,
                                                    const RemoteCommandResponse& response);

    /**
     * Schedules an error "response" to "noi" at virtual time "when".
     * "noi" defaults to next ready request.
     * "when" defaults to now().
     */
    RemoteCommandRequest scheduleErrorResponse(const Status& response);
    RemoteCommandRequest scheduleErrorResponse(TaskExecutor::ResponseStatus response);
    RemoteCommandRequest scheduleErrorResponse(NetworkOperationIterator noi,
                                               const Status& response);
    RemoteCommandRequest scheduleErrorResponse(NetworkOperationIterator noi,
                                               Date_t when,
                                               const Status& response);

    /**
     * Swallows "noi", causing the network interface to not respond to it until
     * shutdown() is called.
     */
    void blackHole(NetworkOperationIterator noi);

    /**
     * Runs the simulator forward until now() == until or hasReadyRequests() is true.
     * Returns now().
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    Date_t runUntil(Date_t until);

    /**
     * Runs the simulator forward until now() == until.
     */
    void advanceTime(Date_t newTime);

    /**
     * Processes all ready, scheduled network operations.
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    void runReadyNetworkOperations();

    /**
     * Sets the reply of the 'isMaster' handshake for a specific host. This reply will only
     * be given to the 'validateHost' method of the ConnectionHook set on this object - NOT
     * to the completion handlers of any 'isMaster' commands scheduled with 'startCommand'.
     *
     * This reply will persist until it is changed again using this method.
     *
     * If the NetworkInterfaceMock conducts a handshake with a simulated host which has not
     * had a handshake reply set, a default constructed RemoteCommandResponse will be passed
     * to validateHost if a hook is set.
     */
    void setHandshakeReplyForHost(const HostAndPort& host, RemoteCommandResponse&& reply);

    /**
     * Deliver the response to the callback handle if the handle is present.
     * This represents interrupting the regular flow with, for example, a NetworkTimeout or
     * CallbackCanceled error.
     */
    void _interruptWithResponse_inlock(const TaskExecutor::CallbackHandle& cbHandle,
                                       const TaskExecutor::ResponseStatus& response);

    /**
     * Returns true if there is no scheduled work (i.e. alarms and scheduled responses) for the
     * network thread to process.
     */
    bool hasReadyNetworkOperations();

private:
    /**
     * Information describing a scheduled alarm.
     */
    struct AlarmInfo {
        using AlarmAction = unique_function<void(Status)>;
        AlarmInfo(TaskExecutor::CallbackHandle inHandle, Date_t inWhen, AlarmAction inAction)
            : handle(inHandle), when(inWhen), action(std::move(inAction)) {}
        bool operator>(const AlarmInfo& rhs) const {
            return when > rhs.when;
        }

        TaskExecutor::CallbackHandle handle;
        Date_t when;
        mutable AlarmAction action;
    };

    /**
     * Type used to identify which thread (network mock or executor) is currently executing.
     *
     * Values are used in a bitmask, as well.
     */
    enum ThreadType { kNoThread = 0, kExecutorThread = 1, kNetworkThread = 2 };

    /**
     * Implementation of startup behavior.
     */
    void _startup_inlock();

    /**
     * Returns the current virtualized time.
     */
    Date_t _now_inlock() const {
        return _clkSource->now();
    }

    /**
     * Implementation of waitForWork*.
     */
    void _waitForWork_inlock(stdx::unique_lock<stdx::mutex>* lk);

    /**
     * Returns true if there are ready requests for the network thread to service.
     */
    bool _hasReadyRequests_inlock();

    /**
     * Returns true if the network thread could run right now.
     */
    bool _isNetworkThreadRunnable_inlock();

    /**
     * Returns true if the executor thread could run right now.
     */
    bool _isExecutorThreadRunnable_inlock();

    /**
     * Enqueues a network operation to run in order of 'consideration date'.
     */
    void _enqueueOperation_inlock(NetworkOperation&& op);

    /**
     * "Connects" to a remote host, and then enqueues the provided operation.
     */
    void _connectThenEnqueueOperation_inlock(const HostAndPort& target, NetworkOperation&& op);

    /**
     * Enqueues a response to be processed the next time we runReadyNetworkOperations.
     *
     * Note that interruption and timeout also invoke this function.
     */
    void _scheduleResponse_inlock(NetworkOperationIterator noi,
                                  Date_t when,
                                  const TaskExecutor::ResponseStatus& response);

    /**
     * Runs all ready network operations, called while holding "lk".  May drop and
     * reaquire "lk" several times, but will not return until the executor has blocked
     * in waitFor*.
     */
    void _runReadyNetworkOperations_inlock(stdx::unique_lock<stdx::mutex>* lk);

    template <typename CallbackFn>
    Status _startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                         RemoteCommandRequestOnAny& request,
                         CallbackFn&& onReply,
                         const BatonHandle& baton = nullptr);

    // Mutex that synchronizes access to mutable data in this class and its subclasses.
    // Fields guarded by the mutex are labled (M), below, and those that are read-only
    // in multi-threaded execution, and so unsynchronized, are labeled (R).
    stdx::mutex _mutex;  // NOLINT

    // A mocked clock source.
    std::unique_ptr<ClockSourceMock> _clkSource;  // (M)

    // Condition signaled to indicate that the network processing thread should wake up.
    stdx::condition_variable _shouldWakeNetworkCondition;  // (M)

    // Condition signaled to indicate that the executor run thread should wake up.
    stdx::condition_variable _shouldWakeExecutorCondition;  // (M)

    // Bitmask indicating which threads are runnable.
    int _waitingToRunMask;  // (M)

    // Indicator of which thread, if any, is currently running.
    ThreadType _currentlyRunning;  // (M)

    // Set to true by "startUp()"
    bool _hasStarted;  // (M)

    // Set to true by "shutDown()".
    AtomicWord<bool> _inShutdown;  // (M)

    // Next date that the executor expects to wake up at (due to a scheduleWorkAt() call).
    Date_t _executorNextWakeupDate;  // (M)

    // The list of operations that have been submitted via startCommand. Operations are never
    // deleted from this list, thus NetworkOperationIterators are valid for the lifetime of the
    // NetworkInterfaceMock.
    NetworkOperationList _operations;  // (M)

    // The list of responses that have been enqueued from scheduleResponse(), cancellation, or
    // timeout. This list is ordered by NetworkResponse::when and is drained front to back by
    // runReadyNetworkOperations().
    NetworkResponseList _responses;  // (M)

    // Heap of alarms, with the next alarm always on top.
    std::priority_queue<AlarmInfo, std::vector<AlarmInfo>, std::greater<AlarmInfo>> _alarms;  // (M)

    // A set of CallbackHandles for canceled alarms
    stdx::unordered_set<TaskExecutor::CallbackHandle> _canceledAlarms;  // (M^:)

    // The connection hook.
    std::unique_ptr<NetworkConnectionHook> _hook;  // (R)

    // The metadata hook.
    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;  // (R)

    // The set of hosts we have seen so far. If we see a new host, we will execute the
    // ConnectionHook's validation and post-connection logic.
    //
    // TODO: provide a way to simulate disconnections.
    stdx::unordered_set<HostAndPort> _connections;  // (M)

    // The handshake replies set for each host.
    stdx::unordered_map<HostAndPort, RemoteCommandResponse> _handshakeReplies;  // (M)
};

/**
 * Representation of an in-progress network operation.
 */
class NetworkInterfaceMock::NetworkOperation {
    using ResponseCallback = unique_function<void(const TaskExecutor::ResponseOnAnyStatus&)>;

public:
    NetworkOperation();
    NetworkOperation(const TaskExecutor::CallbackHandle& cbHandle,
                     const RemoteCommandRequestOnAny& theRequest,
                     Date_t theRequestDate,
                     ResponseCallback onResponse);

    /**
     * Mark the operation as observed by the networking thread. This is equivalent to a remote node
     * processing the operation.
     */
    void markAsProcessing() {
        _isProcessing = true;
    }

    /**
     * Mark the operation as blackholed by the networking thread.
     */
    void markAsBlackholed() {
        _isProcessing = true;
        _isBlackholed = true;
    }

    /**
     * Process a response to an ongoing operation.
     *
     * This invokes the _onResponse callback and may throw.
     */
    bool processResponse(NetworkResponse response);

    /**
     * Predicate that returns true if cbHandle equals the executor's handle for this network
     * operation.  Used for searching lists of NetworkOperations.
     */
    bool isForCallback(const TaskExecutor::CallbackHandle& cbHandle) const {
        return cbHandle == _cbHandle;
    }

    const TaskExecutor::CallbackHandle& getCallbackHandle() const {
        return _cbHandle;
    }

    /**
     * Gets the request that initiated this operation.
     */
    const RemoteCommandRequestOnAny& getRequestOnAny() const {
        return _requestOnAny;
    }

    /**
     * Gets the request that initiated this operation.
     */
    const RemoteCommandRequest& getRequest() const {
        return _request;
    }

    /**
     * Returns true if this operation has not been observed via getNextReadyRequest(), been
     * canceled, or timed out.
     */
    bool hasReadyRequest() const {
        return !_isProcessing && !_isFinished;
    }

    /**
     * Assert that this operation has not been blackholed.
     */
    void assertNotBlackholed() {
        uassert(5440603, "Response scheduled for a blackholed operation", !_isBlackholed);
    }

    /**
     * Gets the virtual time at which the operation was started.
     */
    Date_t getRequestDate() const {
        return _requestDate;
    }

    /**
     * Returns a printable diagnostic string.
     */
    std::string getDiagnosticString() const;

private:
    Date_t _requestDate;
    TaskExecutor::CallbackHandle _cbHandle;
    RemoteCommandRequestOnAny _requestOnAny;
    RemoteCommandRequest _request;

    bool _isProcessing = false;
    bool _isBlackholed = false;
    bool _isFinished = false;

    ResponseCallback _onResponse;
};

/**
 * RAII type to enter and exit network on construction/destruction.
 *
 * Calls enterNetwork on construction, and exitNetwork during destruction,
 * unless dismissed.
 *
 * Not thread-safe.
 */
class NetworkInterfaceMock::InNetworkGuard {
    InNetworkGuard(const InNetworkGuard&) = delete;
    InNetworkGuard& operator=(const InNetworkGuard&) = delete;

public:
    /**
     * Calls enterNetwork.
     */
    explicit InNetworkGuard(NetworkInterfaceMock* net);
    /**
     * Calls exitNetwork, and disables the destructor from calling.
     */
    void dismiss();
    /**
     * Calls exitNetwork, unless dismiss has been called.
     */
    ~InNetworkGuard();

    /**
     * Returns network interface mock pointer.
     */
    NetworkInterfaceMock* operator->() const;

private:
    NetworkInterfaceMock* _net;
    bool _callExitNetwork = true;
};

}  // namespace executor
}  // namespace mongo
