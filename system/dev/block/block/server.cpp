// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits>
#include <new>
#include <string.h>
#include <unistd.h>
#include <utility>

#include <ddk/device.h>
#include <ddk/protocol/block.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <lib/zx/fifo.h>
#include <zircon/compiler.h>
#include <zircon/device/block.h>
#include <zircon/syscalls.h>

#include "server.h"

namespace {

// This signal is set on the FIFO when the server should be instructed
// to terminate.
constexpr zx_signals_t kSignalFifoTerminate   = ZX_USER_SIGNAL_0;
// This signal is set on the FIFO when, after the thread enqueueing operations
// has encountered a barrier, all prior operations have completed.
constexpr zx_signals_t kSignalFifoOpsComplete = ZX_USER_SIGNAL_1;
// Signalled on the fifo when it has finished terminating.
// (If we need to free up user signals, this could easily be transformed
// into a completion object).
constexpr zx_signals_t kSignalFifoTerminated  = ZX_USER_SIGNAL_2;

// Impossible groupid used internally to signify that an operation
// has no accompanying group.
constexpr groupid_t kNoGroup = MAX_TXN_GROUP_COUNT;

void OutOfBandRespond(const fzl::fifo<block_fifo_response_t, block_fifo_request_t>& fifo,
                      zx_status_t status, reqid_t reqid, groupid_t group) {
    block_fifo_response_t response;
    response.status = status;
    response.reqid = reqid;
    response.group = group;
    response.count = 1;

    status = fifo.write_one(response);
    if (status != ZX_OK) {
        fprintf(stderr, "Block Server I/O error: Could not write response\n");
    }
}

void BlockComplete(BlockMsg* msg, zx_status_t status) {
    auto extra = msg->extra();
    // Since iobuf is a RefPtr, it lives at least as long as the txn,
    // and is not discarded underneath the block device driver.
    extra->iobuf = nullptr;
    extra->server->TxnComplete(status, extra->reqid, extra->group);
    extra->server->TxnEnd();
}

void BlockCompleteCb(void* cookie, zx_status_t status, block_op_t* bop) {
    ZX_DEBUG_ASSERT(bop != nullptr);
    BlockMsg msg(static_cast<block_msg_t*>(cookie));
    BlockComplete(&msg, status);
}

uint32_t OpcodeToCommand(uint32_t opcode) {
    // TODO(ZX-1826): Unify block protocol and block device interface
    static_assert(BLOCK_OP_READ == BLOCKIO_READ, "");
    static_assert(BLOCK_OP_WRITE == BLOCKIO_WRITE, "");
    static_assert(BLOCK_OP_FLUSH == BLOCKIO_FLUSH, "");
    static_assert(BLOCK_FL_BARRIER_BEFORE == BLOCKIO_BARRIER_BEFORE, "");
    static_assert(BLOCK_FL_BARRIER_AFTER == BLOCKIO_BARRIER_AFTER, "");
    const uint32_t shared = BLOCK_OP_READ | BLOCK_OP_WRITE | BLOCK_OP_FLUSH |
            BLOCK_FL_BARRIER_BEFORE | BLOCK_FL_BARRIER_AFTER;
    return opcode & shared;
}

void InQueueAdd(zx_handle_t vmo, uint64_t length, uint64_t vmo_offset,
                uint64_t dev_offset, block_msg_t* msg, BlockMsgQueue* queue) {
    block_op_t* bop = &msg->op;
    bop->rw.length = (uint32_t) length;
    bop->rw.vmo = vmo;
    bop->rw.offset_dev = dev_offset;
    bop->rw.offset_vmo = vmo_offset;
    queue->push_back(msg);
}

}  // namespace

IoBuffer::IoBuffer(zx::vmo vmo, vmoid_t id) : io_vmo_(std::move(vmo)), vmoid_(id) { }

IoBuffer::~IoBuffer() {}

