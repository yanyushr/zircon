// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <kernel/fair_scheduler.h>

#include <assert.h>
#include <debug.h>
#include <err.h>
#include <inttypes.h>
#include <kernel/lockdep.h>
#include <kernel/mp.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/thread_lock.h>
#include <lib/ktrace.h>
#include <list.h>
#include <platform.h>
#include <printf.h>
#include <string.h>
#include <target.h>
#include <trace.h>
#include <vm/vm.h>
#include <zircon/types.h>

#include <new>

using ffl::Expression;
using ffl::FromInteger;
using ffl::Max;
using ffl::Round;
using ffl::ToPrecision;

// ktraces just local to this file
#define LOCAL_KTRACE 1

#if LOCAL_KTRACE
#define LOCAL_KTRACE0(probe) ktrace_probe0(probe, true)
#define LOCAL_KTRACE2(probe, x, y) ktrace_probe2(probe, x, y, true)
#define LOCAL_KTRACE64(probe, x) ktrace_probe64(probe, x)
#else
#define LOCAL_KTRACE0(probe)
#define LOCAL_KTRACE2(probe, x, y)
#define LOCAL_KTRACE64(probe, x)
#endif

#define LOCAL_TRACE 0

#define SCHED_TRACEF(str, args...) LTRACEF("[%d] " str, arch_curr_cpu_num(), ##args)

namespace {

// On ARM64 with safe-stack, it's no longer possible to use the unsafe-sp
// after set_current_thread (we'd now see newthread's unsafe-sp instead!).
// Hence this function and everything it calls between this point and the
// the low-level context switch must be marked with __NO_SAFESTACK.
__NO_SAFESTACK static void FinalContextSwitch(thread_t* oldthread,
                                              thread_t* newthread) {
    set_current_thread(newthread);
    arch_context_switch(oldthread, newthread);
}

inline bool IsEarlier(const thread_t& a, const thread_t& b) {
    return a.fair_task_state < b.fair_task_state;
}

} // anonymous namespace

TaskWeight FairScheduler::GetTotalWeight() {
    Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
    return weight_total_;
}

size_t FairScheduler::GetRunnableTasks() {
    Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
    return static_cast<size_t>(runnable_task_count_);
}

FairScheduler* FairScheduler::Get() {
    return Get(arch_curr_cpu_num());
}

FairScheduler* FairScheduler::Get(cpu_num_t cpu) {
    return &percpu[cpu].fair_runqueue;
}

void FairScheduler::InitializeThread(thread_t* thread, TaskWeight weight) {
    new (&thread->fair_task_state) FairTaskState{weight};
}

// Returns the next thread to execute. The result is one of:
//  1. |current_thread| if it is ready and is still the earliest.
//  2. The next thread in the run queue if it is non-empty.
//  3. |idle_thread| otherwise.
thread_t* FairScheduler::NextThread(thread_t* current_thread, thread_t* idle_thread, bool timeslice_expired) {
    const bool is_idle = current_thread == idle_thread;
    const bool is_empty = run_queue_.is_empty();
    const bool is_earliest = !is_idle && (is_empty || (!timeslice_expired && IsEarlier(*current_thread, run_queue_.front())));

    const char* front_name = !is_empty ? run_queue_.front().name : "[none]";
    SCHED_TRACEF("current=%s state=%d is_earliest=%d is_empty=%d front=%s\n",
                 current_thread->name, current_thread->state, is_earliest, is_empty, front_name);

    const SchedDuration front_virtual_finish_time_grans =
        !is_empty ? run_queue_.front().fair_task_state.virtual_finish_time_grans_
                  : SchedDuration{FromInteger(0)};
    LOCAL_KTRACE2("next_thread",
                  Round<uint32_t>(current_thread->fair_task_state.virtual_finish_time_grans_),
                  Round<uint32_t>(front_virtual_finish_time_grans));

    if (current_thread->state == THREAD_READY && is_earliest) {
        return current_thread;
    } else if (!is_empty) {
        return run_queue_.pop_front();
    } else {
        return idle_thread;
    }
}

