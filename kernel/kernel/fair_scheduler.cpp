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
#include <kernel/time.h>
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

// ktraces just local to this file
#define LOCAL_KTRACE 0

#if LOCAL_KTRACE
#define LOCAL_KTRACE0(probe) ktrace_probe0(probe)
#define LOCAL_KTRACE2(probe, x, y) ktrace_probe2(probe, x, y)
#else
#define LOCAL_KTRACE0(probe)
#define LOCAL_KTRACE2(probe, x, y)
#endif

#define LOCAL_TRACE 0

#define SCHED_TRACEF(str, args...) LTRACEF("[%d] " str, arch_curr_cpu_num(), ##args)

namespace {

// On ARM64 with safe-stack, it's no longer possible to use the unsafe-sp
// after set_current_thread (we'd now see newthread's unsafe-sp instead!).
// Hence this function and everything it calls between this point and the
// the low-level context switch must be marked with __NO_SAFESTACK.
__NO_SAFESTACK static void final_context_switch(thread_t* oldthread,
                                                thread_t* newthread) {
    set_current_thread(newthread);
    arch_context_switch(oldthread, newthread);
}

inline bool IsEarlier(const thread_t& a, const thread_t& b) {
    return a.fair_task_state < b.fair_task_state;
}

} // anonymous namespace

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
//  1. |current_thread| if it is ready and still the earliest task.
//  2. The next thread in the run queue if it is non-empty.
//  3. |idle_thread| otherwise.
thread_t* FairScheduler::NextThread(thread_t* current_thread, thread_t* idle_thread) {
    const bool is_idle = current_thread == idle_thread;
    const bool is_empty = run_queue_.is_empty();
    const bool is_earliest = !is_idle &&
                             (is_empty || IsEarlier(*current_thread, run_queue_.front()));

    const char* front_name = !is_empty ? run_queue_.front().name : "[none]";
    SCHED_TRACEF("current=%s state=%d is_earliest=%d is_empty=%d front=%s\n",
                 current_thread->name, current_thread->state, is_earliest, is_empty, front_name);

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
    // distance topology information is available.
    while (available_mask != 0) {
        if (target_queue->utilization_grans_ == 0) {
            SCHED_TRACEF("thread=%s target_cpu=%d\n", thread->name, target_cpu);
            return target_cpu;
        }

        if (target_queue->utilization_grans_ < least_loaded_queue->utilization_grans_) {
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
    return least_loaded_cpu;
}

void FairScheduler::RescheduleCommon() {
    const cpu_num_t current_cpu = arch_curr_cpu_num();
    thread_t* const current_thread = get_current_thread();

    DEBUG_ASSERT(arch_ints_disabled());
    DEBUG_ASSERT(spin_lock_held(&thread_lock));
    DEBUG_ASSERT_MSG(current_thread->state != THREAD_RUNNING, "state %d\n", current_thread->state);
    DEBUG_ASSERT(!arch_blocking_disallowed());

    CPU_STATS_INC(reschedules);

    // Select a thread to run.
    thread_t* const idle_thread = &percpu[current_cpu].idle_thread;
    thread_t* const next_thread = NextThread(current_thread, idle_thread);
    DEBUG_ASSERT(next_thread != nullptr);

    SCHED_TRACEF("current=%s next=%s is_empty=%d\n",
                 current_thread->name, next_thread->name, run_queue_.is_empty());

    // Update the state of the current and next thread before doing accounting.
    // The order of operations is important because the current and next threads
    // might be the same.
    next_thread->state = THREAD_RUNNING;
    current_thread->preempt_pending = false;

    if (current_thread->state != THREAD_READY) {
        current_thread->curr_cpu = INVALID_CPU;
    }
    next_thread->last_cpu = current_cpu;
    next_thread->curr_cpu = current_cpu;

    // Always call to handle races between reschedule IPIs and changes to the run queue.
    mp_prepare_current_cpu_idle_state(thread_is_idle(next_thread));

    if (thread_is_idle(next_thread)) {
        mp_set_cpu_idle(current_cpu);
    }

    // The task is always non-realtime when managed by this scheduler.
    // TODO(eieio): Revisit this when deadline scheduling is addressed.
    mp_set_cpu_non_realtime(current_cpu);

    UpdateAccounting(current_thread, next_thread);

    if (thread_is_idle(current_thread)) {
        const zx::duration last_idle_time{percpu[current_cpu].stats.idle_time};
        const zx::duration idle_time = last_idle_time + last_runtime_ns_;
        percpu[current_cpu].stats.idle_time = idle_time.get();
    }

    LOCAL_KTRACE2("CS timeslice old",
                  static_cast<uint32_t>(current_thread->user_tid),
                  static_cast<uint32_t>(current_thread->fair_task_state.time_slice_ns_.get()));
    LOCAL_KTRACE2("CS timeslice new",
                  static_cast<uint32_t>(next_thread->user_tid),
                  static_cast<uint32_t>(next_thread->fair_task_state.time_slice_ns_.get()));

    ktrace(TAG_CONTEXT_SWITCH,
           static_cast<uint32_t>(next_thread->user_tid),
           current_cpu | (current_thread->state << 8),
           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(current_thread)),
           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(next_thread)));

    if (thread_is_idle(next_thread) || runnable_task_count_ == 1) {
        SCHED_TRACEF("Stop preemption timer: current=%s next=%s\n",
                     current_thread->name, next_thread->name);
        timer_preempt_cancel();
    } else {
        DEBUG_ASSERT(next_thread->fair_task_state.time_slice_ns_ >= minimum_granularity_ns_);

        // Update the preemption time based on the time slice.
        zx::time now{current_time()};
        absolute_deadline_ns_ = now + next_thread->fair_task_state.time_slice_ns_;

        SCHED_TRACEF("Start preemption timer: current=%s next=%s now=%ld deadline=%ld\n",
                     current_thread->name, next_thread->name, now.get(),
                     absolute_deadline_ns_.get());
        timer_preempt_reset(absolute_deadline_ns_.get());
    }

    // Blink the optional debug LEDs on the target.
    target_set_debug_led(0, !thread_is_idle(next_thread));

    SCHED_TRACEF("current=(%s, flags 0x%x) next=(%s, flags 0x%x)\n",
                 current_thread->name, current_thread->flags,
                 next_thread->name, next_thread->flags);

    if (current_thread->aspace != next_thread->aspace) {
        vmm_context_switch(current_thread->aspace, next_thread->aspace);
    }

    if (next_thread != current_thread) {
        CPU_STATS_INC(context_switches);
        final_context_switch(current_thread, next_thread);
    }

#if LOCAL_TRACE
    printf("Dumping threads:\n");
    dump_all_threads_locked(false);
#endif
}