zx_status_t IoBuffer::ValidateVmoHack(uint64_t length, uint64_t vmo_offset) {
    uint64_t vmo_size;
    zx_status_t status;
    if ((status = io_vmo_.get_size(&vmo_size)) != ZX_OK) {
        return status;
    } else if ((vmo_offset > vmo_size) || (vmo_size - vmo_offset < length)) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    return ZX_OK;
}

void BlockServer::BarrierComplete() {
    // This is the only location that unsets the OpsComplete
    // signal. We'll never "miss" a signal, because we process
    // the queue AFTER unsetting it.
    barrier_in_progress_.store(false);
    fifo_.signal(kSignalFifoOpsComplete, 0);
    InQueueDrainer();
}

void BlockServer::TerminateQueue() {
    InQueueDrainer();
    while (true) {
        if (pending_count_.load() == 0 && in_queue_.is_empty()) {
            return;
        }
        zx_signals_t signals = kSignalFifoOpsComplete;
        zx_signals_t seen = 0;
        fifo_.wait_one(signals, zx::deadline_after(zx::msec(10)), &seen);
        if (seen & kSignalFifoOpsComplete) {
            BarrierComplete();
        }
    }
}
void BlockServer::TxnComplete(zx_status_t status, reqid_t reqid, groupid_t group) {
    if (group == kNoGroup) {
        OutOfBandRespond(fifo_, status, reqid, group);
    } else {
        ZX_DEBUG_ASSERT(group < MAX_TXN_GROUP_COUNT);
        groups_[group].Complete(status);
    }
}

zx_status_t BlockServer::Read(block_fifo_request_t* requests, size_t* count) {
    auto cleanup = fbl::MakeAutoCall([this]() {
        TerminateQueue();
        ZX_ASSERT(pending_count_.load() == 0);
        ZX_ASSERT(in_queue_.is_empty());
        fifo_.signal(0, kSignalFifoTerminated);
    });

    // Keep trying to read messages from the fifo until we have a reason to
    // terminate
    zx_status_t status;
    while (true) {
        status = fifo_.read(requests, BLOCK_FIFO_MAX_DEPTH, count);
        zx_signals_t signals;
        zx_signals_t seen;
        switch (status) {
        case ZX_ERR_SHOULD_WAIT:
            signals = ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED |
                    kSignalFifoTerminate | kSignalFifoOpsComplete;
            if ((status = fifo_.wait_one(signals, zx::time::infinite(), &seen)) != ZX_OK) {
                return status;
            }
            if (seen & kSignalFifoOpsComplete) {
                BarrierComplete();
                continue;
            }
            if ((seen & ZX_FIFO_PEER_CLOSED) || (seen & kSignalFifoTerminate)) {
                return ZX_ERR_PEER_CLOSED;
            }
            // Try reading again...
            break;
        case ZX_OK:
            cleanup.cancel();
            return ZX_OK;
        default:
            return status;
        }
    }
}

struct RequestWrapper {
    RequestWrapper(const block_fifo_request_t* req);

    ioqueue::io_op_t op_;
    block_fifo_request_t request_;
};

RequestWrapper::RequestWrapper(const block_fifo_request_t* req) {
    memset(&op_, 0, sizeof(op_));
    memcpy(&request_, req, sizeof(request_));
    // todo: set opcode, sid
}

