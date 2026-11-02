#pragma once
#include <atomic>
#include <cassert>
#include <thread>
#include <utility>

namespace switcher_test {
    /** Fake driver: stop returns only after its callback thread has joined. */
    class CallbackThread final {
    public:
        CallbackThread()=default;
        CallbackThread(const CallbackThread&)=delete;
        CallbackThread& operator=(const CallbackThread&)=delete;
        ~CallbackThread() { stop(); }
        template<class Callback> void start(Callback callback) {
            assert(!worker_.joinable());
            stopRequested.store(false,std::memory_order_relaxed);
            running_.store(true,std::memory_order_release);
            worker_=std::thread([this,callback=std::move(callback)]() mutable {
                while (running_.load(std::memory_order_acquire)) callback();
            });
        }
        void stop() {
            running_.store(false,std::memory_order_release);
            stopRequested.store(true,std::memory_order_release);
            if (worker_.joinable()) worker_.join();
        }
        std::atomic<bool> stopRequested{false};
    private:
        std::atomic<bool> running_{false};
        std::thread worker_;
    };
}
