// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "usb-request.h"

#include <optional>

#include <ddk/debug.h>
#include <ddk/phys-iter.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <fbl/unique_ptr.h>
#include <lib/operation/operation.h>
#include <lib/zx/bti.h>
#include <lib/zx/vmo.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

namespace usb {

// Usage notes:
//
// usb::Request is a c++ wrapper around the usb_request_t object. It provides
// capabilites to interact with a usb_req buffer which is used to traverse the
// usb stack. On deletion, it will automatically free itself.
//
// usb::UnownedRequest provides an unowned variant of usb::Request. It adds
// functionality to store and call a complete callback which isn't present in
// usb::Request.  In addition, it will call the completion on destruction if it
// wasn't already triggered.
//
// usb::RequestPool provides pooling functionality for usb::Request reuse.
//
// usb::RequestQueue provides a queue interface for tracking usb::Request and
// usb::UnownedRequest objects.
//
// Available methods for both Request and UnownedRequest include:
//
//   usb_request_t* request(); // accessor for inner type.
//
//   // Takes ownership of inner type. Should only be used when transferring
//   // ownership to another driver.
//   usb_request_t* take();
//
//   All methods implemented in RequestBase (scroll below for additional info).
//
// Available to Request and UnownedRequest if they templatize of Storage:
//
//   Storage* private_storage(); // accessor for private storage.
//
// Available to Request:
//
//   void Release(); // Frees the inner type.
//
// Available to UnownedRequest:
//
//   void Complete(zx_status_t); // Completes the Request.
//
///////////////////////////////////////////////////////////////////////////////
// Example: Basic allocation with a pool:
//
// usb::RequestPool<> pool;
//
// const size_t op_size = usb::Request<>::RequestSize(parent_req_size);
// for (int i = 0; i < kNumRequest; i++) {
//     std::optional<usb::Request> request;
//     request = usb::Request::Alloc(op_size, parent_req_size);
//
//     if (!request) return ZX_ERR_NO_MEMORY;
//     pool.add(std::move(*request));
// }
//
///////////////////////////////////////////////////////////////////////////////
// Example: Enqueue incoming requests into a usb::RequestQueue:
//
// class Driver {
// public:
//     <...>
// private:
//     usb::UnownedRequestQueue<> request_;
//     const size_t parent_req_size_;
// };
//
// void Driver::UsbRequestQueue(usb_request_t* req, const usb_request_callback_t* completion_cb) {
//     request_.push(usb::UnownedRequest<>(op, cb, parent_req_size_));
// }
//
///////////////////////////////////////////////////////////////////////////////
// Example: Using private context only visible to your driver:
//
// struct PrivateStorage {
//     bool valid;
//     size_t count_metric;
// }
//
// using UsbRequest = usb::UnownedRequest<PrivateStorage>;
//
// void Driver::UsbReqeustQueue(usb_request_t* req, const usb_request_t* completion_cb) {
//     UsbReqeust usb_req(op, cb, parent_req_size_));
//     ZX_DEBUG_ASSERT(usb_req.request()->command == USB_ERASE);
//     usb_req.private_storage()->valid = true;
//     usb_req.private_storage()->count_metric += 1;
//     <...>
// }
//

class RequestBase {
public:
    // Copies the scatter gather list to the request.
    // Future transfers using this request will determine where in the VMO to store read/write data.
    // using the scatter gather list.
    // This will free any existing scatter gather list stored in the request.
    zx_status_t SetScatterGatherList(const phys_iter_sg_entry_t* sg_list, size_t sg_count) {
        return usb_request_set_sg_list(request(), sg_list, sg_count);
    }

    // Copies data from the Request's vm object.
    // Out of range operations are ignored.
    ssize_t CopyFrom(void* data, size_t length, size_t offset) {
        return usb_request_copy_from(request(), data, length, offset);
    }

    // Copies data into a Request's vm object.
    // Out of range operations are ignored.
    ssize_t CopyTo(const void* data, size_t length, size_t offset) {
        return usb_request_copy_to(request(), data, length, offset);
    }

