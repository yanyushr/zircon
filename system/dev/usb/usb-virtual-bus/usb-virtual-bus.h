// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/usb/bus.h>
#include <ddktl/protocol/usb/dci.h>
#include <ddktl/protocol/usb/hci.h>
#include <fbl/mutex.h>
#include <fbl/unique_ptr.h>
#include <lib/sync/completion.h>

#include <optional>
#include <threads.h>

#include "usb-virtual-device.h"
#include "usb-virtual-host.h"

namespace usb_virtual_bus {

struct usb_virtual_ep_t {
    list_node_t host_reqs;
    list_node_t device_reqs;
    // offset into current host req, for dealing with host reqs that are bigger than
    // their matching device req
    zx_off_t req_offset;
    bool stalled;
};

class UsbVirtualBus;
class UsbVirtualDevice;
class UsbVirtualHost;
using UsbVirtualBusType = ddk::Device<UsbVirtualBus, ddk::Unbindable, ddk::Messageable>;

// This is the main class for the platform bus driver.
class UsbVirtualBus : public UsbVirtualBusType {
public:
    explicit UsbVirtualBus(zx_device_t* parent)
        : UsbVirtualBusType(parent) {}

    static zx_status_t Create(zx_device_t* parent);

    // Device protocol implementation.
    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
    void DdkUnbind();
    void DdkRelease();

    // USB device controller protocol implementation.
    void UsbDciRequestQueue(usb_request_t* usb_request, const usb_request_complete_t* complete_cb);
    zx_status_t UsbDciSetInterface(const usb_dci_interface_t* interface);
    zx_status_t UsbDciConfigEp(const usb_endpoint_descriptor_t* ep_desc, const usb_ss_ep_comp_descriptor_t* ss_comp_desc);
    zx_status_t UsbDciDisableEp(uint8_t ep_address);
    zx_status_t UsbDciEpSetStall(uint8_t ep_address);
    zx_status_t UsbDciEpClearStall(uint8_t ep_address);
    size_t UsbDciGetRequestSize();

    // USB host controller protocol implementation.
    void UsbHciRequestQueue(usb_request_t* usb_request, const usb_request_complete_t* complete_cb);
    void UsbHciSetBusInterface(const usb_bus_interface_t* bus_intf);
    size_t UsbHciGetMaxDeviceCount();
    zx_status_t UsbHciEnableEndpoint(uint32_t device_id, const usb_endpoint_descriptor_t* ep_desc, const usb_ss_ep_comp_descriptor_t* ss_com_desc, bool enable);
    uint64_t UsbHciGetCurrentFrame();
    zx_status_t UsbHciConfigureHub(uint32_t device_id, usb_speed_t speed, const usb_hub_descriptor_t* desc);
    zx_status_t UsbHciHubDeviceAdded(uint32_t device_id, uint32_t port, usb_speed_t speed);
    zx_status_t UsbHciHubDeviceRemoved(uint32_t device_id, uint32_t port);
    zx_status_t UsbHciHubDeviceReset(uint32_t device_id, uint32_t port);
    zx_status_t UsbHciResetEndpoint(uint32_t device_id, uint8_t ep_address);
    zx_status_t UsbHciResetDevice(uint32_t hub_address, uint32_t device_id);
    size_t UsbHciGetMaxTransferSize(uint32_t device_id, uint8_t ep_address);
    zx_status_t UsbHciCancelAll(uint32_t device_id, uint8_t ep_address);
    size_t UsbHciGetRequestSize();

    // FIDL messages
    zx_status_t MsgEnable(fidl_txn_t* txn);
    zx_status_t MsgDisable(fidl_txn_t* txn);
    zx_status_t MsgConnect(fidl_txn_t* txn);
    zx_status_t MsgDisconnect(fidl_txn_t* txn);

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(UsbVirtualBus);

    zx_status_t Init();
    zx_status_t CreateDevice();
    zx_status_t CreateHost();
    void SetConnected(bool connected);
    int Thread();
    void HandleControl(usb_request_t* req);
    zx_status_t SetStall(uint8_t ep_address, bool stall);

    fbl::unique_ptr<UsbVirtualDevice> device_;
    fbl::unique_ptr<UsbVirtualHost> host_;

    std::optional<ddk::UsbDciInterfaceClient> dci_intf_;
    std::optional<ddk::UsbBusInterfaceClient> bus_intf_;

    usb_virtual_ep_t eps_[USB_MAX_EPS];

    thrd_t thread_;
    fbl::Mutex lock_;
    sync_completion_t completion_;
    bool connected_ __TA_GUARDED(lock_);
    bool unbinding_ __TA_GUARDED(lock_);
};

} // namespace usb_virtual_bus
