// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zircon/types.h>

#include <fbl/intrusive_wavl_tree.h>
#include <ffl/fixed.h>

#include <utility>

// Forward declaration.
typedef struct thread thread_t;

// Fixed-point task weight / priority with 19bit integral and 12bit fractional
// parts. This split permits the run queue to accurately track ~500k tasks with
// a weight of 1.0.
using TaskWeight = ffl::Fixed<int32_t, 12>;

using SchedDuration = ffl::Fixed<zx_duration_t, 0>;
using SchedTime = ffl::Fixed<zx_time_t, 0>;

template <typename T>
constexpr auto SchedNanoseconds(T nanooseconds) {
    return ffl::FromInteger(ZX_NSEC(nanooseconds));
}
template <typename T>
constexpr auto SchedMicroseconds(T microseconds) {
    return ffl::FromInteger(ZX_USEC(microseconds));
}
template <typename T>
constexpr auto SchedMilliseconds(T milliseconds) {
    return ffl::FromInteger(ZX_MSEC(milliseconds));
}

class FairTaskState {
public:
    using KeyType = std::pair<SchedTime, uintptr_t>;

    static constexpr TaskWeight kDefaultWeight = ffl::FromInteger(1);

    FairTaskState() = default;
    explicit FairTaskState(TaskWeight weight)
        : base_weight_{weight} {}

    FairTaskState(const FairTaskState&) = delete;
    FairTaskState& operator=(const FairTaskState&) = delete;

    // TODO(eieio): Implement inheritance.
    TaskWeight base_weight() const { return base_weight_; }
    TaskWeight effective_weight() const { return base_weight_; }

    // Returns the key used to order the run queue.
    KeyType key() const { return {virtual_finish_time_ns_, reinterpret_cast<uintptr_t>(this)}; }

    bool operator<(const FairTaskState& other) const {
        return key() < other.key();
    }

    // Returns true of the task state is currently enqueued in the runnable tree.
    bool InQueue() const {
        return run_queue_node_.InContainer();
    }

    bool active() const { return active_; }

    // Sets the task state to active (on a run queue). Returns true if the task
    // was not previously active.
    bool OnInsert() {
        const bool was_active = active_;
        active_ = true;
        return !was_active;
    }

    // Sets the task state to inactive (not on a run queue). Returns true if the
    // task was previously active.
    bool OnRemove() {
        const bool was_active = active_;
        active_ = false;
        return was_active;
    }

private:
    friend class FairScheduler;

    fbl::WAVLTreeNodeState<thread_t*> run_queue_node_;

    TaskWeight base_weight_{kDefaultWeight};

    // TODO(eieio): Some of the values below are only relevant when running,
    // while others only while ready. Consider using a union to save space.
    SchedTime virtual_start_time_ns_{SchedNanoseconds(0)};
    SchedTime virtual_finish_time_ns_{SchedNanoseconds(0)};

    SchedDuration time_slice_ns_{SchedNanoseconds(0)};
    SchedDuration lag_time_ns_{SchedNanoseconds(0)};

    SchedDuration runtime_ns_{SchedNanoseconds(0)};

    bool active_{false};
};