cpu_num_t FairScheduler::FindTargetCpu(thread_t* thread) {
    const cpu_mask_t current_cpu_mask = cpu_num_to_mask(arch_curr_cpu_num());
    const cpu_mask_t last_cpu_mask = cpu_num_to_mask(thread->last_cpu);
    const cpu_mask_t affinity_mask = thread->cpu_affinity;
    const cpu_mask_t active_mask = mp_get_active_mask();
    const cpu_mask_t idle_mask = mp_get_idle_mask();

    // Threads may be created and resumed before the thread init level. Work around
    // an empty active mask by assuming the current cpu is scheduleable.
    cpu_mask_t available_mask = active_mask != 0 ? affinity_mask & active_mask
                                                 : current_cpu_mask;
    DEBUG_ASSERT_MSG(available_mask != 0,
                     "thread=%s affinity=%x active=%x idle=%x arch_ints_disabled=%d",
                     thread->name, affinity_mask, active_mask, idle_mask, arch_ints_disabled());

    LOCAL_KTRACE2("find_target", mp_get_online_mask(), active_mask);

    cpu_num_t target_cpu;
    cpu_num_t least_loaded_cpu;
    FairScheduler* target_queue;
    FairScheduler* least_loaded_queue;

    // Select an initial target.
    if (last_cpu_mask & available_mask) {
        target_cpu = thread->last_cpu;
    } else if (current_cpu_mask & available_mask) {
        target_cpu = arch_curr_cpu_num();
    } else {
        target_cpu = lowest_cpu_set(available_mask);
    }

    target_queue = Get(target_cpu);
    least_loaded_cpu = target_cpu;
    least_loaded_queue = target_queue;

    // See if there is a better target in the set of available CPUs.
    // TODO(eieio): Replace this with a search in order of increasing cache
    // distance when topology information is available.
    while (available_mask != 0) {
#if 0
        if (target_queue->utilization_grans_ == 0) {
            SCHED_TRACEF("thread=%s target_cpu=%d\n", thread->name, target_cpu);
            LOCAL_KTRACE2("target_cpu", least_loaded_cpu, available_mask);
            return target_cpu;
        }
#endif

        if (target_queue->weight_total_ < least_loaded_queue->weight_total_) {
            least_loaded_cpu = target_cpu;
            least_loaded_queue = target_queue;
        }

        available_mask &= ~cpu_num_to_mask(target_cpu);
        if (available_mask) {
            target_cpu = lowest_cpu_set(available_mask);
            target_queue = Get(target_cpu);
        }
    }

    SCHED_TRACEF("thread=%s target_cpu=%d\n", thread->name, least_loaded_cpu);
    LOCAL_KTRACE2("target_cpu", least_loaded_cpu, available_mask);
    return least_loaded_cpu;
}

void FairScheduler::UpdateTimeline(SchedTime now) {
    const Expression runtime_ns = now - last_update_time_ns_;
    last_update_time_ns_ = now;

    if (weight_total_ > 0) {
        virtual_time_ns_ += runtime_ns / weight_total_;
    } else {
        virtual_time_ns_ += runtime_ns;
    }

    LOCAL_KTRACE2("update_global_timeline", Round<uint32_t>(runtime_ns),
                  Round<uint32_t>(virtual_time_ns_ / 1000));
}

