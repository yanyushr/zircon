// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <atomic>

#include <ddktl/protocol/block.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>
#include <zircon/types.h>

#include "io-queue.h"
#include "server.h"

// ServerManager controls the state of a background thread (or threads) servicing Fifo
// requests.
class ServerManager {
public:
    DISALLOW_COPY_ASSIGN_AND_MOVE(ServerManager);

    ServerManager();
    ~ServerManager();

    // Launches the Fifo server in a background thread.
    //
    // Returns an error if the block server cannot be created.
    // Returns an error if the Fifo server is already running.
    zx_status_t StartServer(ddk::BlockProtocolClient* protocol, zx::fifo* out_fifo);

    // Ensures the FIFO server has terminated.
    //
    // When this function returns, it is guaranteed that the next call to |StartServer()|
    // won't see an already running Fifo server.
    zx_status_t CloseFifoServer();

    // Attaches a VMO to the currently executing server, if one is running.
    //
    // Returns an error if a server is not currently running.
    zx_status_t AttachVmo(zx::vmo vmo, vmoid_t* out_vmoid);

    inline void AsyncCompleteOp(ioqueue::io_op_t* op, zx_status_t result) {
        queue_->AsyncCompleteOp(op, result);
    }

private:
    // Frees the Fifo server, cleaning up "server_" and setting the thread state to none.
    //
    // Precondition: No background thread is executing.
    void FreeServer();

    // Runs the server until |blockserver_shutdown| is invoked on |server_|, or the client
    // closes their end of the Fifo.
    static int RunServer(void* arg);

    // thrd_t thread_;
    bool server_running_ = false;
    fbl::unique_ptr<BlockServer> server_ = nullptr;
    fbl::unique_ptr<ioqueue::Queue> queue_ = nullptr;
    uint32_t stream_id_;
};