    // Maps the Request's vm object. The 'data' field is set with the mapped address if this
    // function succeeds.
    zx_status_t Mmap(void** data) {
        return usb_request_mmap(request(), data);
    }

    // Performs a cache maintenance op against the request's internal buffer.
    zx_status_t CacheOp(uint32_t op, size_t offset, size_t length) {
        return usb_request_cacheop(request(), op, offset, length);
    }

    // Performs a cache flush on a range of memory in the request's buffer.
    zx_status_t CacheFlush(zx_off_t offset, size_t length) {
        return usb_request_cache_flush(request(), offset, length);
    }

    // Performs a cache flush and invalidate on a range of memory in the request's buffer.
    zx_status_t CacheFlushInvalidate(zx_off_t offset, size_t length) {
        return usb_request_cache_flush_invalidate(request(), offset, length);
    }

    // Looks up the physical pages backing this request's vm object.
    zx_status_t PhysMap(const zx::bti& bti) {
        return usb_request_physmap(request(), bti.get());
    }

    // Initializes a ddk::PhysIter for a usb request.
    // |max_length| is the maximum length of a range returned the iterator.
    // |max_length| must be either a positive multiple of PAGE_SIZE, or zero for no limit.
    ddk::PhysIter phys_iter(size_t max_length) {
        phys_iter_buffer_t buf = {
            .phys = request()->phys_list,
            .phys_count = request()->phys_count,
            .length = request()->header.length,
            .vmo_offset = request()->offset,
            .sg_list = request()->sg_list,
            .sg_count = request()->sg_count};
        return ddk::PhysIter(buf, max_length);
    }

    size_t alloc_size() const {
        return request()->alloc_size;
    }

    virtual usb_request_t* request() const;
};

struct OperationTraits {
    using OperationType = usb_request_t;

    static OperationType* Alloc(size_t op_size) {
        ZX_ASSERT(false);
        return nullptr;
    }

    static void Free(OperationType* op) {
        usb_request_release(op);
    }
};

template <typename Storage = void>
class Request : public operation::Operation<Request<Storage>, OperationTraits, Storage>,
                public RequestBase {
public:
    using BaseClass = operation::Operation<Request<Storage>, OperationTraits, Storage>;
    using NodeType = operation::OperationNode<Request<Storage>, OperationTraits, void, Storage>;

    // Creates a new usb request with payload space of data_size.
    static zx_status_t Alloc(std::optional<Request>* out, uint64_t data_size,
                             uint8_t ep_address, size_t req_size,
                             size_t parent_req_size = sizeof(usb_request_t)) {
        usb_request_t* request;
        zx_status_t status = usb_request_alloc(&request, data_size, ep_address, req_size);
        if (status == ZX_OK) {
            *out = Request(request, parent_req_size);
            new ((*out)->node()) NodeType((*out)->node_offset_);
        } else {
            *out = std::nullopt;
        }
        return status;
    }

    // Creates a new usb request with the given VMO.
    static zx_status_t AllocVmo(std::optional<Request>* out, const zx::vmo& vmo,
                                uint64_t vmo_offset, uint64_t length, uint8_t ep_address,
                                size_t req_size, size_t parent_req_size = sizeof(usb_request_t)) {
        usb_request_t* request;
        zx_status_t status = usb_request_alloc_vmo(&request, vmo.get(), vmo_offset, length,
                                                   ep_address, req_size);
        if (status == ZX_OK) {
            *out = Request(request, parent_req_size);
            new ((*out)->node()) NodeType((*out)->node_offset_);
        } else {
            *out = std::nullopt;
        }
        return status;
    }

    Request(usb_request_t* request, size_t parent_req_size)
        : BaseClass(request, parent_req_size) {}

    Request(Request&& other)
        : BaseClass(std::move(other)) {}

    Request& operator=(Request&& other) {
        BaseClass::operator=(std::move(other));
        return *this;
    }

    virtual ~Request() = default;

    // Initializes the statically allocated usb request with the given VMO.
    // This will free any resources allocated by the usb request but not the usb request itself.
    zx_status_t Init(const zx::vmo& vmo, uint64_t vmo_offset, uint64_t length, uint8_t ep_address) {
        return usb_request_init(BaseClass::operation_, vmo.get(), vmo_offset, length, ep_address);
    }

    static constexpr size_t RequestSize(size_t parent_req_size) {
        return BaseClass::OperationSize(parent_req_size);
    }

    usb_request_t* request() const override {
        return BaseClass::operation();
    }
};

struct CallbackTraits {
    using CallbackType = void(void*, usb_request_t*);

