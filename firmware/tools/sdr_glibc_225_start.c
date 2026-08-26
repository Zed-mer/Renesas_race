/*
 * Minimal ARM Linux entry point for old Pluto glibc images.
 *
 * Zig can cap imported symbol versions at glibc 2.25 while still selecting
 * newer static Scrt1/libc_nonshared startup objects.  Link this file with
 * -nostartfiles and sdr_glibc_225_compat.c so startup itself uses the glibc
 * 2.4 __libc_start_main ABI already proven on the target.
 */

#include <stdlib.h>

extern int main(int argc, char **argv);
extern int __wrap___libc_start_main(int (*main_fn)(int, char **, char **),
                                    int argc,
                                    char **argv,
                                    void (*init_fn)(void),
                                    void (*fini_fn)(void),
                                    void (*rtld_fini_fn)(void),
                                    void *stack_end);

__attribute__((naked, noreturn, section(".text.start")))
void _start(void)
{
    __asm__ volatile (
        "mov fp, #0\n"
        "mov lr, #0\n"
        "pop {r1}\n"
        "mov r2, sp\n"
        "push {r2}\n"
        "push {r0}\n"
        "mov r3, #0\n"
        "push {r3}\n"
        "ldr r0, =main\n"
        "bl __wrap___libc_start_main\n"
        "bl abort\n");
}
