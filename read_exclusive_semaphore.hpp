//
// Created by tobia on 21/02/2021.
//
#pragma once

#include <atomic>
#include "utils.h"
#include "event.h"

class read_exclusive_semaphore {
    using read_count_t = std::atomic_unsigned_lock_free;
    static_assert(read_count_t::is_always_lock_free);
    using read_count_value_t = typename read_count_t::value_type;
    event writer_done_event;
    event reader_done_event;
    event all_readers_done_event;
    read_count_t read_count = std::numeric_limits<read_count_value_t>::min();
    /**
     * set by exclusive accessor
     */
    static const read_count_value_t max_value = std::numeric_limits<read_count_value_t>::max();
    /**
     * Limit for number of concurrent readers
     */
    static const read_count_value_t max_count = max_value - 1;

    class max_readers_exception : public std::exception {
    };

    bool try_read_acquire() {
        auto expected = read_count.load();
        return expected != max_count && read_count.compare_exchange_weak(expected, expected + 1);
    }

    inline bool try_exclusive_acquire() {
        read_count_value_t expected = 0;
        return read_count.compare_exchange_weak(expected, max_count);
    }

    /**
     * Could lead to exclusive accessor being starved
     */
    void exclusive_acquire() noexcept(false) {
        all_readers_done_event.try_or_wait([this] { return try_exclusive_acquire(); });
    }

    void reader_acquire() noexcept(false) {
        reader_done_event.try_or_wait([this] {
            try {
                writer_done_event.try_or_wait([this] {
                    auto expected = read_count.load();
                    do {
                        if (expected == max_value) return false;
                        if (expected == max_count) throw max_readers_exception();
                    } while (!read_count.compare_exchange_weak(expected, expected + 1));
                    return true;
                });
            } catch (const max_readers_exception &) {
                return false;
            }
            return true;
        });
    }

    void reader_release() noexcept(false) {
        if (--read_count == 0) all_readers_done_event.notify();
        reader_done_event.notify();
    }

    void exclusive_release() {
        read_count = 0;
        writer_done_event.notify();
    }

    struct read_token {
        read_exclusive_semaphore &sem;

    public:
        explicit read_token(read_exclusive_semaphore &sem) : sem(sem) {
            sem.reader_acquire();
        }

        ~read_token() {
            sem.reader_release();
        }
    };

    class exclusive_token {
        read_exclusive_semaphore &sem;
    protected:

        explicit exclusive_token(read_exclusive_semaphore &sem) : sem(sem) {
        }

        ~exclusive_token() {
            sem.exclusive_release();
        }
    };

    /**
     * locks on acquire
     */
    struct locking_exclusive_token : public exclusive_token {
        explicit locking_exclusive_token(read_exclusive_semaphore &sem) : exclusive_token(sem) {
            sem.exclusive_acquire();
        }
    };

    /**
     * throws if fails to lock-free acquire
     */
    class lock_free_exclusive_token : public exclusive_token {
    public:
        explicit lock_free_exclusive_token(read_exclusive_semaphore &sem) : exclusive_token(sem) {
            if (!sem.try_exclusive_acquire())
                throw acquisition_error("failed to obtain exclusive_locking access to semaphore");
        }
    };

public:
    inline auto exclusive_lock_free() noexcept(false) { return lock_free_exclusive_token(*this); }

    inline auto exclusive_locking() { return locking_exclusive_token(*this); }

    inline auto read_locking() { return read_token(*this); }
};
