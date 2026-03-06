#pragma once
#include <mutex>
#include <unordered_set>

class SessionTracker {
public:
    class SessionBase {
    public:
        virtual ~SessionBase() = default;
        virtual void shutdown() = 0;
    };

    static SessionTracker& instance() {
        static SessionTracker st;
        return st;
    }

    void add(SessionBase* session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.insert(session);
    }

    void remove(SessionBase* session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session);
    }

    void shutdownAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* session : sessions_) {
            session->shutdown();
        }
    }

private:
    std::mutex mutex_;
    std::unordered_set<SessionBase*> sessions_;
};
