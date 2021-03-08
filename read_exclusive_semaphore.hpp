//
// Created by tobia on 21/02/2021.
//
#pragma once

#include <atomic>
#include "utils.h"
#include "event.h"

static_assert(std::atomic_bool::is_always_lock_free);

template<typename T>
constexpr T make_read_count_mask(uint8_t n) {
    return ((((T) 1U) << (sizeof(T) * CHAR_BIT - n)) - 1U);
}

template<typename T>
constexpr T make_writer_using_bit_flag(uint8_t n) {
    return ~make_read_count_mask<T>(n);
}


class read_exclusive_semaphore {
    using read_count_t = std::atomic_unsigned_lock_free;
    static_assert(read_count_t::is_always_lock_free);
    using read_count_value_t = typename read_count_t::value_type;

    event writer_done_event;
    event reader_done_event;

    read_count_t read_count = 0;
    static const size_t read_count_value_size = sizeof(read_count) * CHAR_BIT;
/**
 * TODO: CTAD on make_*bits
 *       `Clang-Tidy: Initialization of 'max_readers' with static storage duration may throw an exception that cannot be caught`
 *           despite being constexpr
 */
    static const read_count_value_t max_readers = make_read_count_mask<read_count_value_t>(1);
    static const read_count_value_t writer_using_bit_flag = make_writer_using_bit_flag<read_count_value_t>(1);
//    read_count_value_t another_bit_flag = 1 >> (read_count_value_size - 2);

    bool try_read_acquire() {
        // expected to not have writer_using flag set
        auto expected = read_count.load() & max_readers;

        // if we hit the max_readers limit, fail
        if (expected == max_readers) return false;

        return read_count.compare_exchange_weak(expected, expected + 1);
    }

    inline bool try_exclusive_acquire() {
        read_count_value_t expected_read_count_no_readers_or_writer_using = 0;
        const auto read_count_no_readers_writer_using = writer_using_bit_flag;

        return read_count.compare_exchange_weak(expected_read_count_no_readers_or_writer_using,
                                                read_count_no_readers_writer_using);
    }

    void exclusive_acquire() noexcept(false) {
        writer_done_event.try_or_wait([&] {
            // assert writer_using flag is not set
            auto expected_read_count_writer_using = read_count.load() & ~writer_using_bit_flag;
            // set writer_using flag to true, if (!writer_using) assertion still holds, or fail
            if (!read_count.compare_exchange_weak(expected_read_count_writer_using,
                                                  expected_read_count_writer_using | writer_using_bit_flag)) {
                // signifies wait on writer
                return false;
            }
            // wait till reader_count hits 0
            reader_done_event.try_or_wait([read_count = &this->read_count] { return read_count == 0; });

            // signifies done waiting on writer
            return true;
        });
    }

    void reader_acquire() noexcept(false) {
        reader_done_event.try_or_wait([this] {
            try {
                writer_done_event.try_or_wait([this] {
                    auto expected = read_count.load();
                    do {
                        // checks if writer_using flag is set, or fails
                        if ((expected & writer_using_bit_flag) != 0U) {
                            // signifies wait on writer
                            return false;
                        }
                        // checks if reader_count is maxed_out - if so, jumps to reader_done_event wait
                        if ((expected & max_readers) == max_readers) throw max_readers_exception();
                    } while (!read_count.compare_exchange_weak(expected, expected + 1));

                    // signifies done waiting on writer
                    return true;
                });
            } catch (const max_readers_exception &) {
                // signifies wait on reader
                return false;
            }

            // signifies done waiting on reader
            return true;
        });
    }

    void reader_release() noexcept(false) {
        // no CAS required, as we aren't conditionally mutating
        // this could internally lock/block/spin
        --read_count;

        reader_done_event.notify();
    }

    void exclusive_release() {
        // This could internally lock/block/spin
        // No locking is actually required as this has exclusive access
        read_count &= ~writer_using_bit_flag;

        writer_done_event.notify();
    }


    class max_readers_exception : public std::exception {
    };

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
     * locks only on acquire
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
