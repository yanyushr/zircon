// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/protocol/acpi.h>
#include <ddk/protocol/pciroot.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>

#include <acpica/acpi.h>
#include <zircon/compiler.h>
#include <zircon/process.h>
#include <zircon/syscalls/iommu.h>

#include "acpi-private.h"
#include "cpu-trace.h"
#include "dev.h"
#include "errors.h"
#include "init.h"
#include "iommu.h"
#include "methods.h"
#include "nhlt.h"
#include "pci.h"
#include "pciroot.h"
#include "power.h"
#include "resources.h"
#include "sysmem.h"

zx_handle_t root_resource_handle;

static void acpi_device_release(void* ctx) {
    acpi_device_t* dev = (acpi_device_t*)ctx;
    free(dev);
}

static zx_protocol_device_t acpi_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = acpi_device_release,
};

typedef struct {
    acpi_device_resource_t* resources;
    size_t resource_count;
    size_t resource_i;

    acpi_device_irq_t* irqs;
    size_t irq_count;
    size_t irq_i;
} acpi_crs_ctx_t;

static ACPI_STATUS report_current_resources_resource_cb(ACPI_RESOURCE* res, void* _ctx) {
    acpi_crs_ctx_t* ctx = (acpi_crs_ctx_t*)_ctx;

    if (resource_is_memory(res)) {
        resource_memory_t mem;
        zx_status_t st = resource_parse_memory(res, &mem);
        // only expect fixed memory resource. resource_parse_memory sets minimum == maximum
        // for this memory resource type.
        if ((st != ZX_OK) || (mem.minimum != mem.maximum)) {
            return AE_ERROR;
        }

        ctx->resources[ctx->resource_i].writeable = mem.writeable;
        ctx->resources[ctx->resource_i].base_address = mem.minimum;
        ctx->resources[ctx->resource_i].alignment = mem.alignment;
        ctx->resources[ctx->resource_i].address_length = mem.address_length;

        ctx->resource_i += 1;

    } else if (resource_is_address(res)) {
        resource_address_t addr;
        zx_status_t st = resource_parse_address(res, &addr);
        if (st != ZX_OK) {
            return AE_ERROR;
        }
        if ((addr.resource_type == RESOURCE_ADDRESS_MEMORY) && addr.min_address_fixed &&
            addr.max_address_fixed && (addr.maximum < addr.minimum)) {

            ctx->resources[ctx->resource_i].writeable = true;
            ctx->resources[ctx->resource_i].base_address = addr.min_address_fixed;
            ctx->resources[ctx->resource_i].alignment = 0;
            ctx->resources[ctx->resource_i].address_length = addr.address_length;

            ctx->resource_i += 1;
        }

    } else if (resource_is_irq(res)) {
        resource_irq_t irq;
        zx_status_t st = resource_parse_irq(res, &irq);
        if (st != ZX_OK) {
            return AE_ERROR;
        }
        for (size_t i = 0; i < irq.pin_count; i++) {
            ctx->irqs[ctx->irq_i].trigger = irq.trigger;
            ctx->irqs[ctx->irq_i].polarity = irq.polarity;
            ctx->irqs[ctx->irq_i].sharable = irq.sharable;
            ctx->irqs[ctx->irq_i].wake_capable = irq.wake_capable;
            ctx->irqs[ctx->irq_i].pin = irq.pins[i];

            ctx->irq_i += 1;
        }
    }

    return AE_OK;
}

static ACPI_STATUS report_current_resources_count_cb(ACPI_RESOURCE* res, void* _ctx) {
    acpi_crs_ctx_t* ctx = (acpi_crs_ctx_t*)_ctx;

    if (resource_is_memory(res)) {
        resource_memory_t mem;
        zx_status_t st = resource_parse_memory(res, &mem);
        if ((st != ZX_OK) || (mem.minimum != mem.maximum)) {
            return AE_ERROR;
        }
        ctx->resource_count += 1;

    } else if (resource_is_address(res)) {
        resource_address_t addr;
        zx_status_t st = resource_parse_address(res, &addr);
        if (st != ZX_OK) {
            return AE_ERROR;
        }
        if ((addr.resource_type == RESOURCE_ADDRESS_MEMORY) && addr.min_address_fixed &&
            addr.max_address_fixed && (addr.maximum < addr.minimum)) {
            ctx->resource_count += 1;
        }

    } else if (resource_is_irq(res)) {
        ctx->irq_count += res->Data.Irq.InterruptCount;
    }

    return AE_OK;
}

