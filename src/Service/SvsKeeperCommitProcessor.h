#pragma once

#include <Service/RequestsQueue.h>
#include <Service/SvsKeeperServer.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int RAFT_ERROR;
}

class SvsKeeperCommitProcessor
{
using Request = SvsKeeperStorage::RequestForSession;

public:
    SvsKeeperCommitProcessor(
        RequestsCommitEvent & requests_commit_event_, SvsKeeperResponsesQueue & responses_queue_)
        : requests_queue(std::make_shared<RequestsQueue>(1, 20000)), requests_commit_event(requests_commit_event_), responses_queue(responses_queue_), log(&Poco::Logger::get("SvsKeeperCommitProcessor"))
    {
        main_thread = ThreadFromGlobalPool([this] { run3(); });
    }

    void processRequest(Request request_for_session)
    {
        if (!shutdown_called) {
            requests_queue->push(request_for_session);
            cv.notify_all();
        }
    }

    void run()
    {
        while (!shutdown_called)
        {
//            UInt64 max_wait = UInt64(configuration_and_settings->coordination_settings->operation_timeout_ms.totalMilliseconds());
            try
            {
                UInt64 max_wait = 10000;

                SvsKeeperStorage::RequestForSession request_for_session;
                if (!requests_queue->tryPop(0, request_for_session, max_wait))
                {
                    continue;
                }

                if (!request_for_session.request->isReadRequest())
                {
                    LOG_TRACE(log, "wait commit session {}, xid {}", request_for_session.session_id, request_for_session.request->xid);
                    requests_commit_event.waitForCommit(request_for_session.session_id, request_for_session.request->xid);
                    LOG_TRACE(log, "wait commit done session {}, xid {}", request_for_session.session_id, request_for_session.request->xid);
                }

                if (requests_commit_event.isError(request_for_session.session_id, request_for_session.request->xid))
                {
                    auto response = request_for_session.request->makeResponse();

                    response->xid = request_for_session.request->xid;
                    response->zxid = 0;

                    auto [accepted, error_code] = requests_commit_event.getError(request_for_session.session_id, request_for_session.request->xid);
                    response->error = error_code == nuraft::cmd_result_code::TIMEOUT ? Coordination::Error::ZOPERATIONTIMEOUT
                                                                                     : Coordination::Error::ZCONNECTIONLOSS;

                    responses_queue.push(DB::SvsKeeperStorage::ResponseForSession{request_for_session.session_id, response});

                    requests_commit_event.eraseError(request_for_session.session_id, request_for_session.request->xid);

                    if (!accepted)
                        throw Exception(ErrorCodes::RAFT_ERROR,
                                        "Request batch is not accepted.");
                    else
                        throw Exception(ErrorCodes::RAFT_ERROR,
                                        "Request batch error, nuraft code {}", error_code);

                }
                else /// TODO leader alive
                {
                    server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, request_for_session.request, request_for_session.session_id, {}, true, false);
                }
            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
            }
        }
    }

    void run1()
    {
        while (!shutdown_called)
        {
            //            UInt64 max_wait = UInt64(configuration_and_settings->coordination_settings->operation_timeout_ms.totalMilliseconds());
            std::optional<Request> nextPending;
            SvsKeeperStorage::RequestsForSessions toProcess;
            try
            {
                int len = toProcess.size();
                for (int i = 0; i < len; i++)
                {
                    server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, toProcess[i].request, toProcess[i].session_id, {}, true, false);
                }
                toProcess.clear();

                {
                    std::unique_lock lk(mutex);

                    cv.wait(lk, [&]{return !((requests_queue->size() == 0 || nextPending)
                                               && committed_queue.size() == 0);});

                    // First check and see if the commit came in for the pending
                    // request
                    if ((requests_queue->size() == 0 || nextPending)
                        && committed_queue.size() > 0) {
                        Request r;
                        committed_queue.tryPop(r);
                        /*
                     * We match with nextPending so that we can move to the
                     * next request when it is committed. We also want to
                     * use nextPending because it has the cnxn member set
                     * properly.
                     */
                        if (nextPending
                            && nextPending->session_id == r.session_id
                            && nextPending->request->xid == r.request->xid) {
                            // we want to send our version of the request.
                            // the pointer to the connection in the request
                            //                            nextPending.hdr = r.hdr;
                            //                            nextPending.txn = r.txn;
                            //                            nextPending.zxid = r.zxid;
                            toProcess.push_back(*nextPending);
                            nextPending.reset();
                        } else {
                            // this request came from someone else so just
                            // send the commit packet
                            toProcess.push_back(r);
                        }
                    }
                }


                // We haven't matched the pending requests, so go back to
                // waiting
                if (nextPending) {
                    continue;
                }

                {
                    std::unique_lock lk(mutex);
                    // Process the next requests in the queuedRequests
                    while (!nextPending && requests_queue->size() > 0)
                    {
                        //                        Request request = queuedRequests.remove();
                        Request request;
                        requests_queue->tryPop(0, request);
                        if (!request.request->isReadRequest())
                        {
                            nextPending = request;
                        }
                        else
                        {
                            toProcess.push_back(request);
                        }
                    }
                }
            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
            }
        }
    }

    /// for each requests_queue and committed_queue, if requests_queue pop a write request need wait it appear in the committed_queue.
    /// So for each requests_queue pop a read request process it, wait write request commit, for each committed_queue find current wait write request and process other request.
    void run2()
    {
        std::optional<Request> pending_write_request;
        while (!shutdown_called)
        {
            //            UInt64 max_wait = UInt64(configuration_and_settings->coordination_settings->operation_timeout_ms.totalMilliseconds());
            try
            {
                auto need_wait = [&]()-> bool
                {
                    bool wait = false;
                    if (pending_write_request)
                    {
                        if (requests_commit_event.isError(pending_write_request->session_id, pending_write_request->request->xid))
                            wait = false;

                        if (committed_queue.empty())
                        {
                            wait = true;
                        }
                    }
                    else
                    {
                        if (requests_queue->empty() && committed_queue.empty())
                            wait = true;
                    }
                    return wait;
                };

                {
                    std::unique_lock lk(mutex);
                    if (pending_write_request)
                    {
                        LOG_TRACE(log, "wait pending_write_request has value {}, {}", pending_write_request->session_id, pending_write_request->request->xid);
                    }
                    else
                    {
                        LOG_TRACE(log, "wait pending_write_request not has value, requests_queue->size {}, committed_queue.size {}", requests_queue->size(), committed_queue.size());
                    }

                    cv.wait(lk, [&]{ return !need_wait() || shutdown_called; });

                    if (pending_write_request)
                    {
                        LOG_TRACE(log, "wait done pending_write_request has value {}, {}", pending_write_request->session_id, pending_write_request->request->xid);
                    }
                    else
                    {
                        LOG_TRACE(log, "wait done pending_write_request not has value, requests_queue->size {}, committed_queue.size {}", requests_queue->size(), committed_queue.size());
                    }
                }

                if (shutdown_called)
                    return;

                if (pending_write_request)
                {
                    if (requests_commit_event.isError(pending_write_request->session_id, pending_write_request->request->xid))
                    {
                        auto response = pending_write_request->request->makeResponse();

                        response->xid = pending_write_request->request->xid;
                        response->zxid = 0;

                        auto [accepted, error_code] = requests_commit_event.getError(pending_write_request->session_id, pending_write_request->request->xid);
                        response->error = error_code == nuraft::cmd_result_code::TIMEOUT ? Coordination::Error::ZOPERATIONTIMEOUT
                                                                                         : Coordination::Error::ZCONNECTIONLOSS;

                        responses_queue.push(DB::SvsKeeperStorage::ResponseForSession{pending_write_request->session_id, response});

                        requests_commit_event.eraseError(pending_write_request->session_id, pending_write_request->request->xid);

                        if (!accepted)
                            throw Exception(ErrorCodes::RAFT_ERROR,
                                            "Request batch is not accepted.");
                        else
                            throw Exception(ErrorCodes::RAFT_ERROR,
                                            "Request batch error, nuraft code {}", error_code);

                    }
                }


                size_t committed_request_size = committed_queue.size();
                size_t request_size = requests_queue->size();

                Request request;
                if (!pending_write_request)
                {
                    for (size_t i = 0; i < request_size; ++i)
                    {
                        if (requests_queue->tryPop(0, request))
                        {
                            if (request.request->isReadRequest())
                            {
                                LOG_TRACE(log, "ReadRequest i {}, session {}, xid {}", i, request.session_id, request.request->xid);
                                server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, request.request, request.session_id, {}, true, false);
                            }
                            else
                            {
                                pending_write_request = request;
                                LOG_TRACE(log, "pending_write_request i {}, session {}, xid {}", i, pending_write_request->session_id, pending_write_request->request->xid);
                                break;
                            }
                        }
                    }
                }

                LOG_TRACE(log, "committed_request_size {}", committed_request_size);
                Request committed_request;
                for (size_t i = 0; i < committed_request_size; ++i)
                {
                    if (committed_queue.tryPop(committed_request))
                    {
                        if (pending_write_request && committed_request.request->xid == pending_write_request->request->xid
                            && committed_request.session_id == pending_write_request->session_id)
                        {
                            LOG_TRACE(log, "match committed_request and pending_write_request i {}, session {}, xid {}", i, pending_write_request->session_id, pending_write_request->request->xid);
                            server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, committed_request.request, committed_request.session_id, {}, true, false);
                            pending_write_request.reset();
                            break;
                        }
                        else
                        {
                            LOG_TRACE(log, "not match committed_request and pending_write_request i {}, session {}, xid {}", i, committed_request.session_id, committed_request.request->xid);
                            server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, committed_request.request, committed_request.session_id, {}, true, false);
                        }
                    }
                }

            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
            }
        }
    }

    void run3()
    {
        while (!shutdown_called)
        {
            //            UInt64 max_wait = UInt64(configuration_and_settings->coordination_settings->operation_timeout_ms.totalMilliseconds());
            try
            {
                auto need_wait = [&]()-> bool
                {
                    if (errors.empty() && pending_requests.empty() && requests_queue->empty() && committed_queue.empty())
                        return true;

                    return false;
                };

                {
                    std::unique_lock lk(mutex);

                    cv.wait(lk, [&]{ return !need_wait() || shutdown_called; });
                }

                if (shutdown_called)
                    return;

                {
                    std::lock_guard lock(errors_mutex);
                    if (!errors.empty())
                    {

                        for (auto it = errors.begin(); it != errors.end();)
                        {
                            auto & [ session_id, xid ] = it->first;

                            LOG_TRACE(log, "error session {}, xid {}", session_id, xid);

                            auto & requests = pending_requests.find(session_id)->second;

                            std::optional<Request> request;
                            for (auto request_it = requests.begin(); request_it != requests.end();)
                            {
                                if (uint64_t(request_it->request->xid) == xid)
                                {
                                    request = *request_it;
                                    request_it = requests.erase(request_it);
                                    break;
                                }
                                else
                                {
                                    ++request_it;
                                }
                            }

                            auto & w_requests = pending_write_requests.find(session_id)->second;
                            for (auto w_request_it = w_requests.begin(); w_request_it != w_requests.end();)
                            {
                                if (uint64_t(w_request_it->request->xid) == xid)
                                {
                                    w_request_it = w_requests.erase(w_request_it);
                                    break;
                                }
                                else
                                {
                                    ++w_request_it;
                                }
                            }

                            if (request)
                            {
                                auto response = request->request->makeResponse();

                                response->xid = request->request->xid;
                                response->zxid = 0;

                                auto [accepted, error_code] = it->second;
                                response->error = error_code == nuraft::cmd_result_code::TIMEOUT ? Coordination::Error::ZOPERATIONTIMEOUT
                                                                                                 : Coordination::Error::ZCONNECTIONLOSS;

                                responses_queue.push(DB::SvsKeeperStorage::ResponseForSession{request->session_id, response});

                                it = errors.erase(it);

                                if (!accepted)
                                    throw Exception(ErrorCodes::RAFT_ERROR, "Request batch is not accepted.");
                                else
                                    throw Exception(ErrorCodes::RAFT_ERROR, "Request batch error, nuraft code {}", error_code);

                            }
                            else
                            {
                                throw Exception(ErrorCodes::RAFT_ERROR, "Logic Error");
                            }
                        }
                    }
                }

                size_t committed_request_size = committed_queue.size();
                size_t request_size = requests_queue->size();

                LOG_TRACE(log, "request_size {}", request_size);
                for (size_t i = 0; i < request_size; ++i)
                {
                    Request request;
                    if (requests_queue->tryPop(0, request))
                    {
                        pending_requests[request.session_id].push_back(request);
                        if (!request.request->isReadRequest())
                        {
                            pending_write_requests[request.session_id].push_back(request);
                        }
                    }
                }

                /// process every session, until encountered write request
                for (auto it = pending_requests.begin(); it != pending_requests.end();)
                {
                    auto current_session_id = it->first;

                    auto & requests = it->second;
                    for (auto requets_it = requests.begin(); requets_it != requests.end();)
                    {
                        LOG_TRACE(log, "current_session_id {}, requets_it->request->xid {}", current_session_id, requets_it->request->xid);
                        if (pending_write_requests[current_session_id].empty() || requets_it->request->xid < pending_write_requests[current_session_id].begin()->request->xid)
                        {
                            /// read request
                            LOG_TRACE(log, "current_session_id {}, pending head write requests xid {}", current_session_id, pending_write_requests[current_session_id].begin()->request->xid);
                            if (!requets_it->request->isReadRequest())
                                throw Exception(ErrorCodes::RAFT_ERROR, "Logic Error, request requried read request");

                            server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, requets_it->request, requets_it->session_id, {}, true, false);
                            requets_it = requests.erase(requets_it);
                        }
                        else
                        {
                            break;
                        }
                    }

                    if (requests.empty())
                        it = pending_requests.erase(it);
                    else
                        ++it;
                }


                LOG_TRACE(log, "committed_request_size {}", committed_request_size);
                Request committed_request;
                for (size_t i = 0; i < committed_request_size; ++i)
                {
                    if (committed_queue.tryPop(committed_request))
                    {
                        auto & current_session_pending_w_requests = pending_write_requests[committed_request.session_id];
                        if (current_session_pending_w_requests.empty()) /// another server session request
                        {
                            server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, committed_request.request, committed_request.session_id, {}, true, false);
                        }
                        else
                        {
                            if (current_session_pending_w_requests.begin()->request->xid != committed_request.request->xid)
                                throw Exception(ErrorCodes::RAFT_ERROR, "Logic Error, current session {} pending head write request xid {} not same committed request xid {}", committed_request.session_id, current_session_pending_w_requests.begin()->request->xid, committed_request.request->xid);

                            auto & current_session_pending_requests = pending_requests[committed_request.session_id];
                            if (current_session_pending_requests.begin()->request->xid != committed_request.request->xid)
                                throw Exception(ErrorCodes::RAFT_ERROR, "Logic Error, current session {} pending head request xid {} not same committed request xid {}", committed_request.session_id, current_session_pending_requests.begin()->request->xid, committed_request.request->xid);

                            server->getKeeperStateMachine()->getStorage().processRequest(responses_queue, committed_request.request, committed_request.session_id, {}, true, false);

                            current_session_pending_w_requests.erase(current_session_pending_w_requests.begin());
                            current_session_pending_requests.erase(current_session_pending_requests.begin());

                            if (current_session_pending_w_requests.empty())
                                pending_write_requests.erase(committed_request.session_id);

                            if (current_session_pending_requests.empty())
                                pending_requests.erase(committed_request.session_id);
                        }
                    }
                }

            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
            }
        }
    }

    void shutdown()
    {
        if (shutdown_called)
            return;

        shutdown_called = true;
        cv.notify_all();

        if (main_thread.joinable())
            main_thread.join();

        SvsKeeperStorage::RequestForSession request_for_session;
        while (requests_queue->tryPop(0,request_for_session))
        {
            auto response = request_for_session.request->makeResponse();
            response->xid = request_for_session.request->xid;
            response->zxid = 0;
            response->error = Coordination::Error::ZSESSIONEXPIRED;
            responses_queue.push(DB::SvsKeeperStorage::ResponseForSession{request_for_session.session_id, response});
        }
    }

    void setRaftServer(std::shared_ptr<SvsKeeperServer> server_)
    {
        server = server_;
    }

    void commit(Request request) {
        if (!shutdown_called) {
            committed_queue.push(request);
            cv.notify_all();
        }
    }


    void onError(int64_t session_id, int64_t xid, bool accepted, nuraft::cmd_result_code error_code) {
        if (!shutdown_called) {
            std::lock_guard lock(errors_mutex);
            std::pair<bool, nuraft::cmd_result_code> v{ accepted, error_code };
            errors.emplace(UInt128(session_id, xid), v);
            cv.notify_all();
        }
    }

    void notifyOnError() {
        if (!shutdown_called) {
            cv.notify_all();
        }
    }

private:
    ptr<RequestsQueue> requests_queue;

    ThreadFromGlobalPool main_thread;

    bool shutdown_called{false};

//    size_t thread_index;

    RequestsCommitEvent & requests_commit_event;

    std::shared_ptr<SvsKeeperServer> server;

    SvsKeeperResponsesQueue & responses_queue;

    SvsKeeperThreadSafeQueue<SvsKeeperStorage::RequestForSession> committed_queue;

    using RequestForSessions = std::vector<SvsKeeperStorage::RequestForSession>;
    std::unordered_map<int64_t, RequestForSessions> pending_write_requests;

    std::unordered_map<int64_t, RequestForSessions> pending_requests;

    mutable std::mutex errors_mutex;
    /// key : session_id xid
    std::unordered_map<UInt128, std::pair<bool, nuraft::cmd_result_code>> errors;

    std::mutex mutex;

    std::condition_variable cv;

    Poco::Logger * log;
};

}