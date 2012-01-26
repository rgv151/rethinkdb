#ifndef __CONCURRENCY_QUEUE_CROSS_THREAD_LIMITED_FIFO_HPP__
#define __CONCURRENCY_QUEUE_CROSS_THREAD_LIMITED_FIFO_HPP__

#include <list>

#include "concurrency/queue/passive_producer.hpp"
#include "concurrency/semaphore.hpp"
#include "concurrency/drain_semaphore.hpp"
#include "do_on_thread.hpp"

/* `cross_thread_limited_fifo_t` is like `limited_fifo_t`, except that it is
efficient even when objects are being pushed onto the queue from a thread other
than the home thread. In the constructor, pass an extra parameter for the
thread that you intend to push objects onto the queue from. Pushing objects
onto the queue from that thread will be very efficient. */

template <class value_t, class queue_t = std::list<value_t> >
struct cross_thread_limited_fifo_t :
    public passive_producer_t<value_t>,
    public home_thread_mixin_t
{
    cross_thread_limited_fifo_t(int st, int capacity, float trickle_fraction = 0.0) :
        passive_producer_t<value_t>(&available_control),
        source_thread(st),
        semaphore(capacity, trickle_fraction),
        in_destructor(false)
    {
        assert_good_thread_id(source_thread);
        drain_semaphore.rethread(source_thread);
    }

    ~cross_thread_limited_fifo_t() {
        // Set `in_destructor` to `true` so nothing gets pushed onto the queue
        // or popped off of it; that way we will release the drain semaphore
        // the correct number of times
        in_destructor = true;
        int number_times_to_release_drain_semaphore = queue.size();
        {
            on_thread_t thread_switcher(source_thread);
            // The drain semaphore was acquired once for each thing pushed onto the
            // queue, and released once for each thing popped off the queue. The
            // difference, which we compensate for here, is the number of objects
            // that were on the queue at the time that the destructor was called.
            for (int i = 0; i < number_times_to_release_drain_semaphore; i++) drain_semaphore.release();
            drain_semaphore.drain();
        }
        drain_semaphore.rethread(home_thread());
    }

    struct do_pusher_t {
	cross_thread_limited_fifo_t *parent_;
	value_t value_;
	do_pusher_t(cross_thread_limited_fifo_t *parent, const value_t& value)
	    : parent_(parent), value_(value) { }

	void operator()() const {
	    parent_->do_push(value_);
	}
    };

    void push(const value_t& value) {
        rassert(get_thread_id() == source_thread);
        drain_semaphore.acquire();
        semaphore.co_lock();

        do_on_thread(home_thread(), do_pusher_t(this, value));
    }

    void set_capacity(int capacity) {
        on_thread_t thread_switcher(source_thread);
        semaphore.set_capacity(capacity);
    }

private:
    int source_thread;
    adjustable_semaphore_t semaphore;
    drain_semaphore_t drain_semaphore;
    bool in_destructor;
    queue_t queue;
    availability_control_t available_control;

    void do_push(const value_t &value) {
        assert_thread();
        rassert(!in_destructor);
        queue.push_back(value);
        available_control.set_available(!queue.empty());
    }

    void do_done() {
        rassert(get_thread_id() == source_thread);
        semaphore.unlock();
        drain_semaphore.release();
    }

    struct do_done_caller_t {
	cross_thread_limited_fifo_t *parent_;
	explicit do_done_caller_t(cross_thread_limited_fifo_t *parent) : parent_(parent) { }
	void operator()() const { parent_->do_done(); }
    };

    value_t produce_next_value() {
        assert_thread();
        rassert(!in_destructor);
        value_t v = queue.front();
        queue.pop_front();
        do_on_thread(source_thread, do_done_caller_t(this));
        available_control.set_available(!queue.empty());
        return v;
    }

    DISABLE_COPYING(cross_thread_limited_fifo_t);
};

#endif /* __CONCURRENCY_QUEUE_CROSS_THREAD_LIMITED_FIFO_HPP__ */