static zx_status_t report_current_resources(acpi_device_t* dev) {
    acpi_crs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (dev->got_resources) {
        return ZX_OK;
    }

    // call _CRS to count number of resources
    ACPI_STATUS acpi_status = AcpiWalkResources(dev->ns_node, (char*)"_CRS",
            report_current_resources_count_cb, &ctx);
    if ((acpi_status != AE_NOT_FOUND) && (acpi_status != AE_OK)) {
        return acpi_to_zx_status(acpi_status);
    }

    if (ctx.resource_count == 0) {
        return ZX_OK;
    }

    // allocate resources
    ctx.resources = calloc(ctx.resource_count, sizeof(acpi_device_resource_t));
    if (!ctx.resources) {
        return ZX_ERR_NO_MEMORY;
    }
    ctx.irqs = calloc(ctx.irq_count, sizeof(acpi_device_irq_t));
    if (!ctx.irqs) {
        free(ctx.resources);
        return ZX_ERR_NO_MEMORY;
    }

    // call _CRS again and fill in resources
    acpi_status = AcpiWalkResources(dev->ns_node, (char*)"_CRS",
            report_current_resources_resource_cb, &ctx);
    if ((acpi_status != AE_NOT_FOUND) && (acpi_status != AE_OK)) {
        free(ctx.resources);
        free(ctx.irqs);
        return acpi_to_zx_status(acpi_status);
    }

    dev->resources = ctx.resources;
    dev->resource_count = ctx.resource_count;
    dev->irqs = ctx.irqs;
    dev->irq_count = ctx.irq_count;

    zxlogf(TRACE, "acpi-bus[%s]: found %zd resources %zx irqs\n", device_get_name(dev->zxdev),
            dev->resource_count, dev->irq_count);
    if (driver_get_log_flags() & DDK_LOG_SPEW) {
        zxlogf(SPEW, "resources:\n");
        for (size_t i = 0; i < dev->resource_count; i++) {
            zxlogf(SPEW, "  %02zd: addr=0x%x length=0x%x align=0x%x writeable=%d\n", i,
                    dev->resources[i].base_address,
                    dev->resources[i].address_length,
                    dev->resources[i].alignment,
                    dev->resources[i].writeable);
        }
        zxlogf(SPEW, "irqs:\n");
        for (size_t i = 0; i < dev->irq_count; i++) {
            zxlogf(SPEW, "  %02zd: pin=%u %s %s %s %s\n", i,
                    dev->irqs[i].pin,
                    dev->irqs[i].trigger ? "edge" : "level",
                    (dev->irqs[i].polarity == 2) ? "both" :
                        (dev->irqs[i].polarity ? "low" : "high"),
                    dev->irqs[i].sharable ? "shared" : "exclusive",
                    dev->irqs[i].wake_capable ? "wake" : "nowake");
        }
    }

    dev->got_resources = true;

    return ZX_OK;
}

