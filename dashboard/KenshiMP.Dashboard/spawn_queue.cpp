#include "spawn_queue.h"
#include <Windows.h>
#include <stdio.h>

namespace kmp::dash {

bool StubDispatcher::Dispatch(const SpawnRequest& req, SpawnResult& out) {
    out.ok = false;
    out.netId = req.netId;
    out.entityAddr = 0;
    out.failureReason = "stub dispatcher (named pipe to SafeAddon not yet wired)";
    char buf[256];
    sprintf_s(buf,
        "KMP Dashboard: WOULD spawn netId=%llu template='%s' at "
        "(%.1f,%.1f,%.1f) factionId=%u (stubbed)\n",
        (unsigned long long)req.netId, req.templateName.c_str(),
        req.x, req.y, req.z, req.factionId);
    OutputDebugStringA(buf);
    return false;
}

void SpawnQueue::Submit(SpawnRequest req) {
    req.queuedAt = std::chrono::steady_clock::now();
    std::lock_guard lk(m_mu);
    m_q.push_back(std::move(req));
}

void SpawnQueue::DrainTick() {
    // Snapshot the queue under the lock, then dispatch outside the
    // lock so a slow Dispatch can't block Submit.  Failed requests
    // get re-queued at the back with their attempt counter bumped.
    std::deque<SpawnRequest> work;
    {
        std::lock_guard lk(m_mu);
        work.swap(m_q);
    }
    std::deque<SpawnRequest> retry;
    while (!work.empty()) {
        SpawnRequest req = std::move(work.front());
        work.pop_front();
        SpawnResult res;
        bool ok = m_disp.Dispatch(req, res);
        if (ok) {
            m_completed.fetch_add(1);
            continue;
        }
        ++req.attempts;
        if (req.attempts >= kMaxAttempts) {
            m_failed.fetch_add(1);
            char buf[256];
            sprintf_s(buf,
                "KMP Dashboard: dropping spawn netId=%llu after %d attempts "
                "(reason: %s)\n",
                (unsigned long long)req.netId, req.attempts,
                res.failureReason.c_str());
            OutputDebugStringA(buf);
            continue;
        }
        retry.push_back(std::move(req));
    }
    if (!retry.empty()) {
        std::lock_guard lk(m_mu);
        for (auto& r : retry) m_q.push_back(std::move(r));
    }
}

size_t SpawnQueue::PendingCount() const {
    std::lock_guard lk(m_mu);
    return m_q.size();
}

void SpawnQueue::Clear() {
    std::lock_guard lk(m_mu);
    m_q.clear();
    m_completed.store(0);
    m_failed.store(0);
}

} // namespace kmp::dash