void FairScheduler::RescheduleCommon(SchedTime now) {
    const cpu_num_t current_cpu = arch_curr_cpu_num();
    thread_t* const current_thread = get_current_thread();

    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    DEBUG_ASSERT_MSG(current_thread->state != THREAD_RUNNING, "state %d\n", current_thread->state);
    DEBUG_ASSERT(!arch_blocking_disallowed());

    CPU_STATS_INC(reschedules);

    UpdateTimeline(now);

    const SchedDuration actual_runtime_ns = now - last_reschedule_time_ns_;
    last_reschedule_time_ns_ = now;

    // Update the accounting for the thread that just ran.
    FairTaskState* const current_state = &current_thread->fair_task_state;
    current_state->runtime_ns_ += actual_runtime_ns;
    const bool timeslice_expired = now >= absolute_deadline_ns_;

    // Select a thread to run.
    thread_t* const idle_thread = &percpu[current_cpu].idle_thread;
    thread_t* const next_thread = NextThread(current_thread, idle_thread, timeslice_expired);
    DEBUG_ASSERT(next_thread != nullptr);

    SCHED_TRACEF("current=%s next=%s is_empty=%d\n",
                 current_thread->name, next_thread->name, run_queue_.is_empty());

    // Update the state of the current and next thread. The order of operations
    // is important because the current and next threads might be the same.
    next_thread->state = THREAD_RUNNING;
    current_thread->preempt_pending = false;

    if (current_thread->state != THREAD_READY) {
        current_thread->curr_cpu = INVALID_CPU;
    }
    next_thread->last_cpu = current_cpu;
    next_thread->curr_cpu = current_cpu;

    if (current_thread->state == THREAD_READY) {
        // Re-queue the previous thread to run again later.
        NextThreadTimeslice(current_thread);
        UpdateThreadTimeline(current_thread);
        QueueThread(current_thread);
    } else if (current_thread->state != THREAD_RUNNING) {
        // The previous thread is not re-queued and is not about to run again.
        // Remove its accounting because it is blocked/suspended/dead.
        Remove(current_thread);
    }

    if (next_thread != current_thread || timeslice_expired) {
        // Re-compute the timeslice for the new thread based on the latest state.
        NextThreadTimeslice(next_thread);
    }

    // Always call to handle races between reschedule IPIs and changes to the run queue.
    mp_prepare_current_cpu_idle_state(thread_is_idle(next_thread));

    if (thread_is_idle(next_thread)) {
        mp_set_cpu_idle(current_cpu);
    } else {
        mp_set_cpu_busy(current_cpu);
    }

    // The task is always non-realtime when managed by this scheduler.
    // TODO(eieio): Revisit this when deadline scheduling is addressed.
    mp_set_cpu_non_realtime(current_cpu);

    if (thread_is_idle(current_thread)) {
        percpu[current_cpu].stats.idle_time += actual_runtime_ns.raw_value();
    }

    if (thread_is_idle(next_thread) /*|| runnable_task_count_ == 1*/) {
        SCHED_TRACEF("Stop preemption timer: current=%s next=%s\n",
                     current_thread->name, next_thread->name);
        timer_preempt_cancel();
    } else {
        FairTaskState* const next_state = &next_thread->fair_task_state;
        DEBUG_ASSERT(next_state->time_slice_ns_ > 0);

        // Update the preemption time based on the time slice.
        absolute_deadline_ns_ = now + next_state->time_slice_ns_;

        LOCAL_KTRACE2("start_preemption: now,deadline", Round<uint32_t>(now / 1000),
                      Round<uint32_t>(absolute_deadline_ns_ / 1000));

        SCHED_TRACEF("Start preemption timer: current=%s next=%s now=%ld deadline=%ld\n",
                     current_thread->name, next_thread->name, now.raw_value(),
                     absolute_deadline_ns_.raw_value());
        timer_preempt_reset(absolute_deadline_ns_.raw_value());
    }

    if (next_thread != current_thread) {
        LOCAL_KTRACE2("reschedule: count,curr_slice",
                      static_cast<uint32_t>(runnable_task_count_),
                      Round<uint32_t>(current_thread->fair_task_state.time_slice_ns_));
        LOCAL_KTRACE2("reschedule: weight_total,next_slice",
                      static_cast<uint32_t>(weight_total_.raw_value()),
                      Round<uint32_t>(next_thread->fair_task_state.time_slice_ns_));

        ktrace(TAG_CONTEXT_SWITCH,
               static_cast<uint32_t>(next_thread->user_tid),
               current_cpu | (current_thread->state << 8) |
                   (current_thread->base_priority << 16) |
                   (next_thread->base_priority << 24),
               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(current_thread)),
               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(next_thread)));

        // Blink the optional debug LEDs on the target.
        target_set_debug_led(0, !thread_is_idle(next_thread));

        SCHED_TRACEF("current=(%s, flags 0x%x) next=(%s, flags 0x%x)\n",
                     current_thread->name, current_thread->flags,
                     next_thread->name, next_thread->flags);

        if (current_thread->aspace != next_thread->aspace) {
            vmm_context_switch(current_thread->aspace, next_thread->aspace);
        }

        CPU_STATS_INC(context_switches);
        FinalContextSwitch(current_thread, next_thread);
    }