static zx_status_t acpi_op_map_resource(void* ctx, uint32_t res_id, uint32_t cache_policy,
        void** out_vaddr, size_t* out_size, zx_handle_t* out_handle) {
    acpi_device_t* dev = (acpi_device_t*)ctx;
    mtx_lock(&dev->lock);

    zx_status_t st = report_current_resources(dev);
    if (st != ZX_OK) {
        goto unlock;
    }

    if (res_id >= dev->resource_count) {
        st = ZX_ERR_NOT_FOUND;
        goto unlock;
    }

    acpi_device_resource_t* res = dev->resources + res_id;
    if (((res->base_address & (PAGE_SIZE - 1)) != 0) ||
        ((res->address_length & (PAGE_SIZE - 1)) != 0)) {
        zxlogf(ERROR, "acpi-bus[%s]: resource id=%d addr=0x%08x len=0x%x is not page aligned\n",
                device_get_name(dev->zxdev), res_id, res->base_address, res->address_length);
        st = ZX_ERR_NOT_FOUND;
        goto unlock;
    }

    zx_handle_t vmo;
    zx_vaddr_t vaddr;
    size_t size = res->address_length;
    st = zx_vmo_create_physical(get_root_resource(), res->base_address, size, &vmo);
    if (st != ZX_OK) {
        goto unlock;
    }

    st = zx_vmo_set_cache_policy(vmo, cache_policy);
    if (st != ZX_OK) {
        zx_handle_close(vmo);
        goto unlock;
    }

    st = zx_vmar_map(zx_vmar_root_self(),
                     ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                     0, vmo, 0, size, &vaddr);
    if (st != ZX_OK) {
        zx_handle_close(vmo);
    } else {
        *out_handle = vmo;
        *out_vaddr = (void*)vaddr;
        *out_size = size;
    }
unlock:
    mtx_unlock(&dev->lock);
    return st;
}

static zx_status_t acpi_op_map_interrupt(void* ctx, int64_t which_irq, zx_handle_t* out_handle) {
    acpi_device_t* dev = (acpi_device_t*)ctx;
    mtx_lock(&dev->lock);

    zx_status_t st = report_current_resources(dev);
    if (st != ZX_OK) {
        goto unlock;
    }

    if ((uint)which_irq >= dev->irq_count) {
        st = ZX_ERR_NOT_FOUND;
        goto unlock;
    }

    acpi_device_irq_t* irq = dev->irqs + which_irq;
    zx_handle_t handle;
    st = zx_interrupt_create(get_root_resource(), irq->pin, ZX_INTERRUPT_REMAP_IRQ, &handle);
    if (st != ZX_OK) {
        goto unlock;
    }
    *out_handle = handle;

unlock:
    mtx_unlock(&dev->lock);
    return st;
}

// TODO marking unused until we publish some devices
static __attribute__ ((unused)) acpi_protocol_ops_t acpi_proto = {
    .map_resource = acpi_op_map_resource,
    .map_interrupt = acpi_op_map_interrupt,
};

static zx_protocol_device_t acpi_root_device_proto = {
    .version = DEVICE_OPS_VERSION,
};

static zx_status_t sys_device_suspend(void* ctx, uint32_t flags) {
    switch (flags & DEVICE_SUSPEND_REASON_MASK) {
    case DEVICE_SUSPEND_FLAG_MEXEC: {
        AcpiTerminate();
        return ZX_OK;
    }
    case DEVICE_SUSPEND_FLAG_REBOOT:
        reboot();
        // Kill this driver so that the IPC channel gets closed; devmgr will
        // perform a fallback that should shutdown or reboot the machine.
        exit(0);
    case DEVICE_SUSPEND_FLAG_POWEROFF:
        poweroff();
        exit(0);
    case DEVICE_SUSPEND_FLAG_SUSPEND_RAM:
        return suspend_to_ram();
    default:
        return ZX_ERR_NOT_SUPPORTED;
    };
}

static zx_protocol_device_t sys_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .suspend = sys_device_suspend,
};

static const char* hid_from_acpi_devinfo(ACPI_DEVICE_INFO* info) {
    const char* hid = NULL;
    if ((info->Valid & ACPI_VALID_HID) &&
            (info->HardwareId.Length > 0) &&
            ((info->HardwareId.Length - 1) <= sizeof(uint64_t))) {
        hid = (const char*)info->HardwareId.String;
    }
    return hid;
}

