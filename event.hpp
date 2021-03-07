//
// Created by tobia on 22/02/2021.
//
#pragma once

#include "utils.h"
#include <condition_variable>


/**
 * must be used either by the same thread, or one that is auto-joined, e.g. `std::jthread`
 */
class event {
    using nonce_t = uint64_t;

// ############### START OF STATE - ONLY ACCESS WITH LOCK ON event::m
    /**
     * used to protect against spurious wakeups
     */
    nonce_t nonce = 0;
    bool _shutdown = false;
// ############### END OF STATE

    std::mutex m;
    std::condition_variable cv;

    class wait_token {
        std::unique_lock<std::mutex> lock;
        event &e;

    public:
        explicit wait_token(event &e) noexcept(false): lock(e.m), e(e) {
            e.assert_not_shutdown();
        }

        void wait() noexcept(false) {
            // preemptively check if shutdown before performing wait
            e.assert_not_shutdown();

            const auto last_nonce = e.nonce;
            while (last_nonce == e.nonce) {
                e.cv.wait(lock);

                // check if this wake-up was actually a shutdown event
                e.assert_not_shutdown();
            }
        }
    };

    void assert_not_shutdown() const noexcept(false) {
        if (_shutdown) {
            throw shutdown_t();
        }
    }

public:
    struct shutdown_t : public std::runtime_error {
        shutdown_t() : std::runtime_error("Failed to acquire - context has shutdown") {}
    };

    void notify() {
        {
//            TODO: is it necessary to lock here? or can we utilise std::atomic<nonce_t>::is_always_lock_free
            std::scoped_lock lock{m};
            // invalidate state
            nonce++;
        }
        cv.notify_all();
    }

    /**
     * use try_or_wait in any scenario like the following:
     * while (!pred()) event.wait();
     *
     * For proper atomicity
     */
    void wait() noexcept(false) {
        wait_token{*this}.wait();
    }

    template<class TryFunction>
    requires std::is_invocable_r_v<bool, TryFunction>
    void try_or_wait(TryFunction try_function) noexcept(false) {
        wait_token wt{*this};
        while (!try_function()) {
            wt.wait();
        }
    }

    /**
     * idempotent
     */
    void shutdown() noexcept {
        {
            // atomically shutdown
            std::scoped_lock lock{m};
            _shutdown = true;
        }

        // wakeup waiters
        cv.notify_all();
    }

    ~event() noexcept {
        shutdown();
    }
};
