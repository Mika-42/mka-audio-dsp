/**
 * @file switcher_backend_fixture.hpp
 * @brief Test fixtures for switcher backend fixture.
 * @author Mika
 */
#pragma once
#include <atomic>
#include <cassert>
#include <thread>
#include <utility>

namespace switcher_test {
    /** Fake driver: stop returns only after its callback thread has joined. */
    class CallbackThread final {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        CallbackThread()=default;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        CallbackThread(const CallbackThread&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        CallbackThread& operator=(const CallbackThread&)=delete;
        /// @brief Record fixture destruction and release owned state on the calling thread.
        ~CallbackThread() { stop(); }
        /// @brief Start the fake driver thread and invoke its callback until stop is requested.
        template<class Callback> void start(Callback callback) {
            assert(!worker_.joinable());
            stopRequested.store(false,std::memory_order_relaxed);
            running_.store(true,std::memory_order_release);
            worker_=std::thread([this,callback=std::move(callback)]() mutable {
                while (running_.load(std::memory_order_acquire)) callback();
            });
        }
        /// @brief Request the fake driver to stop and join its worker before returning.
        void stop() {
            running_.store(false,std::memory_order_release);
            stopRequested.store(true,std::memory_order_release);
            if (worker_.joinable()) worker_.join();
        }
        /// @brief Atomic indication that the control path requested driver shutdown.
        std::atomic<bool> stopRequested{false};
    private:
        /// @brief Atomic worker-loop admission flag used by the fake backend.
        std::atomic<bool> running_{false};
        /// @brief Owned driver thread joined before backend teardown returns.
        std::thread worker_;
    };
}
