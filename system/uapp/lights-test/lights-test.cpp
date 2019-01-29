// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/auto_call.h>
#include <fuchsia/hardware/light/c/fidl.h>
#include <lib/fdio/util.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#define DEV_LIGHT_DIR  "/dev/class/light"


static int name_command(zx_handle_t svc, int argc, const char* argv[]) {
    char buffer[fuchsia_hardware_light_LIGHT_NAME_LEN];
    size_t actual;
    auto status = fuchsia_hardware_light_LightGetName(svc, buffer, sizeof(buffer), &actual);
    if (status == ZX_OK) {
        printf("%s\n", buffer);
        return 0;
    } else {
        fprintf(stderr, "fuchsia_hardware_light_DeviceGetName failed: %d\n", status);
        return -1;
    }
}

static int count_command(zx_handle_t svc, int argc, const char* argv[]) {
    uint32_t count;
    auto status = fuchsia_hardware_light_LightGetCount(svc, &count);
    if (status == ZX_OK) {
        printf("%u\n", count);
        return 0;
    } else {
        fprintf(stderr, "fuchsia_hardware_light_DeviceGetCount failed: %d\n", status);
        return -1;
    }
}

struct Command {
    const char* name;
    int (*command)(zx_handle_t svc, int argc, const char* argv[]);
    const char* description;
};

static Command commands[] = {
    {
        "name",
        name_command,
        "name - returns the name of the light"
    },
    {
        "count",
        count_command,
        "count - returns the number of physical lights"
    },
    {},
};

static void usage(void) {
    fprintf(stderr, "usage: \"lights-test [-d <dev-file>] <command>\", where command is one of:\n");

    Command* command = commands;
    while (command->name) {
        fprintf(stderr, "    %s\n", command->description);
        command++;
    }
}

int main(int argc, const char** argv) {
    if (argc < 2) {
        usage();
        return -1;
    }
    argv++;
    argc--;

    const char* dev_file_name = "000";
    if (!strcmp(argv[0], "-d")) {
        if (argc < 3) {
            usage();
            return -1;
        }

        dev_file_name = argv[1];
        if (strlen(dev_file_name) != 3) {
            usage();
            return -1;
        }

        argv += 2;
        argc -= 2;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/dev/class/light/%s", dev_file_name);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("Error opening %s\n", path);
        return -1;
    }

    zx_handle_t svc;
    zx_status_t status = fdio_get_service_handle(fd, &svc);
    if (status != ZX_OK) {
        close(fd);
        printf("Error opening FIDL connection for %s\n", path);
        return -1;
    }
    auto cleanup = fbl::MakeAutoCall([fd, svc]() {zx_handle_close(svc); close(fd); });
    
    const char* command_name = argv[0];
    argv++;
    argc--;
    Command* command = commands;
    while (command->name) {
        if (!strcmp(command_name, command->name)) {
            return command->command(svc, argc, argv);
        }
        command++;
    }
    // if we fall through, print usage
    usage();
    return -1;
}