#if LOCAL_TRACE
    printf("Dumping threads:\n");
    dump_all_threads_locked(false);
#endif
}

void FairScheduler::UpdatePeriod() {
    DEBUG_ASSERT(runnable_task_count_ >= 0);
    DEBUG_ASSERT(minimum_granularity_ns_ > 0);
    DEBUG_ASSERT(peak_latency_ns_ > 0);
    DEBUG_ASSERT(target_latency_ns_ > 0);

    const int64_t num_tasks = runnable_task_count_;
    const int64_t peak_tasks = Round<int64_t>(peak_latency_ns_ / minimum_granularity_ns_);
    const int64_t normal_tasks = Round<int64_t>(target_latency_ns_ / minimum_granularity_ns_);

    // The scheduling period stretches when there are too many tasks to fit
    // within the target latency.
    scheduling_period_grans_ = FromInteger(num_tasks > normal_tasks ? num_tasks : normal_tasks);
    utilization_grans_ = num_tasks > peak_tasks ? num_tasks : 0;

    LOCAL_KTRACE2("update_period",
                  Round<uint32_t>(scheduling_period_grans_),
                  static_cast<uint32_t>(num_tasks));

    SCHED_TRACEF("num_tasks=%ld peak_tasks=%ld normal_tasks=%ld period_grans=%ld utilization=%ld\n",
                 num_tasks, peak_tasks, normal_tasks, scheduling_period_grans_.raw_value(),
                 utilization_grans_);
}

void FairScheduler::NextThreadTimeslice(thread_t* thread) {
    if (thread_is_idle(thread) || thread->state == THREAD_DEATH) {
        return;
    }

    FairTaskState* const state = &thread->fair_task_state;

    // Calculate the relative portion of the scheduling period in units of
    // minimum granularity.
    ffl::Fixed<zx_duration_t, 5> time_slice_grans =
        (scheduling_period_grans_ * state->effective_weight()) / weight_total_;

    state->time_slice_grans_ = FromInteger(time_slice_grans.Ceiling());
    state->time_slice_ns_ = state->time_slice_grans_ * minimum_granularity_ns_;

    DEBUG_ASSERT(state->time_slice_grans_ > 0);

    LOCAL_KTRACE2("next_timeslice: slice,weight",
                  Round<uint32_t>(state->time_slice_grans_),
                  state->effective_weight().raw_value());

    SCHED_TRACEF("name=%s weight_total=%x weight=%x time_slice_grans=%ld\n",
                 thread->name,
                 weight_total_.raw_value(),
                 state->effective_weight().raw_value(),
                 state->time_slice_grans_.raw_value());
}

void FairScheduler::UpdateThreadTimeline(thread_t* thread) {
    if (thread_is_idle(thread) || thread->state == THREAD_DEATH) {
        return;
    }

    FairTaskState* const state = &thread->fair_task_state;

    // Update virtual timeline.
    const SchedTime virtual_time_grans = virtual_time_ns_ / minimum_granularity_ns_;

    state->virtual_start_time_grans_ = Max(state->virtual_finish_time_grans_, virtual_time_grans);
    state->virtual_finish_time_grans_ = state->virtual_start_time_grans_ +
                                        state->time_slice_grans_ / state->effective_weight();

    LOCAL_KTRACE2("update_timeline: vs,vf",
                  Round<uint32_t>(state->virtual_start_time_grans_),
                  Round<uint32_t>(state->virtual_finish_time_grans_));

    SCHED_TRACEF("name=%s vstart=%ld vfinish=%ld lag=%ld vtime=%ld\n",
                 thread->name,
                 state->virtual_start_time_grans_.raw_value(),
                 state->virtual_finish_time_grans_.raw_value(),
                 state->lag_time_ns_.raw_value(),
                 virtual_time_ns_.raw_value());
}

void FairScheduler::QueueThread(thread_t* thread) {
    DEBUG_ASSERT(thread->state == THREAD_READY);
    if (!thread_is_idle(thread)) {
        thread->fair_task_state.generation_ = ++generation_count_;
        run_queue_.insert(thread);
        LOCAL_KTRACE0("queue_thread");
    }
}

