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

    SchedTime absolute_deadline_ns() const { return absolute_deadline_ns_; }

private:
    static SchedTime CurrentTime() {
        return ffl::FromInteger(current_time());
    }

    // Returns the FairScheduler instance for the current CPU.
    static FairScheduler* Get() TA_REQ(thread_lock);

    // Returns the FairScheduler instance for the given CPU.
    static FairScheduler* Get(cpu_num_t cpu) TA_REQ(thread_lock);

    static cpu_num_t FindTargetCpu(thread_t* thread) TA_REQ(thread_lock);

    void RescheduleCommon(SchedTime now) TA_REQ(thread_lock);

    // Returns the next thread to execute.
    thread_t* NextThread(thread_t* current_thread, thread_t* idle_thread,
                         bool timeslice_expired) TA_REQ(thread_lock);

    void QueueThread(thread_t* thread) TA_REQ(thread_lock);

    void UpdateThread(thread_t* thread) TA_REQ(thread_lock);
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
    RunQueue run_queue_;

    int32_t runnable_task_count_{0};

    SchedTime virtual_time_ns_{SchedNanoseconds(0)};
    SchedTime last_update_time_ns_{SchedNanoseconds(0)};
    //SchedTime rebalancing_time_ns_{SchedNanoseconds(0)};
    SchedTime absolute_deadline_ns_{SchedNanoseconds(0)};

    SchedTime last_reschedule_time_ns_{SchedNanoseconds(0)};

    // Scheduling period in which every runnable task executes once, in units of
    // minimum granularity (grans).
    int64_t scheduling_period_grans_{0};

    SchedDuration minimum_granularity_ns_{kDefaultMinimumGranularity};
    SchedDuration target_latency_ns_{kDefaultTargetLatency};
    SchedDuration peak_latency_ns_{kDefaultPeakLatency};

    int64_t utilization_grans_{0};

    TaskWeight weight_total_{ffl::FromInteger(0)};
};