zx_status_t BlockServer::ReadOps(ioqueue::io_op_t** op_list, uint32_t* op_count, bool wait) {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    uint32_t max_ops = *op_count;
    if (max_ops == 0) {
        printf("%s:%u\n", __FUNCTION__, __LINE__);
        return ZX_ERR_INVALID_ARGS;
    }
    if (max_ops > BLOCK_FIFO_MAX_DEPTH) {
        max_ops = BLOCK_FIFO_MAX_DEPTH;
    }
    block_fifo_request_t requests[BLOCK_FIFO_MAX_DEPTH];
    size_t count;
    zx_status_t status;
    for ( ; ; ) {
        if ((status = fifo_.read(requests, BLOCK_FIFO_MAX_DEPTH, &count)) == ZX_OK) {
            break;
        }
        printf("%s:%u\n", __FUNCTION__, __LINE__);
        if (status != ZX_ERR_SHOULD_WAIT) {
            printf("%s:%u\n", __FUNCTION__, __LINE__);
            printf("read error\n");
            return ZX_ERR_IO;
        }
        zx_signals_t signals = ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED |
                               kSignalFifoTerminate | kSignalFifoOpsComplete;
        zx_signals_t seen;
        status = fifo_.wait_one(signals, zx::time::infinite(), &seen);
        if (status == ZX_ERR_CANCELED) {
            // Fifo closed locally by shutdown routine.
            return ZX_ERR_CANCELED;
        }
        if (status != ZX_OK) {
            return ZX_ERR_IO;
        }
            // if (seen & kSignalFifoOpsComplete) {
            //     BarrierComplete();
            //     continue;
            // }
        if ((seen & ZX_FIFO_PEER_CLOSED) || (seen & kSignalFifoTerminate)) {
            printf("%s:%u\n", __FUNCTION__, __LINE__);
            return ZX_ERR_CANCELED;  // todo: switch to ZX_ERR_PEER_CLOSED
        }
    }
    // todo: make this not suck.
    memset(op_list, 0, sizeof(op_list[0]) * max_ops);
    fbl::AllocChecker ac;
    status = ZX_OK;
    for (size_t i = 0; i < count; i++) {
        RequestWrapper* wrapper = new (&ac) RequestWrapper(&requests[i]);
        if (!ac.check()) {
            status = ZX_ERR_IO;
            printf("%s:%u\n", __FUNCTION__, __LINE__);
            break;
        }
        op_list[i] = &wrapper->op_;
    }
    if (status != ZX_OK) {
        for (size_t i = 0; i < max_ops; i++) {
            if (op_list[i] != nullptr) {
                delete op_list[i];
                op_list[i] = nullptr;
            }
        }
        count = 0;
    }
    *op_count = static_cast<uint32_t>(count);
    return status;
}


zx_status_t BlockServer::FindVmoIDLocked(vmoid_t* out) {
    for (vmoid_t i = last_id_; i < std::numeric_limits<vmoid_t>::max(); i++) {
        if (!tree_.find(i).IsValid()) {
            *out = i;
            last_id_ = static_cast<vmoid_t>(i + 1);
            return ZX_OK;
        }
    }
    for (vmoid_t i = VMOID_INVALID + 1; i < last_id_; i++) {
        if (!tree_.find(i).IsValid()) {
            *out = i;
            last_id_ = static_cast<vmoid_t>(i + 1);
            return ZX_OK;
        }
    }
    return ZX_ERR_NO_RESOURCES;
}