zx_device_t* publish_device(zx_device_t* parent,
                            ACPI_HANDLE handle,
                            ACPI_DEVICE_INFO* info,
                            const char* name,
                            uint32_t protocol_id,
                            void* protocol_ops) {
    zx_device_prop_t props[4];
    int propcount = 0;

    char acpi_name[5] = { 0 };
    if (!name) {
        memcpy(acpi_name, &info->Name, sizeof(acpi_name) - 1);
        name = (const char*)acpi_name;
    }

    // Publish HID in device props
    const char* hid = hid_from_acpi_devinfo(info);
    if (hid) {
        props[propcount].id = BIND_ACPI_HID_0_3;
        props[propcount++].value = htobe32(*((uint32_t*)(hid)));
        props[propcount].id = BIND_ACPI_HID_4_7;
        props[propcount++].value = htobe32(*((uint32_t*)(hid + 4)));
    }

    // Publish the first CID in device props
    const char* cid = (const char*)info->CompatibleIdList.Ids[0].String;
    if ((info->Valid & ACPI_VALID_CID) &&
            (info->CompatibleIdList.Count > 0) &&
            ((info->CompatibleIdList.Ids[0].Length - 1) <= sizeof(uint64_t))) {
        props[propcount].id = BIND_ACPI_CID_0_3;
        props[propcount++].value = htobe32(*((uint32_t*)(cid)));
        props[propcount].id = BIND_ACPI_CID_4_7;
        props[propcount++].value = htobe32(*((uint32_t*)(cid + 4)));
    }

    if (driver_get_log_flags() & DDK_LOG_SPEW) {
        // ACPI names are always 4 characters in a uint32
        zxlogf(SPEW, "acpi: got device %s\n", acpi_name);
        if (info->Valid & ACPI_VALID_HID) {
            zxlogf(SPEW, "     HID=%s\n", info->HardwareId.String);
        } else {
            zxlogf(SPEW, "     HID=invalid\n");
        }
        if (info->Valid & ACPI_VALID_ADR) {
            zxlogf(SPEW, "     ADR=0x%" PRIx64 "\n", (uint64_t)info->Address);
        } else {
            zxlogf(SPEW, "     ADR=invalid\n");
        }
        if (info->Valid & ACPI_VALID_CID) {
            zxlogf(SPEW, "    CIDS=%d\n", info->CompatibleIdList.Count);
            for (uint i = 0; i < info->CompatibleIdList.Count; i++) {
                zxlogf(SPEW, "     [%u] %s\n", i, info->CompatibleIdList.Ids[i].String);
            }
        } else {
            zxlogf(SPEW, "     CID=invalid\n");
        }
        zxlogf(SPEW, "    devprops:\n");
        for (int i = 0; i < propcount; i++) {
            zxlogf(SPEW, "     [%d] id=0x%08x value=0x%08x\n", i, props[i].id, props[i].value);
        }
    }

    acpi_device_t* dev = calloc(1, sizeof(acpi_device_t));
    if (!dev) {
        return NULL;
    }

    dev->ns_node = handle;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ctx = dev,
        .ops = &acpi_device_proto,
        .proto_id = protocol_id,
        .proto_ops = protocol_ops,
        .props = (propcount > 0) ? props : NULL,
        .prop_count = propcount,
    };

    zx_status_t status;
    if ((status = device_add(parent, &args, &dev->zxdev)) != ZX_OK) {
        zxlogf(ERROR, "acpi: error %d in device_add, parent=%s(%p)\n",
                status, device_get_name(parent), parent);
        free(dev);
        return NULL;
    } else {
        zxlogf(ERROR, "acpi: published device %s(%p), parent=%s(%p), handle=%p\n",
                name, dev, device_get_name(parent), parent, (void*)dev->ns_node);
        return dev->zxdev;
    }
}

