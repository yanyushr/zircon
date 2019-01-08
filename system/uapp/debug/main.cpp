#include <unistd.h>
#include <fcntl.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <stdio.h>

int main(int argc, char** argv) {
    zx_handle_t output;
    if(zx_debugger_get_vmo(&output)) {
        printf("Unable to acquire debug VMO\n");
    }
    size_t vmoLen;
    if(zx_vmo_get_size(output, &vmoLen)) {
        printf("Failed to get VMO size\n");
    }
    zx_vaddr_t addr;
    if(zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ, 0, output, 0, vmoLen, &addr)) {
        printf("Failed to map VMO into root VMAR\n");
    }

    int fd = open("/data/debug", O_WRONLY | O_CREAT);
    write(fd, ((size_t*)addr)+1, *(size_t*)((size_t)addr));
    return 0;
}