void FairScheduler::Insert(SchedTime now, thread_t* thread) {
    DEBUG_ASSERT(thread->state == THREAD_READY);
    DEBUG_ASSERT(!thread_is_idle(thread));

    FairTaskState* const state = &thread->fair_task_state;

    // Ensure insertion happens only once, even if Unblock is called multiple times.
    if (state->OnInsert()) {
        runnable_task_count_++;
        DEBUG_ASSERT(runnable_task_count_ >= 0);

        UpdatePeriod();
        UpdateTimeline(now);

        thread->curr_cpu = arch_curr_cpu_num();

        // Factor this task into the run queue.
        weight_total_ += state->effective_weight();
        DEBUG_ASSERT(weight_total_ > TaskWeight{FromInteger(0)});
        //virtual_time_ns_ -= state->lag_time_ns_ / weight_total_;

        NextThreadTimeslice(thread);
        UpdateThreadTimeline(thread);
        QueueThread(thread);
    }
}

void FairScheduler::Remove(thread_t* thread) {
    DEBUG_ASSERT(!thread_is_idle(thread));

    FairTaskState* const state = &thread->fair_task_state;
    DEBUG_ASSERT(!state->InQueue());

    // Ensure that removal happens only once, even if Block() is called multiple times.
    if (state->OnRemove()) {
        runnable_task_count_--;
        DEBUG_ASSERT(runnable_task_count_ >= 0);

        UpdatePeriod();

        // Factor this task out of the run queue.
        //virtual_time_ns_ += state->lag_time_ns_ / weight_total_;
        weight_total_ -= state->effective_weight();
        DEBUG_ASSERT(weight_total_ >= TaskWeight{FromInteger(0)});

        SCHED_TRACEF("name=%s weight_total=%x weight=%x lag_time_ns=%ld\n",
                     thread->name,
                     weight_total_.raw_value(),
                     state->effective_weight().raw_value(),
                     state->lag_time_ns_.raw_value());
    }
}

void FairScheduler::Block() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* const current_thread = get_current_thread();

    DEBUG_ASSERT(current_thread->magic == THREAD_MAGIC);
    DEBUG_ASSERT(current_thread->state != THREAD_RUNNING);

    LOCAL_KTRACE0("sched_block");

    const SchedTime now = CurrentTime();
    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, now.raw_value());

    FairScheduler::Get()->RescheduleCommon(now);
}

bool FairScheduler::Unblock(thread_t* thread) {
    DEBUG_ASSERT(thread->magic == THREAD_MAGIC);
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    LOCAL_KTRACE0("sched_unblock");

    const SchedTime now = CurrentTime();
    SCHED_TRACEF("thread=%s now=%ld\n", thread->name, now.raw_value());

    const cpu_num_t target_cpu = FindTargetCpu(thread);
    FairScheduler* const target = Get(target_cpu);

    thread->state = THREAD_READY;
    target->Insert(now, thread);

    if (target_cpu == arch_curr_cpu_num()) {
        return true;
    } else {
        mp_reschedule(cpu_num_to_mask(target_cpu), 0);
        return false;
    }
}

bool FairScheduler::Unblock(list_node* list) {
    DEBUG_ASSERT(list);
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    const SchedTime now = CurrentTime();

    LOCAL_KTRACE0("sched_unblock_list");

    cpu_mask_t cpus_to_reschedule_mask = 0;
    thread_t* thread;
    while ((thread = list_remove_tail_type(list, thread_t, queue_node)) != nullptr) {
        DEBUG_ASSERT(thread->magic == THREAD_MAGIC);
        DEBUG_ASSERT(!thread_is_idle(thread));

        SCHED_TRACEF("thread=%s now=%ld\n", thread->name, now.raw_value());

        const cpu_num_t target_cpu = FindTargetCpu(thread);
        FairScheduler* const target = Get(target_cpu);

        thread->state = THREAD_READY;
        target->Insert(now, thread);

        cpus_to_reschedule_mask |= cpu_num_to_mask(target_cpu);
    }

    // Issue reschedule IPIs if there are CPUs other than the current CPU in the mask.
    const cpu_mask_t current_cpu_mask = cpu_num_to_mask(arch_curr_cpu_num());
    if ((cpus_to_reschedule_mask & ~current_cpu_mask) != 0) {
        mp_reschedule(cpus_to_reschedule_mask, 0);
    }

    // Return true if the current CPU is in the mask.
    return cpus_to_reschedule_mask & current_cpu_mask;
}