static ACPI_STATUS acpi_ns_walk_callback(ACPI_HANDLE object, uint32_t nesting_level,
                                         void* context, void** status) {
    ACPI_DEVICE_INFO* info = NULL;
    ACPI_STATUS acpi_status = AcpiGetObjectInfo(object, &info);
    if (acpi_status != AE_OK) {
        return acpi_status;
    }

    publish_acpi_device_ctx_t* ctx = (publish_acpi_device_ctx_t*)context;
    zx_device_t* parent = ctx->parent;

    // TODO: This is a temporary workaround until we have full ACPI device
    // enumeration. If this is the I2C1 bus, we run _PS0 so the controller
    // is active.
    if (!memcmp(&info->Name, "I2C1", 4)) {
        acpi_status = AcpiEvaluateObject(object, (char*)"_PS0", NULL, NULL);
        if (acpi_status != AE_OK) {
            zxlogf(ERROR, "acpi: acpi error 0x%x in I2C1._PS0\n", acpi_status);
        }

    // Attach the NHLT table as metadata on the HDA device.
    // The ACPI node representing the HDA controller is named "HDAS" on Pixelbook.
    // TODO: This is a temporary workaround for ACPI device enumeration.
    } else if (!memcmp(&info->Name, "HDAS", 4)) {
        // We must have already seen at least one PCI root due to traversal order.
        if (ctx->last_pci == 0xFF) {
            zxlogf(ERROR, "acpi: Found HDAS node, but no prior PCI root was discovered!\n");
        } else if (!(info->Valid & ACPI_VALID_ADR)) {
            zxlogf(ERROR, "acpi: no valid ADR found for HDA device\n");
        } else {
            zx_status_t status = nhlt_publish_metadata(parent,
                                                       ctx->last_pci,
                                                       (uint64_t)info->Address,
                                                       object);
            if ((status != ZX_OK) && (status != ZX_ERR_NOT_FOUND)) {
                zxlogf(ERROR, "acpi: failed to publish NHLT metadata\n");
            }
        }
    }

    const char* hid = hid_from_acpi_devinfo(info);
    if (hid == 0) {
        goto out;
    }
    const char* cid = NULL;
    if ((info->Valid & ACPI_VALID_CID) &&
            (info->CompatibleIdList.Count > 0) &&
            // IDs may be 7 or 8 bytes, and Length includes the null byte
            (info->CompatibleIdList.Ids[0].Length == HID_LENGTH ||
             info->CompatibleIdList.Ids[0].Length == HID_LENGTH + 1)) {
        cid = (const char*)info->CompatibleIdList.Ids[0].String;
    }

    if ((!memcmp(hid, PCI_EXPRESS_ROOT_HID_STRING, HID_LENGTH) ||
         !memcmp(hid, PCI_ROOT_HID_STRING, HID_LENGTH))) {
        pci_init(parent, object, info, ctx);
    } else if (!memcmp(hid, BATTERY_HID_STRING, HID_LENGTH)) {
        battery_init(parent, object);
    } else if (!memcmp(hid, PWRSRC_HID_STRING, HID_LENGTH)) {
        pwrsrc_init(parent, object);
    } else if (!memcmp(hid, EC_HID_STRING, HID_LENGTH)) {
        ec_init(parent, object);
    } else if (!memcmp(hid, GOOGLE_TBMC_HID_STRING, HID_LENGTH)) {
        tbmc_init(parent, object);
    } else if (!memcmp(hid, GOOGLE_CROS_EC_HID_STRING, HID_LENGTH)) {
        cros_ec_lpc_init(parent, object);
    } else if (!memcmp(hid, DPTF_THERMAL_HID_STRING, HID_LENGTH)) {
        thermal_init(parent, info, object);
    } else if (!memcmp(hid, I8042_HID_STRING, HID_LENGTH) ||
               (cid && !memcmp(cid, I8042_HID_STRING, HID_LENGTH))) {
        publish_device(parent, object, info, "i8042", ZX_PROTOCOL_ACPI, &acpi_proto);
    } else if (!memcmp(hid, RTC_HID_STRING, HID_LENGTH) ||
               (cid && !memcmp(cid, RTC_HID_STRING, HID_LENGTH))) {
        publish_device(parent, object, info, "rtc", ZX_PROTOCOL_ACPI, &acpi_proto);
    }

out:
    ACPI_FREE(info);

    return AE_OK;
}