void FairScheduler::UpdatePeriod() {
    DEBUG_ASSERT(runnable_task_count_ >= 0);
    DEBUG_ASSERT(minimum_granularity_ns_.get() > 0);
    DEBUG_ASSERT(peak_latency_ns_.get() > 0);
    DEBUG_ASSERT(target_latency_ns_.get() > 0);

    const int64_t num_tasks = runnable_task_count_;
    const int64_t peak_tasks = peak_latency_ns_ / minimum_granularity_ns_;
    const int64_t normal_tasks = target_latency_ns_ / minimum_granularity_ns_;

    // The scheduling period stretches when there are too many tasks to fit
    // within the target latency.
    scheduling_period_grans_ = num_tasks > normal_tasks ? num_tasks : normal_tasks;
    utilization_grans_ = num_tasks > peak_tasks ? num_tasks : 0;

    SCHED_TRACEF("num_tasks=%ld peak_tasks=%ld normal_tasks=%ld period_grans=%ld utilization=%ld\n",
                 num_tasks, peak_tasks, normal_tasks, scheduling_period_grans_, utilization_grans_);
}

void FairScheduler::Insert(thread_t* thread) {
    DEBUG_ASSERT(thread->state == THREAD_READY);
    FairTaskState* const state = &thread->fair_task_state;

    // Ensure insertion happens only once, even if Resume is called multiple times.
    if (!state->active()) {
        state->OnInsert();

        runnable_task_count_++;
        DEBUG_ASSERT_MSG(runnable_task_count_ >= 0, "runnable_task_count_=%ld", runnable_task_count_);
        UpdatePeriod();

        thread->curr_cpu = arch_curr_cpu_num();
        mp_set_cpu_busy(thread->curr_cpu);

        // Factor this task into the run queue.
        weight_total_ += state->effective_weight();
        DEBUG_ASSERT(weight_total_ > 0);

        const auto lag_ns = state->lag_time_ns_.get() / weight_total_;
        virtual_time_ns_ -= zx::duration{ffl::Round<zx_duration_t>(lag_ns)};

        const auto relative_rate = weight_total_ / state->effective_weight();
        const auto time_slice_grans = scheduling_period_grans_ / relative_rate;

        // Calculate the time slice and make sure it is big enough.
        const auto time_slice = minimum_granularity_ns_.get() * time_slice_grans;
        const int64_t time_slice_ns = ffl::Round<int64_t>(time_slice);
        const bool round_up = time_slice_ns < minimum_granularity_ns_.get();
        state->time_slice_ns_ = round_up ? minimum_granularity_ns_ : zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s weight_total=%x weight=%x relative_rate=%x time_slice_ns=%ld\n",
                     thread->name,
                     weight_total_.raw_value(),
                     state->effective_weight().raw_value(),
                     TaskWeight{relative_rate}.raw_value(),
                     time_slice_ns);

        state->virtual_start_time_ns_ = virtual_time_ns_ - state->lag_time_ns_;
        state->virtual_finish_time_ns_ = state->virtual_start_time_ns_ + zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s vstart=%ld vfinish=%ld vtime=%ld\n",
                     thread->name,
                     state->virtual_start_time_ns_.get(),
                     state->virtual_finish_time_ns_.get(),
                     virtual_time_ns_.get());

        DEBUG_ASSERT(!thread_is_idle(thread));
        run_queue_.insert(thread);
    }
}

