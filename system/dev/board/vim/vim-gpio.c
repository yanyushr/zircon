// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>

#include <soc/aml-s905x/s905x-gpio.h>
#include <soc/aml-s912/s912-gpio.h>
#include <soc/aml-s912/s912-hw.h>

#include <limits.h>

#include "vim.h"

// S905X and S912 have same MMIO addresses
static const pbus_mmio_t gpio_mmios[] = {
    {
        .base = S912_GPIO_BASE,
        .length = S912_GPIO_LENGTH,
    },
    {
        .base = S912_GPIO_AO_BASE,
        .length = S912_GPIO_AO_LENGTH,
    },
    {
        .base = S912_GPIO_INTERRUPT_BASE,
        .length = S912_GPIO_INTERRUPT_LENGTH,
    },
};

// S905X and S912 have same GPIO IRQ numbers
static const pbus_irq_t gpio_irqs[] = {
    {
        .irq = S912_GPIO_IRQ_0,
    },
    {
        .irq = S912_GPIO_IRQ_1,
    },
    {
        .irq = S912_GPIO_IRQ_2,
    },
    {
        .irq = S912_GPIO_IRQ_3,
    },
    {
        .irq = S912_GPIO_IRQ_4,
    },
    {
        .irq = S912_GPIO_IRQ_5,
    },
    {
        .irq = S912_GPIO_IRQ_6,
    },
    {
        .irq = S912_GPIO_IRQ_7,
    },
    {
        .irq = S912_AO_GPIO_IRQ_0,
    },
    {
        .irq = S912_AO_GPIO_IRQ_1,
    },
};

static const pbus_dev_t gpio_dev = {
    .name = "gpio",
    .vid = PDEV_VID_AMLOGIC,
    .pid = PDEV_PID_AMLOGIC_S912,
    .did = PDEV_DID_AMLOGIC_GPIO,
    .mmio_list = gpio_mmios,
    .mmio_count = countof(gpio_mmios),
    .irq_list = gpio_irqs,
    .irq_count = countof(gpio_irqs),
};

zx_status_t vim_gpio_init(vim_bus_t* bus) {
    zx_status_t status = pbus_protocol_device_add(&bus->pbus, ZX_PROTOCOL_GPIO_IMPL, &gpio_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "vim_gpio_init: pbus_protocol_device_add failed: %d\n", status);
        return status;
    }

    status = device_get_protocol(bus->parent, ZX_PROTOCOL_GPIO_IMPL, &bus->gpio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "vim_gpio_init: device_get_protocol failed: %d\n", status);
        return status;
    }

#if 0
    const pbus_gpio_t gpio_test_gpios[] = {
        {
            // SYS_LED
            .gpio = S912_GPIOAO(9),
        },
        {
            // GPIO PIN
            .gpio = S912_GPIOAO(2),
        },
    };

    const pbus_dev_t gpio_test_dev = {
        .name = "vim-gpio-test",
        .vid = PDEV_VID_GENERIC,
        .pid = PDEV_PID_GENERIC,
        .did = PDEV_DID_GPIO_TEST,
        .gpio_list = gpio_test_gpios,
        .gpio_count = countof(gpio_test_gpios),
    };

    status = pbus_device_add(&bus->pbus, &gpio_test_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "vim_gpio_init could not add gpio_test_dev: %d\n", status);
        return status;
    }
#else
    const pbus_gpio_t gpio_light_gpios[] = {
        {
            // SYS_LED
            .gpio = S912_GPIOAO(9),
        },
    };

    const pbus_dev_t gpio_light_dev = {
        .name = "gpio-light",
        .vid = PDEV_VID_GENERIC,
        .pid = PDEV_PID_GENERIC,
        .did = PDEV_DID_GPIO_LIGHT,
        .gpio_list = gpio_light_gpios,
        .gpio_count = countof(gpio_light_gpios),
    };

    status = pbus_device_add(&bus->pbus, &gpio_light_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "vim_gpio_init could not add gpio_test_dev: %d\n", status);
        return status;
    }
#endif

    return ZX_OK;
}
