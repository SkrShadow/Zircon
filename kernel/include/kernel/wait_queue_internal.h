// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_WAIT_QUEUE_INTERNAL_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_WAIT_QUEUE_INTERNAL_H_

#include <lib/ktrace.h>
#include <platform.h>
#include <zircon/errors.h>

#include <kernel/scheduler.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

namespace internal {

void wait_queue_timeout_handler(Timer* timer, zx_time_t now, void* arg);

// Used by WaitQueue and OwnedWaitQueue to manage changes to the maximum
// priority of a wait queue due to external effects (thread priority change,
// thread timeout, thread killed).  Do not call this function from an external
// site.
bool wait_queue_waiters_priority_changed(WaitQueue* wq, int old_prio) TA_REQ(thread_lock);

// Notes for wait_queue_block_etc_(pre|post).
//
// Currently, there are two variants of wait_queues in Zircon.  The standard
// WaitQueue (used for most tasks) and the specialized
// OwnedWaitQueues (used for mutexes/futexes/brwlocks, and anything else which
// needs to have a concept of priority inheritance).
//
// The "Block" operation for these two versions are _almost_ identical.  The
// only real difference between the two is that the OWQ implementation needs to
// stop after we have decided that we are actually going to block the thread,
// but before the timeout timer is armed and the thread is actually blocked, in
// order to update it's PI chain bookkeeping.
//
// Instead of duplicating the code, or exposing a code-injection mechanism into
// the public API, we split the code into two inline functions that we hide in
// internal:: instead.  The first (pre) performs all of the checks and bookkeeping
// up-to the point of arming the timer and blocking, the second (post) finishes
// the job.
//
// The traditional WaitQueue implementation of
// wait_queue_block_etc just calls these two functions back to back, relying on
// the inlining to generate the original function.  The OwnedWaitQueue
// implementation does the same, but injects its bookkeeping at the appropriate
// point.
//
// Nothing but these two specific pieces of code should *ever* need to call
// these functions.  Users should *always* be using either
// wait_queue_block_etc/wait_queue_block (or the WaitQueue wrappers of the
// same), or OwnedWaitQueue::BlockAndAssignOwner instead.
//
inline zx_status_t wait_queue_block_etc_pre(WaitQueue* wait, const Deadline& deadline,
                                            uint signal_mask, ResourceOwnership reason)
    TA_REQ(thread_lock) {
  Thread* current_thread = Thread::Current::Get();

  if (deadline.when() != ZX_TIME_INFINITE && deadline.when() <= current_time()) {
    return ZX_ERR_TIMED_OUT;
  }

  if (current_thread->interruptable_ && (unlikely(current_thread->signals_ & ~signal_mask))) {
    if (current_thread->signals_ & THREAD_SIGNAL_KILL) {
      return ZX_ERR_INTERNAL_INTR_KILLED;
    } else if (current_thread->signals_ & THREAD_SIGNAL_SUSPEND) {
      return ZX_ERR_INTERNAL_INTR_RETRY;
    }
  }

  wait->collection_.Insert(current_thread);
  current_thread->state_ =
      (reason == ResourceOwnership::Normal) ? THREAD_BLOCKED : THREAD_BLOCKED_READ_LOCK;
  current_thread->blocking_wait_queue_ = wait;
  current_thread->blocked_status_ = ZX_OK;

  return ZX_OK;
}

inline zx_status_t wait_queue_block_etc_post(WaitQueue* wait, const Deadline& deadline)
    TA_REQ(thread_lock) {
  Thread* current_thread = Thread::Current::Get();
  Timer timer;

  // if the deadline is nonzero or noninfinite, set a callback to yank us out of the queue
  if (deadline.when() != ZX_TIME_INFINITE) {
    timer.Set(deadline, wait_queue_timeout_handler, (void*)current_thread);
  }

  ktrace_ptr(TAG_KWAIT_BLOCK, wait, 0, 0);

  Scheduler::Block();

  ktrace_ptr(TAG_KWAIT_UNBLOCK, wait, current_thread->blocked_status_, 0);

  // we don't really know if the timer fired or not, so it's better safe to try to cancel it
  if (deadline.when() != ZX_TIME_INFINITE) {
    timer.Cancel();
  }

  return current_thread->blocked_status_;
}

}  // namespace internal

// This is defined here, rather than in wait.h, as it needs to see Thread internals as well.
template <typename Callable>
void WaitQueueCollection::ForeachThread(const Callable& visit_thread) TA_REQ(thread_lock) {
  auto consider_queue = [&visit_thread](Thread* queue_head) TA_REQ(thread_lock) -> bool {
    // So, this is a bit tricky.  We need to visit each node in a
    // wait_queue priority level in a way which permits our visit_thread
    // function to remove the thread that we are visiting.
    //
    // Each priority level starts with a queue head which has a list of
    // more threads which exist at that priority level, but the queue
    // head itself is not a member of this list, so some special care
    // must be taken.
    //
    // Start with the queue_head and look up the next thread (if any) at
    // the priority level.  Visit the thread, and if (after visiting the
    // thread), the next thread has become the new queue_head, update
    // queue_head and keep going.
    //
    // If we advance past the queue head, but still have threads to
    // consider, switch to a more standard enumeration of the queue
    // attached to the queue_head.  We know at this point in time that
    // the queue_head can no longer change out from under us.
    //
    DEBUG_ASSERT(queue_head != nullptr);
    Thread* next;

    while (true) {
      next = list_peek_head_type(&queue_head->wait_queue_state_.queue_node_, Thread,
                                 wait_queue_state_.queue_node_);

      if (!visit_thread(queue_head)) {
        return false;
      }

      // Have we run out of things to visit?
      if (!next) {
        return true;
      }

      // If next is not the new queue head, stop.
      if (!list_in_list(&next->wait_queue_state_.wait_queue_heads_node_)) {
        break;
      }

      // Next is the new queue head.  Update and keep going.
      queue_head = next;
    }

    // If we made it this far, then we must still have a valid next.
    DEBUG_ASSERT(next);
    do {
      Thread* t = next;
      next =
          list_next_type(&queue_head->wait_queue_state_.queue_node_,
                         &t->wait_queue_state_.queue_node_, Thread, wait_queue_state_.queue_node_);

      if (!visit_thread(t)) {
        return false;
      }
    } while (next != nullptr);

    return true;
  };

  Thread* last_queue_head = nullptr;
  Thread* queue_head;

  list_for_every_entry (&private_heads_, queue_head, Thread,
                        wait_queue_state_.wait_queue_heads_node_) {
    if ((last_queue_head != nullptr) && !consider_queue(last_queue_head)) {
      return;
    }
    last_queue_head = queue_head;
  }

  if (last_queue_head != nullptr) {
    consider_queue(last_queue_head);
  }
}

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_WAIT_QUEUE_INTERNAL_H_