    static std::tuple<zx_status_t, uint32_t> AutoCompleteArgs() {
        return std::make_tuple(ZX_ERR_INTERNAL, 0);
    }

    static void Callback(CallbackType* callback, void* cookie, usb_request_t* op,
                         zx_status_t status, zx_off_t actual) {
        usb_request_complete_t complete_cb = {
            .callback = callback,
            .ctx = cookie,
        };
        usb_request_complete(op, status, actual, &complete_cb);
    }
};

// Similar to usb::Request, but it doesn't call usb_request_release on delete.
// This should be used to wrap usb_request_t* objects allocated in other
// drivers.
template <typename Storage = void>
class UnownedRequest : public operation::UnownedOperation<UnownedRequest<Storage>, OperationTraits,
                                                          CallbackTraits, Storage>,
                       public RequestBase {
public:
    using BaseClass = operation::UnownedOperation<UnownedRequest<Storage>,
                                                  OperationTraits, CallbackTraits, Storage>;

    UnownedRequest(usb_request_t* request, const usb_request_complete_t& complete_cb,
                   size_t parent_req_size)
        : BaseClass(request, complete_cb.callback, complete_cb.ctx, parent_req_size) {}

    UnownedRequest(usb_request_t* request, size_t parent_req_size)
        : BaseClass(request, parent_req_size) {}

    UnownedRequest(UnownedRequest&& other)
        : BaseClass(std::move(other)) {}

    UnownedRequest& operator=(UnownedRequest&& other) {
        BaseClass::operator=(std::move(other));
        return *this;
    }

    virtual ~UnownedRequest() = default;

    static constexpr size_t RequestSize(size_t parent_req_size) {
        return BaseClass::OperationSize(parent_req_size);
    }

    usb_request_t* request() const override {
        return BaseClass::operation();
    }
};

// A driver may use usb::RequestPool for recycling their own usb requests.
template <typename Storage = void>
class RequestPool : operation::OperationPool<Request<Storage>, OperationTraits, Storage> {
public:
    using BaseClass = operation::OperationPool<Request<Storage>, OperationTraits, Storage>;

    // Inherit constructors.
    using BaseClass::BaseClass;

    void Add(Request<Storage> req) {
        BaseClass::push(std::forward<Request<Storage>>(req));
    }

    // Returns a request from the pool that has a buffer of the given length,
    // or null if no such request exists.
    // The request is not re-initialized in any way and should be set accordingly by the user.
    std::optional<Request<Storage>> Get(size_t length) {
        fbl::AutoLock al(&this->lock_);
        auto node = this->queue_.erase_if([length](const auto& node) {
            auto request = node.operation();
            const size_t size = request.alloc_size();
            __UNUSED auto* dummy = request.take(); // Don't free request.
            return size == length;
        });
        if (node) {
            return node->operation();
        }
        return std::nullopt;
    }

    using BaseClass::Release;
};

template <typename Storage = void>
using UnownedRequestQueue = operation::UnownedOperationQueue<UnownedRequest<Storage>,
                                                             OperationTraits,
                                                             CallbackTraits, Storage>;

template <typename Storage = void>
using RequestQueue = operation::OperationQueue<Request<Storage>, OperationTraits, Storage>;

} // namespace usb