static zx_status_t publish_acpi_devices(zx_device_t* parent) {
    zx_status_t status = pwrbtn_init(parent);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: failed to initialize pwrbtn device: %d\n", status);
    }

    // Walk the ACPI namespace for devices and publish them
    // Only publish a single PCI device
    publish_acpi_device_ctx_t ctx = {
        .parent = parent,
        .found_pci = false,
        .last_pci = 0xFF,
    };
    ACPI_STATUS acpi_status = AcpiWalkNamespace(ACPI_TYPE_DEVICE,
                                                ACPI_ROOT_OBJECT,
                                                MAX_NAMESPACE_DEPTH,
                                                acpi_ns_walk_callback,
                                                NULL, &ctx, NULL);
    if (acpi_status != AE_OK) {
        return ZX_ERR_BAD_STATE;
    } else {
        return ZX_OK;
    }
}

static zx_status_t acpi_drv_create(void* ctx, zx_device_t* parent, const char* name,
                                   const char* _args, zx_handle_t zbi_vmo) {
    // ACPI is the root driver for its devhost so run init in the bind thread.
    zxlogf(TRACE, "acpi: bind to %s %p\n", device_get_name(parent), parent);
    root_resource_handle = get_root_resource();

    // We don't need ZBI VMO handle.
    zx_handle_close(zbi_vmo);

    zx_status_t status = init();
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: failed to initialize ACPI %d \n", status);
        return ZX_ERR_INTERNAL;
    }

    zxlogf(TRACE, "acpi: initialized\n");

    // publish sys root
    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ops = &sys_device_proto,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    zx_device_t* sys_root = NULL;
    status = device_add(parent, &args, &sys_root);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: error %d in device_add(sys)\n", status);
        return status;
    }

    zx_handle_t dummy_iommu_handle;
    status = iommu_manager_get_dummy_iommu(&dummy_iommu_handle);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi-bus: error %d in iommu_manager_get_dummy_iommu()\n", status);
        return status;
    }

    // Sysmem is started early so zx_vmo_create_contiguous() works.
    zx_handle_t sysmem_bti;
    status = zx_bti_create(dummy_iommu_handle, 0, SYSMEM_BTI_ID, &sysmem_bti);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: error %d in bti_create(sysmem_bti)\n", status);
        return status;
    }
    status = publish_sysmem(sysmem_bti, sys_root);
    if (status != ZX_OK) {
        zxlogf(ERROR, "publish_sysmem failed: %d\n", status);
        return status;
    }

    zx_handle_t cpu_trace_bti;
    status = zx_bti_create(dummy_iommu_handle, 0, CPU_TRACE_BTI_ID, &cpu_trace_bti);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: error %d in bti_create(cpu_trace_bti)\n", status);
        return status;
    }
    status = publish_cpu_trace(cpu_trace_bti, sys_root);
    if (status != ZX_OK) {
        zxlogf(ERROR, "publish_cpu_trace failed: %d\n", status);
        return status;
    }

    // publish acpi root
    device_add_args_t args2 = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "acpi",
        .ops = &acpi_root_device_proto,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    zx_device_t* acpi_root = NULL;
    status = device_add(sys_root, &args2, &acpi_root);
    if (status != ZX_OK) {
        zxlogf(ERROR, "acpi: error %d in device_add(sys/acpi)\n", status);
        device_remove(sys_root);
        return status;
    }

    // TODO - retrieve more useful board name from ACPI data
    const char board_name[] = { "pc" };

    // Publish board name to sysinfo driver
    status = device_publish_metadata(acpi_root, "/dev/misc/sysinfo", DEVICE_METADATA_BOARD_NAME,
                                     board_name, sizeof(board_name));
    if (status != ZX_OK) {
        zxlogf(ERROR, "device_publish_metadata(board_name) failed: %d\n", status);
    }

    publish_acpi_devices(acpi_root);

    return ZX_OK;
}

static zx_driver_ops_t acpi_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .create = acpi_drv_create,
};

ZIRCON_DRIVER_BEGIN(acpi, acpi_driver_ops, "zircon", "0.1", 1)
    BI_ABORT_IF_AUTOBIND, // loaded by devcoordinator
ZIRCON_DRIVER_END(acpi)
