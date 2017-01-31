#include "task.h"
#include "string.h"
#include "liballoc.h"
#include "gdt.h"
#include "mm.h"
#include "elf.h"
#include "vfs.h"
#include "debug.h"
#include "errno.h"
#include "tss.h"
#include <stdbool.h>

void open_process_std();
extern page_directory_t *page_directory;
int execve(char *path, char **argv);

static void process_bootstrap(char *path)
{
    char *params[2];
    params[0] = "-dash";
    params[1] = 0;
    execve(path, params);
}

uint8_t exec(char *path)
{
    struct process *p = create_process(&process_bootstrap, (uint32_t)path);
    schedule_process(p);
    return 0;
}
extern void enter_userspace(uintptr_t location, uintptr_t stack);
char *envp[] = {"PWD=/"};

int execve(char *path, char **argv)
{
    int err = check_elf(path);
    if (err) {
        return err;
    }

    // count args count and required memory
    int argc = 0;
    int size = 0;
    while(argv[argc] != NULL) {
        size += strlen(argv[argc]) + 1; // +1 for EOS char
        argc++;
    }

    if (size + sizeof(uint32_t*) * argc > 0x1000) {
        return -E2BIG;
    }

    // copy params to tmp storage
    void *params = kmalloc(size);
    void *cur = params;
    for(uint32_t i = 0; i < argc; i++) {
        int length = strlen(argv[i]) + 1;
        memcpy(cur, argv[i], length);
        cur += length;
    }

    // backup exec path
    char *path_back = kmalloc(MAX_PATH_LENGTH);
    strcpy(path_back, path);

    free_userspace();
    current_process->brk = NULL;
    // from now thread must be terminated if any errors, it can't return to userspace anymore

    uint32_t entry_point = 0;
    err = load_elf(path_back, (void**)&entry_point);
    kfree(path_back);
    if (err || entry_point == 0) {
        kfree(params);
        stop_process();
    }

    // setup userspace stack
    void* brk = current_process->brk;
    for(uint8_t i = 0; i < USERSPACE_STACK_SIZE / 0x1000; i++) {
        uint32_t page = alloc_physical_page();
        map_virtual_to_physical(USERSPACE_STACK + i * 0x1000, page);
    }
    // restore brk because it's modified by last map_virtual_to_physical calls
    current_process->brk = brk;
    char **argv_new = current_process->brk;
    uint32_t offset = 0;
    uint32_t phys = alloc_physical_page();
    map_virtual_to_physical((uint32_t)argv_new, phys);
    char *rows = (void*)argv_new + sizeof(char*) * (argc + 1); // +1 for empty NULL param
    for(uint32_t i = 0; i < argc; i++) {
        uint32_t size = strlen(params + offset) + 1;
        memcpy(rows, params + offset, size);
        argv_new[i] = rows;
        rows += size;
        offset += size;
    }
    argv_new[argc] = 0;
    kfree(params);

    for(uint32_t i = 0; i < argc; i++) {
        debug("%s\n", argv_new[i]);
    }

    open_process_std();
    set_fg_pid(get_pid());

    set_kernel_stack((uint32_t)current_thread->stack_mem + KERNEL_STACK_SIZE);

    uint32_t user_stack = USERSPACE_STACK_TOP;

    PUSH_STACK(user_stack, envp);
    PUSH_STACK(user_stack, argv_new);
    PUSH_STACK(user_stack, argc);
    user_stack -= 4; // return address

    enter_userspace((uint32_t)entry_point, user_stack);
    assert(true, "something isn't ok with execve -> unreachable point");
    return 0;
}
