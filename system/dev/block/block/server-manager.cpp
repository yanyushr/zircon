// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>

#include <utility>

#include <ddk/debug.h>

#include "server-manager.h"

ServerManager::ServerManager() = default;

ServerManager::~ServerManager() {
    CloseFifoServer();
}

zx_status_t ServerManager::StartServer(ddk::BlockProtocolClient* protocol, zx::fifo* out_fifo) {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    if (server_running_) {
        return ZX_ERR_ALREADY_BOUND;
    }
    ZX_DEBUG_ASSERT(server_ == nullptr);

    fbl::unique_ptr<BlockServer> server;
    fzl::fifo<block_fifo_request_t, block_fifo_response_t> fifo;
    zx_status_t status = BlockServer::Create(protocol, &fifo, &server);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    ioqueue::Queue* qptr = new (&ac) ioqueue::Queue(server->GetOps());
    if (!ac.check()) {
        printf("Failed to allocate queue\n");
        return ZX_ERR_NO_MEMORY;
    }
    fbl::unique_ptr<ioqueue::Queue> queue(qptr);
    status = queue->OpenStream(0, &stream_id_);
    if (status != ZX_OK) {
        printf("Failed to open stream\n");
        return status;
    }

    server_ = std::move(server);
    queue_ = std::move(queue);
    server_running_ = true;
    status = queue_->Serve(1);
    if (status != ZX_OK) {
        printf("Serve returned failure\n");
        FreeServer();
        return status;
    }
    *out_fifo = zx::fifo(fifo.release());
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    return ZX_OK;
}

zx_status_t ServerManager::CloseFifoServer() {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    if (server_running_ == false) {
        return ZX_OK;
    }
    queue_->Shutdown();
    server_->Shutdown();
    FreeServer();
    return ZX_OK;
}

zx_status_t ServerManager::AttachVmo(zx::vmo vmo, vmoid_t* out_vmoid) {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    if (server_ == nullptr) {
        return ZX_ERR_BAD_STATE;
    }
    return server_->AttachVmo(std::move(vmo), out_vmoid);
}

void ServerManager::FreeServer() {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    server_running_ = false;
    server_.reset();
    queue_.reset();
    printf("%s:%u\n", __FUNCTION__, __LINE__);
}

int ServerManager::RunServer(void* arg) {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    // ServerManager* manager = reinterpret_cast<ServerManager*>(arg);

    // // The completion of "thrd_create" synchronizes-with the beginning of this thread, so
    // // we may assume that "manager->server_" is available for our usage.
    // //
    // // The "manager->server_" pointer shall not be modified by this thread.
    // //
    // // The "manager->server_" pointer will only be nullified after thrd_join, because join
    // // synchronizes-with the completion of this thread.
    // ZX_DEBUG_ASSERT(manager->server_);
    // manager->server_->Serve();
    // manager->SetState(ThreadState::Joinable);
    // printf("%s:%u\n", __FUNCTION__, __LINE__);
    return 0;
}