zx_status_t BlockServer::AttachVmo(zx::vmo vmo, vmoid_t* out) {
    zx_status_t status;
    vmoid_t id;
    fbl::AutoLock server_lock(&server_lock_);
    if ((status = FindVmoIDLocked(&id)) != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    fbl::RefPtr<IoBuffer> ibuf = fbl::AdoptRef(new (&ac) IoBuffer(std::move(vmo), id));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    tree_.insert(std::move(ibuf));
    *out = id;
    return ZX_OK;
}

void BlockServer::TxnEnd() {
    size_t old_count = pending_count_.fetch_sub(1);
    ZX_ASSERT(old_count > 0);
    if ((old_count == 1) && barrier_in_progress_.load()) {
        // Since we're avoiding locking, and there is a gap between
        // "pending count decremented" and "FIFO signalled", it's possible
        // that we'll receive spurious wakeup requests.
        fifo_.signal(0, kSignalFifoOpsComplete);
    }
}

void BlockServer::InQueueDrainer() {
    while (true) {
        if (in_queue_.is_empty()) {
            return;
        }

        auto msg = in_queue_.begin();
        if (deferred_barrier_before_) {
            msg->op.command |= BLOCK_FL_BARRIER_BEFORE;
            deferred_barrier_before_ = false;
        }

        if (msg->op.command & BLOCK_FL_BARRIER_BEFORE) {
            barrier_in_progress_.store(true);
            if (pending_count_.load() > 0) {
                return;
            }
            // Since we're the only thread that could add to pending
            // count, we reliably know it has terminated.
            barrier_in_progress_.store(false);
        }
        if (msg->op.command & BLOCK_FL_BARRIER_AFTER) {
            deferred_barrier_before_ = true;
        }
        pending_count_.fetch_add(1);
        in_queue_.pop_front();
        // Underlying block device drivers should not see block barriers
        // which are already handled by the block midlayer.
        //
        // This may be altered in the future if block devices
        // are capable of implementing hardware barriers.
        msg->op.command &= ~(BLOCK_FL_BARRIER_BEFORE | BLOCK_FL_BARRIER_AFTER);
        bp_->Queue(&msg->op, BlockCompleteCb, &*msg);
    }
}

zx_status_t BlockServer::Create(ddk::BlockProtocolClient* bp, fzl::fifo<block_fifo_request_t,
                                block_fifo_response_t>* fifo_out,
                                fbl::unique_ptr<BlockServer>* out) {
    fbl::AllocChecker ac;
    BlockServer* bs = new (&ac) BlockServer(bp);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    fbl::unique_ptr<BlockServer> server(bs);

    zx_status_t status;
    if ((status = fzl::create_fifo(BLOCK_FIFO_MAX_DEPTH, 0, fifo_out, &server->fifo_)) != ZX_OK) {
        return status;
    }

    for (size_t i = 0; i < fbl::count_of(server->groups_); i++) {
        server->groups_[i].Initialize(server->fifo_.get_handle(), static_cast<groupid_t>(i));
    }

    // Notably, drop ZX_RIGHT_SIGNAL_PEER, since we use server->fifo for thread
    // signalling internally within the block server.
    zx_rights_t rights = ZX_RIGHT_TRANSFER | ZX_RIGHT_READ | ZX_RIGHT_WRITE |
            ZX_RIGHT_SIGNAL | ZX_RIGHT_WAIT;
    if ((status = fifo_out->replace(rights, fifo_out)) != ZX_OK) {
        return status;
    }

    bp->Query(&server->info_, &server->block_op_size_);

    // TODO(ZX-1583): Allocate BlockMsg arena based on block_op_size_.

    *out = std::move(server);
    return ZX_OK;
}

zx_status_t BlockServer::ProcessReadWriteRequest(block_fifo_request_t* request) {
    reqid_t reqid = request->reqid;
    groupid_t group = request->group;

    // TODO(ZX-1586): Reduce the usage of this lock (only used to protect
    // IoBuffers).
    fbl::AutoLock server_lock(&server_lock_);

    auto iobuf = tree_.find(request->vmoid);
    if (!iobuf.IsValid()) {
        // Operation which is not accessing a valid vmo.
        return ZX_ERR_IO;
    }

    if ((request->length < 1) ||
        (request->length > std::numeric_limits<uint32_t>::max())) {
        // Operation which is too small or too large.
        return ZX_ERR_INVALID_ARGS;
    }

    // Hack to ensure that the vmo is valid.
    // In the future, this code will be responsible for pinning VMO pages,
    // and the completion will be responsible for un-pinning those same pages.
    uint32_t bsz = info_.block_size;
    zx_status_t status = iobuf->ValidateVmoHack(bsz * request->length,
                                                bsz * request->vmo_offset);
    if (status != ZX_OK) {
        return status;
    }

    BlockMsg msg;
    if ((status = BlockMsg::Create(block_op_size_, &msg)) != ZX_OK) {
        return status;
    }
    block_msg_extra_t* extra = msg.extra();
    extra->iobuf = iobuf.CopyPointer();
    extra->server = this;
    extra->reqid = reqid;
    extra->group = group;
    msg.op()->command = OpcodeToCommand(request->opcode);

    const uint32_t max_xfer = info_.max_transfer_size / bsz;
    if (max_xfer != 0 && max_xfer < request->length) {
        uint32_t len_remaining = request->length;
        uint64_t vmo_offset = request->vmo_offset;
        uint64_t dev_offset = request->dev_offset;

        // If the request is larger than the maximum transfer size,
        // split it up into a collection of smaller block messages.
        //
        // Once all of these smaller messages are created, splice
        // them into the input queue together.
        BlockMsgQueue sub_txns_queue;
        uint32_t sub_txns = fbl::round_up(len_remaining, max_xfer) / max_xfer;
        uint32_t sub_txn_idx = 0;
        while (sub_txn_idx != sub_txns) {
            // We'll be using a new BlockMsg for each sub-component.
            if (!msg.valid()) {
                if ((status = BlockMsg::Create(block_op_size_, &msg)) != ZX_OK) {
                    return status;
                }
                block_msg_extra_t* extra = msg.extra();
                extra->iobuf = iobuf.CopyPointer();
                extra->server = this;
                extra->reqid = reqid;
                extra->group = group;
                msg.op()->command = OpcodeToCommand(request->opcode);
            }

            uint32_t length = fbl::min(len_remaining, max_xfer);
            len_remaining -= length;

            // Only set the "AFTER" barrier on the last sub-txn.
            msg.op()->command &= ~(sub_txn_idx == sub_txns - 1 ? 0 :
                                   BLOCK_FL_BARRIER_AFTER);
            // Only set the "BEFORE" barrier on the first sub-txn.
            msg.op()->command &= ~(sub_txn_idx == 0 ? 0 :
                                   BLOCK_FL_BARRIER_BEFORE);
            InQueueAdd(iobuf->vmo(), length, vmo_offset, dev_offset, msg.release(),
                       &sub_txns_queue);
            vmo_offset += length;
            dev_offset += length;
            sub_txn_idx++;
        }
        groups_[group].CtrAdd(sub_txns - 1);
        ZX_DEBUG_ASSERT(len_remaining == 0);

        in_queue_.splice(in_queue_.end(), sub_txns_queue);
    } else {
        InQueueAdd(iobuf->vmo(), request->length, request->vmo_offset,
                   request->dev_offset, msg.release(), &in_queue_);
    }
    return ZX_OK;
}

zx_status_t BlockServer::ProcessCloseVmoRequest(block_fifo_request_t* request) {
    fbl::AutoLock server_lock(&server_lock_);

    auto iobuf = tree_.find(request->vmoid);
    if (!iobuf.IsValid()) {
        // Operation which is not accessing a valid vmo
        return ZX_ERR_IO;
    }

    // TODO(smklein): Ensure that "iobuf" is not being used by
    // any in-flight txns.
    tree_.erase(*iobuf);
    return ZX_OK;
}

zx_status_t BlockServer::ProcessFlushRequest(block_fifo_request_t* request) {
    zx_status_t status;
    BlockMsg msg;
    if ((status = BlockMsg::Create(block_op_size_, &msg)) != ZX_OK) {
        return status;
    }
    block_msg_extra_t* extra = msg.extra();
    extra->iobuf = nullptr;
    extra->server = this;
    extra->reqid = request->reqid;
    extra->group = request->group;
    msg.op()->command = OpcodeToCommand(request->opcode);
    InQueueAdd(ZX_HANDLE_INVALID, 0, 0, 0, msg.release(), &in_queue_);
    return ZX_OK;
}

void BlockServer::ProcessRequest(block_fifo_request_t* request) {
    zx_status_t status;
    switch (request->opcode & BLOCKIO_OP_MASK) {
    case BLOCKIO_READ:
    case BLOCKIO_WRITE:
        if ((status = ProcessReadWriteRequest(request)) != ZX_OK) {
            TxnComplete(status, request->reqid, request->group);
        }
        break;
    case BLOCKIO_CLOSE_VMO:
        status = ProcessCloseVmoRequest(request);
        TxnComplete(status, request->reqid, request->group);
        break;
    case BLOCKIO_FLUSH:
        if ((status = ProcessFlushRequest(request)) != ZX_OK) {
            TxnComplete(status, request->reqid, request->group);
        }
        break;
    default:
        fprintf(stderr, "Unrecognized Block Server operation: %x\n",
                request->opcode);
        TxnComplete(ZX_ERR_NOT_SUPPORTED, request->reqid, request->group);
    }
}

zx_status_t BlockServer::Serve() {
    zx_status_t status;
    block_fifo_request_t requests[BLOCK_FIFO_MAX_DEPTH];
    size_t count;
    while (true) {
        // Attempt to drain as much of the input queue as possible
        // before (potentially) blocking in Read.
        InQueueDrainer();

        if ((status = Read(requests, &count) != ZX_OK)) {
            return status;
        }

        for (size_t i = 0; i < count; i++) {
            bool wants_reply = requests[i].opcode & BLOCKIO_GROUP_LAST;
            bool use_group = requests[i].opcode & BLOCKIO_GROUP_ITEM;

            reqid_t reqid = requests[i].reqid;

            if (use_group) {
                groupid_t group = requests[i].group;
                if (group >= MAX_TXN_GROUP_COUNT) {
                    // Operation which is not accessing a valid group.
                    if (wants_reply) {
                        OutOfBandRespond(fifo_, ZX_ERR_IO, reqid, group);
                    }
                    continue;
                }

                // Enqueue the message against the transaction group.
                status = groups_[group].Enqueue(wants_reply, reqid);
                if (status != ZX_OK) {
                    TxnComplete(status, reqid, group);
                    continue;
                }
            } else {
                requests[i].group = kNoGroup;
            }

            ProcessRequest(&requests[i]);
        }
    }
}

BlockServer::BlockServer(ddk::BlockProtocolClient* bp) :
    bp_(bp), block_op_size_(0), pending_count_(0), barrier_in_progress_(false),
    last_id_(VMOID_INVALID + 1) {
    size_t block_op_size;
    bp->Query(&info_, &block_op_size);
    ops_.acquire = OpAcquire;
    ops_.issue = OpIssue;
    ops_.release = OpRelease;
    ops_.cancel_acquire = OpCancelAcquire;
    ops_.fatal = FatalFromQueue;
    ops_.context = this;
}

BlockServer::~BlockServer() {
    ZX_ASSERT(pending_count_.load() == 0);
    ZX_ASSERT(in_queue_.is_empty());
}

void BlockServer::SignalFifoCancel() {
    fifo_.signal(0, kSignalFifoTerminate);
}

void BlockServer::Shutdown() {
    printf("%s:%u\n", __FUNCTION__, __LINE__);
    // Identify that the server should stop reading and return,
    // implicitly closing the fifo.
    // fifo_.signal(0, kSignalFifoTerminate);
    // zx_signals_t signals = kSignalFifoTerminated;
    // zx_signals_t seen;
    // fifo_.wait_one(signals, zx::time::infinite(), &seen);
}

zx_status_t BlockServer::OpAcquire(void* context, ioqueue::io_op_t** op_list, uint32_t* op_count, bool wait) {
    printf("acquire op\n");
    BlockServer* bs = static_cast<BlockServer*>(context);
    return bs->ReadOps(op_list, op_count, wait);
}

zx_status_t BlockServer::OpIssue(void* context, ioqueue::io_op_t* op) {
    printf("issue op\n");
    return ZX_OK;
}

void BlockServer::OpRelease(void* context, ioqueue::io_op_t* op) {
    printf("release op\n");
}

void BlockServer::OpCancelAcquire(void* context) {
    printf("cancel acquire\n");
    BlockServer* bs = static_cast<BlockServer*>(context);
    bs->SignalFifoCancel();
}

void BlockServer::FatalFromQueue(void* context) {
    ZX_DEBUG_ASSERT(false);
}
