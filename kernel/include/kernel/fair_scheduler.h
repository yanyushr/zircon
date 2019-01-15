// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <stdint.h>
#include <zircon/types.h>

#include <kernel/fair_task_state.h>
#include <kernel/thread.h>

#include <platform.h>

#include <fbl/intrusive_pointer_traits.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/type_support.h>
#include <ffl/fixed.h>

class FairScheduler {
public:
    // Default minimum granularity of time slices.
    static constexpr SchedDuration kDefaultMinimumGranularity = SchedMicroseconds(750);

    // Default target latency for a scheduling period.
    static constexpr SchedDuration kDefaultTargetLatency = SchedMilliseconds(6);

    // Default peak latency for a scheduling period.
    static constexpr SchedDuration kDefaultPeakLatency = SchedMilliseconds(10);

    FairScheduler() = default;
    ~FairScheduler() = default;

    FairScheduler(const FairScheduler&) = delete;
    FairScheduler& operator=(const FairScheduler&) = delete;

    static void InitializeThread(thread_t* thread, TaskWeight weight);
    static void Block() TA_REQ(thread_lock);
    static void Yield() TA_REQ(thread_lock);
    static void Preempt() TA_REQ(thread_lock);
    static void Reschedule() TA_REQ(thread_lock);
    static void RescheduleInternal() TA_REQ(thread_lock);

    static bool Unblock(thread_t* thread) __WARN_UNUSED_RESULT TA_REQ(thread_lock);
    static bool Unblock(list_node* thread_list) __WARN_UNUSED_RESULT TA_REQ(thread_lock);
    static void UnblockIdle(thread_t* idle_thread) TA_REQ(thread_lock);

    static void TimerTick(SchedTime now);

    TaskWeight GetTotalWeight();
    size_t GetRunnableTasks();

private:
    // Returns the current system time as a SchedTime value.
    static SchedTime CurrentTime() {
        return ffl::FromInteger(current_time());
    }

    // Returns the FairScheduler instance for the current CPU.
    static FairScheduler* Get();

    // Returns the FairScheduler instance for the given CPU.
    static FairScheduler* Get(cpu_num_t cpu);

    static cpu_num_t FindTargetCpu(thread_t* thread) TA_REQ(thread_lock);

    void RescheduleCommon(SchedTime now) TA_REQ(thread_lock);

    // Returns the next thread to execute.
    thread_t* NextThread(thread_t* current_thread, thread_t* idle_thread, bool timeslice_expired) TA_REQ(thread_lock);

    void QueueThread(thread_t* thread) TA_REQ(thread_lock);

    void UpdateActiveThread(thread_t* thread, SchedDuration actual_runtime_ns) TA_REQ(thread_lock);
    void NextThreadTimeslice(thread_t* thread) TA_REQ(thread_lock);
    void UpdateThreadTimeline(thread_t* thread) TA_REQ(thread_lock);

    void UpdatePeriod() TA_REQ(thread_lock);
    void UpdateTimeline(SchedTime now) TA_REQ(thread_lock);

    void Insert(SchedTime now, thread_t* thread) TA_REQ(thread_lock);
    void Remove(thread_t* thread) TA_REQ(thread_lock);

    struct TaskTraits {
        using KeyType = FairTaskState::KeyType;
        static KeyType GetKey(const thread_t& thread) { return thread.fair_task_state.key(); }
        static bool LessThan(KeyType a, KeyType b) { return a < b; }
        static bool EqualTo(KeyType a, KeyType b) { return a == b; }
        static auto& node_state(thread_t& thread) { return thread.fair_task_state.run_queue_node_; }
    };

    using RunQueue = fbl::WAVLTree<zx_time_t, thread_t*, TaskTraits, TaskTraits>;
    RunQueue TA_GUARDED(thread_lock) run_queue_;

    // Monotonically increasing counter to break ties when queuing tasks with
    // the same virtual finish time. This has the effect of placing newly
    // queued tasks behind already queued tasks with the same virtual finish
    // time. This is also necessary to guarantee uniqueness of the key as
    // required by the WAVLTree container.
    uint64_t TA_GUARDED(thread_lock) generation_count_{0};

    // Count of the threads running on this CPU, including threads in the run
    // queue and the currently running thread. Does not include the idle thread.
    int32_t TA_GUARDED(thread_lock) runnable_task_count_{0};

    // Total weights of threads running on this CPU, including threads in the
    // run queue and the currently running thread. Does not include the idle
    // thread.
    TaskWeight TA_GUARDED(thread_lock) weight_total_{ffl::FromInteger(0)};

    SchedTime TA_GUARDED(thread_lock) virtual_time_ns_{SchedNanoseconds(0)};
    SchedTime TA_GUARDED(thread_lock) last_update_time_ns_{SchedNanoseconds(0)};
    SchedTime TA_GUARDED(thread_lock) absolute_deadline_ns_{SchedNanoseconds(0)};
    SchedTime TA_GUARDED(thread_lock) last_reschedule_time_ns_{SchedNanoseconds(0)};

    // Scheduling period in which every runnable task executes once, in units of
    // minimum granularity (grans).
    SchedDuration TA_GUARDED(thread_lock) scheduling_period_grans_{ffl::FromInteger(0)};

    SchedDuration TA_GUARDED(thread_lock) minimum_granularity_ns_{kDefaultMinimumGranularity};
    SchedDuration TA_GUARDED(thread_lock) target_latency_ns_{kDefaultTargetLatency};
    SchedDuration TA_GUARDED(thread_lock) peak_latency_ns_{kDefaultPeakLatency};

    int64_t TA_GUARDED(thread_lock) utilization_grans_{0};
};
