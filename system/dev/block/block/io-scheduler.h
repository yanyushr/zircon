// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <fbl/condition_variable.h>
#include <semaphore.h>
#include <zircon/types.h>

#include "io-op.h"
#include "io-stream.h"

#define IO_SCHED_DEFAULT_PRI       16
#define IO_SCHED_NUM_PRI           32
#define IO_SCHED_MAX_PRI           (IO_SCHED_NUM_PRI - 1)

#define SCHED_OPS_HIWAT 20
#define SCHED_OPS_LOWAT 5
#define SCHED_MAX_ISSUES 2

namespace ioqueue {

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void AddStream(StreamRef stream, uint32_t* id_out);
    StreamRef FindStream(uint32_t id);
    void RemoveStream(StreamRef stream);
    void RemoveStreamLocked(StreamRef stream);

    zx_status_t InsertOps(io_op_t** op_list, uint32_t op_count, uint32_t* out_num_ready);
    zx_status_t GetNextOp(bool wait, io_op_t** op_out);
    void CompleteOp(io_op_t* op, zx_status_t result);

    void CloseAll();
    void WaitUntilDrained();

    uint32_t NumReadyOps();

private:
    using StreamIdMap = Stream::WAVLTreeSortById;
    using StreamPriList = Stream::ListSortByPriority;

    StreamRef FindStreamLocked(uint32_t id);

    fbl::Mutex lock_;
//    cnd_t event_available_;          // New ops are available.
    fbl::ConditionVariable event_drained_;   // All ops have been consumed.
    sem_t issue_sem_;                // Number of concurrent issues.
    uint32_t num_streams_ = 0;       // Number of streams in the priority list.
    uint32_t num_ready_ops_ = 0;     // Number of ops waiting to be issued.
    uint32_t num_issued_ops_ = 0;    // Number of issued ops.
    uint32_t max_issues_;            // Maximum number of concurrent issues.
    uint32_t max_id_ = 0;

    StreamIdMap stream_map_;         // Map of id to stream.
    StreamPriList pri_list_[IO_SCHED_NUM_PRI];
};

} // namespace
