// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <kernel/time.h>

#include <fbl/intrusive_wavl_tree.h>
#include <ffl/fixed.h>

#include <utility>

// Forward declaration.
typedef struct thread thread_t;

// Fixed-point task weight / priority with 19bit integral and 12bit fractional
// parts. This split permits the run queue to accurately track ~500k tasks with
// a weight of 1.0.
using TaskWeight = ffl::Fixed<int32_t, 12>;

class FairTaskState {
public:
    using KeyType = std::pair<zx::time, uintptr_t>;

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

    bool InQueue() const {
        return run_queue_node_.InContainer();
    }

    int64_t active() const { return active_; }

    void OnInsert() { active_ = true; }
    void OnRemove() { active_ = false; }

private:
    friend class FairScheduler;

    fbl::WAVLTreeNodeState<thread_t*> run_queue_node_;

    TaskWeight base_weight_{kDefaultWeight};

    // TODO(eieio): Some of the values below are only relevant when running,
    // while others only while ready. Consider using a union to save space.
    zx::time virtual_start_time_ns_{0};
    zx::time virtual_finish_time_ns_{0};

    zx::duration time_slice_ns_{0};
    zx::duration lag_time_ns_{0};

    zx::duration runtime_ns_{0};

    bool active_{false};
};