void FairScheduler::UnblockIdle(thread_t* thread) {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    DEBUG_ASSERT(thread_is_idle(thread));
    DEBUG_ASSERT(thread->cpu_affinity && (thread->cpu_affinity & (thread->cpu_affinity - 1)) == 0);

    SCHED_TRACEF("thread=%s now=%ld\n", thread->name, current_time());

    thread->state = THREAD_READY;
    thread->curr_cpu = lowest_cpu_set(thread->cpu_affinity);
}

void FairScheduler::Yield() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* current_thread = get_current_thread();
    DEBUG_ASSERT(!thread_is_idle(current_thread));

    LOCAL_KTRACE0("sched_yield");

    const SchedTime now = CurrentTime();
    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, now.raw_value());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon(now);
}

void FairScheduler::Preempt() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* current_thread = get_current_thread();
    const cpu_num_t current_cpu = arch_curr_cpu_num();

    DEBUG_ASSERT(current_thread->curr_cpu == current_cpu);
    DEBUG_ASSERT(current_thread->last_cpu == current_thread->curr_cpu);

    LOCAL_KTRACE0("sched_preempt");

    const SchedTime now = CurrentTime();
    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, now.raw_value());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon(now);
}

void FairScheduler::Reschedule() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* current_thread = get_current_thread();
    const cpu_num_t current_cpu = arch_curr_cpu_num();

    if (current_thread->disable_counts != 0) {
        current_thread->preempt_pending = true;
        return;
    }

    DEBUG_ASSERT(current_thread->curr_cpu == current_cpu);
    DEBUG_ASSERT(current_thread->last_cpu == current_thread->curr_cpu);
    LOCAL_KTRACE0("sched_reschedule");

    const SchedTime now = CurrentTime();
    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, now.raw_value());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon(now);
}

void FairScheduler::RescheduleInternal() {
    Get()->RescheduleCommon(CurrentTime());
}

void FairScheduler::TimerTick(SchedTime now) {
    SchedTime absolute_deadline_ns;

    {
        FairScheduler* const queue = Get();
        Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
        absolute_deadline_ns = queue->absolute_deadline_ns_;
    }

    SCHED_TRACEF("now=%ld deadline=%ld\n", now.raw_value(), absolute_deadline_ns.raw_value());

    LOCAL_KTRACE2("sched_timer_tick", Round<uint32_t>(now / 1000),
                  Round<uint32_t>(absolute_deadline_ns / 1000));

    thread_preempt_set_pending();
}

// Temporary compatibility with the thread layer.

void sched_init_thread(thread_t* thread, int priority) {
    FairScheduler::InitializeThread(thread, ffl::FromRatio(priority, NUM_PRIORITIES));
    thread->base_priority = priority;
}

void sched_block() {
    FairScheduler::Block();
}

bool sched_unblock(thread_t* thread) {
    return FairScheduler::Unblock(thread);
}

bool sched_unblock_list(list_node* list) {
    return FairScheduler::Unblock(list);
}

void sched_unblock_idle(thread_t* thread) {
    FairScheduler::UnblockIdle(thread);
}

void sched_yield() {
    FairScheduler::Yield();
}

void sched_preempt() {
    FairScheduler::Preempt();
}

void sched_reschedule() {
    FairScheduler::Reschedule();
}

void sched_resched_internal() {
    FairScheduler::RescheduleInternal();
}

void sched_transition_off_cpu(cpu_num_t old_cpu) {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    DEBUG_ASSERT(old_cpu == arch_curr_cpu_num());

    (void)old_cpu;
}

void sched_migrate(thread_t* t) {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    (void)t;
}

void sched_inherit_priority(thread_t* t, int pri, bool* local_resched) {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    (void)t;
    (void)pri;
    (void)local_resched;
}

void sched_change_priority(thread_t* t, int pri) {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    (void)t;
    (void)pri;
}

void sched_preempt_timer_tick(zx_time_t now) {
    FairScheduler::TimerTick(ffl::FromInteger(now));
}

void sched_init_early() {
}
