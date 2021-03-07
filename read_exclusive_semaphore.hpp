#pragma once

#include <atomic>
#include "utils.h"
#include "event.h"

struct read_write_semaphore {
    using atomic_count_t = std::atomic_unsigned_lock_free;
    static_assert(atomic_count_t::is_always_lock_free);
    using atomic_count_value_t = typename atomic_count_t::value_type;

    inline auto exclusive_lock_free() noexcept(false) { return lock_free_exclusive_token(*this); }

    inline auto write() { return exclusive_token(*this); }

    inline auto read() { return read_token(*this); }

private:
    event writer_done_event;
    event readers_done_event;
    atomic_count_t reading = std::numeric_limits<atomic_count_value_t>::min();
    /**
     * set by exclusive accessor
     */
    static const atomic_count_value_t max_value = std::numeric_limits<atomic_count_value_t>::max();
    /**
     * Limit for number of concurrent readers
     */
    static const atomic_count_value_t max_count = max_value - 1;

    class max_readers_exception : public std::exception {
    };

    bool reader_try_acquire() {
        auto expected = reading.load();
        return expected != max_count && reading.compare_exchange_weak(expected, expected + 1);
    }

    bool reader_try_release() {
        auto expected = reading.load();
        bool freed = reading.compare_exchange_weak(expected, expected - 1);
        if (freed and expected == 1) readers_done_event.notify();
        return freed;
    }

    inline bool writer_try_acquire() {
        atomic_count_value_t expected = 0;
        return reading.compare_exchange_weak(expected, max_count);
    }

    /**
     * CAN STARVE WRITER - USE WITH CAUTION
     * @deprecated not really deprecated - just use with caution
     */
    void exclusive_acquire() noexcept(false) {
        readers_done_event.try_or_wait([this] { return writer_try_acquire(); });
    }

    void reader_acquire() noexcept(false) {
        writer_done_event.try_or_wait([this] {
            try {
                writer_done_event.try_or_wait([this] {
                    auto expected = reading.load();
                    do {
                        if (expected == max_value) return false;
                        if (expected == max_count) throw max_readers_exception();
                    } while (!reading.compare_exchange_weak(expected, expected + 1));
                    return true;
                });
            } catch (const max_readers_exception &) {
                return false;
            }
            return true;
        });
    }

    void reader_release() noexcept(false) {
        if (--reading == 0) readers_done_event.notify();
    }

    void exclusive_release() {
        reading = 0;
        writer_done_event.notify();
    }

    struct read_token {
        read_write_semaphore &sem;

    public:
        explicit read_token(read_write_semaphore &sem) : sem(sem) {
            sem.reader_acquire();
        }

        ~read_token() {
            sem.reader_release();
        }
    };

    class exclusive_token {
        read_write_semaphore &sem;

    public:
        explicit exclusive_token(read_write_semaphore &sem) : sem(sem) {
            sem.exclusive_acquire();
        }

        ~exclusive_token() {
            sem.exclusive_release();
        }
    };

    class lock_free_exclusive_token {
        read_write_semaphore &sem;

    public:
        explicit lock_free_exclusive_token(read_write_semaphore &sem) noexcept(false): sem(sem) {
            if (!sem.writer_try_acquire())
                throw acquisition_error("failed to obtain write access to semaphore");
        }

        ~lock_free_exclusive_token() {
            sem.exclusive_release();
        }
    };
}