void FairScheduler::Remove(thread_t* thread) {
    FairTaskState* const state = &thread->fair_task_state;
    DEBUG_ASSERT(!state->InQueue());

    // Ensure that removal happens only once, even if Block() is called multiple times.
    if (state->active()) {
        state->OnRemove();

        runnable_task_count_--;
        DEBUG_ASSERT_MSG(runnable_task_count_ >= 0, "runnable_task_count_=%ld", runnable_task_count_);
        UpdatePeriod();

        const zx::time now{current_time()};
        const zx::duration actual_runtime_ns = now - last_reschedule_time_ns_;

        // Update the accounting for the scheduler timeline.
        last_reschedule_time_ns_ = now;
        last_runtime_ns_ = actual_runtime_ns;
        virtual_time_ns_ = virtual_time_ns_ + actual_runtime_ns;

        // Update the accounting for the thread that just ran.
        state->runtime_ns_ += actual_runtime_ns;
        state->lag_time_ns_ = state->time_slice_ns_ - actual_runtime_ns;

        // Factor this task out of the run queue.
        const auto lag_ns = state->lag_time_ns_.get() / weight_total_;
        virtual_time_ns_ -= zx::duration{ffl::Round<zx_duration_t>(lag_ns)};

        weight_total_ -= state->effective_weight();
        DEBUG_ASSERT(weight_total_ >= 0);

        SCHED_TRACEF("name=%s weight_total=%x weight=%x lag_time_ns=%ld\n",
                     thread->name,
                     weight_total_.raw_value(),
                     state->effective_weight().raw_value(),
                     state->lag_time_ns_.get());
    }
}

// Updates the accounting for the run queue and the previous and next threads.
void FairScheduler::UpdateAccounting(thread_t* current_thread, thread_t* next_thread) {
    DEBUG_ASSERT(next_thread->state == THREAD_RUNNING);

    FairTaskState* const current_state = &current_thread->fair_task_state;
    FairTaskState* const next_state = &next_thread->fair_task_state;

    SCHED_TRACEF("current=%s next=%s\n", current_thread->name, next_thread->name);

    const zx::time now{current_time()};
    const zx::duration actual_runtime_ns = now - last_reschedule_time_ns_;

    // Update the accounting for the scheduler timeline.
    last_reschedule_time_ns_ = now;
    last_runtime_ns_ = actual_runtime_ns;
    virtual_time_ns_ = virtual_time_ns_ + actual_runtime_ns;

    // Update the accounting for the thread that just ran.
    current_state->runtime_ns_ += actual_runtime_ns;
    current_state->lag_time_ns_ = current_state->time_slice_ns_ - actual_runtime_ns;

    if (!thread_is_idle(next_thread)) {
        // Update the accounting for the thread that is about to run.
        const auto relative_rate = weight_total_ / next_state->effective_weight();
        const auto time_slice_grans = scheduling_period_grans_ / relative_rate;

        // Calculate the time slice and make sure it is big enough.
        const auto time_slice = minimum_granularity_ns_.get() * time_slice_grans;
        const int64_t time_slice_ns = ffl::Round<zx_duration_t>(time_slice);
        const bool round_up = time_slice_ns < minimum_granularity_ns_.get();
        next_state->time_slice_ns_ = round_up ? minimum_granularity_ns_ : zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s weight_total=%x weight=%x relative_rate=%x time_slice_ns=%ld grans=%ld lag=%ld\n",
                     next_thread->name,
                     weight_total_.raw_value(),
                     next_state->effective_weight().raw_value(),
                     TaskWeight{relative_rate}.raw_value(),
                     time_slice_ns,
                     ffl::Round<zx_duration_t>(time_slice_grans),
                     next_state->lag_time_ns_.get());

        next_state->virtual_start_time_ns_ = virtual_time_ns_ - next_state->lag_time_ns_;
        next_state->virtual_finish_time_ns_ = next_state->virtual_start_time_ns_ +
                                              zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s vstart=%ld vfinish=%ld vtime=%ld\n",
                     next_thread->name,
                     next_state->virtual_start_time_ns_.get(),
                     next_state->virtual_finish_time_ns_.get(),
                     virtual_time_ns_.get());
    }

    if (next_thread != current_thread && !thread_is_idle(current_thread) && current_thread->state == THREAD_READY) {
        const auto relative_rate = weight_total_ / current_state->effective_weight();
        const auto time_slice_grans = scheduling_period_grans_ / relative_rate;

        // Calculate the time slice and make sure it is big enough.
        const auto time_slice = minimum_granularity_ns_.get() * time_slice_grans;
        const int64_t time_slice_ns = ffl::Round<int64_t>(time_slice);
        const bool round_up = time_slice_ns < minimum_granularity_ns_.get();
        current_state->time_slice_ns_ = round_up ? minimum_granularity_ns_ : zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s weight_total=%x weight=%x relative_rate=%x time_slice_ns=%ld lag=%ld\n",
                     current_thread->name,
                     weight_total_.raw_value(),
                     current_state->effective_weight().raw_value(),
                     TaskWeight{relative_rate}.raw_value(),
                     time_slice_ns,
                     current_state->lag_time_ns_.get());

        current_state->virtual_start_time_ns_ = virtual_time_ns_ - current_state->lag_time_ns_;
        current_state->virtual_finish_time_ns_ = current_state->virtual_start_time_ns_ +
                                                 zx::duration{time_slice_ns};

        SCHED_TRACEF("name=%s vstart=%ld vfinish=%ld vtime=%ld\n",
                     current_thread->name,
                     current_state->virtual_start_time_ns_.get(),
                     current_state->virtual_finish_time_ns_.get(),
                     virtual_time_ns_.get());

        DEBUG_ASSERT(!thread_is_idle(current_thread));
        run_queue_.insert(current_thread);
    }
}

void FairScheduler::Block() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* const current_thread = get_current_thread();

    DEBUG_ASSERT(current_thread->magic == THREAD_MAGIC);
    DEBUG_ASSERT(current_thread->state != THREAD_RUNNING);

    LOCAL_KTRACE0("sched_block");

    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, current_time());

    FairScheduler::Get()->Remove(current_thread);
    FairScheduler::Get()->RescheduleCommon();
}

bool FairScheduler::Unblock(thread_t* thread) {
    DEBUG_ASSERT(thread->magic == THREAD_MAGIC);
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    LOCAL_KTRACE0("sched_unblock");

    SCHED_TRACEF("thread=%s now=%ld\n", thread->name, current_time());

    const cpu_num_t target_cpu = FindTargetCpu(thread);
    FairScheduler* const target = Get(target_cpu);

    thread->state = THREAD_READY;
    target->Insert(thread);

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

    LOCAL_KTRACE0("sched_unblock_list");

    cpu_mask_t cpus_to_reschedule_mask = 0;
    thread_t* thread;
    while ((thread = list_remove_tail_type(list, thread_t, queue_node)) != nullptr) {
        DEBUG_ASSERT(thread->magic == THREAD_MAGIC);
        DEBUG_ASSERT(!thread_is_idle(thread));

        SCHED_TRACEF("thread=%s now=%ld\n", thread->name, current_time());

        const cpu_num_t target_cpu = FindTargetCpu(thread);
        FairScheduler* const target = Get(target_cpu);

        thread->state = THREAD_READY;
        target->Insert(thread);

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

    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, current_time());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon();
}

void FairScheduler::Preempt() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* current_thread = get_current_thread();
    uint curr_cpu = arch_curr_cpu_num();

    DEBUG_ASSERT(current_thread->curr_cpu == curr_cpu);
    DEBUG_ASSERT(current_thread->last_cpu == current_thread->curr_cpu);
    LOCAL_KTRACE0("sched_preempt");

    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, current_time());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon();
}

void FairScheduler::Reschedule() {
    DEBUG_ASSERT(spin_lock_held(&thread_lock));

    thread_t* current_thread = get_current_thread();
    uint curr_cpu = arch_curr_cpu_num();

    if (current_thread->disable_counts != 0) {
        current_thread->preempt_pending = true;
        return;
    }

    DEBUG_ASSERT(current_thread->curr_cpu == curr_cpu);
    DEBUG_ASSERT(current_thread->last_cpu == current_thread->curr_cpu);
    LOCAL_KTRACE0("sched_reschedule");

    SCHED_TRACEF("current=%s now=%ld\n", current_thread->name, current_time());

    current_thread->state = THREAD_READY;
    Get()->RescheduleCommon();
}

void FairScheduler::RescheduleInternal() {
    Get()->RescheduleCommon();
}

void FairScheduler::TimerTick(zx::time now) {
    thread_t* const current_thread = get_current_thread();
    if (unlikely(thread_is_real_time_or_idle(current_thread))) {
        return;
    }

    LOCAL_KTRACE2("sched_preempt_timer_tick",
                  static_cast<uint32_t>(current_thread->user_tid),
                  static_cast<uint32_t>(current_thread->remaining_time_slice));

    {
        Guard<spin_lock_t, IrqSave> guard{ThreadLock::Get()};
        FairScheduler* const queue = FairScheduler::Get(current_thread->curr_cpu);
        SCHED_TRACEF("now=%ld deadline=%ld\n", now.get(), queue->absolute_deadline_ns().get());
    }

    thread_preempt_set_pending();
}

// Temporary compatibility with the thread layer.

void sched_init_thread(thread_t* thread, int priority) {
    FairScheduler::InitializeThread(thread, ffl::FromRatio(priority + 1, NUM_PRIORITIES));
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
    FairScheduler::TimerTick(zx::time{now});
}

void sched_init_early() {
}
