/*
 * Link-time compatibility shims for the Pluto image's glibc 2.25 runtime.
 *
 * Newer ARM Linux cross toolchains select newer default symbol versions even
 * for APIs that have existed for years.  --wrap redirects those references
 * here, where the oldest compatible versions are selected explicitly.
 */

#include <stdarg.h>
#include <stdlib.h>

typedef int (*sdr_main_fn_t)(int, char **, char **);
typedef void (*sdr_init_fn_t)(void);

extern int sdr_libc_start_main_2_4(sdr_main_fn_t main_fn,
                                   int argc,
                                   char **argv,
                                   sdr_init_fn_t init_fn,
                                   sdr_init_fn_t fini_fn,
                                   sdr_init_fn_t rtld_fini_fn,
                                   void *stack_end);
extern unsigned long sdr_strtoul_2_4(const char *text,
                                    char **end,
                                    int base);
extern unsigned long long sdr_strtoull_2_4(const char *text,
                                          char **end,
                                          int base);
extern int sdr_fcntl_2_4(int fd, int command, ...);

__asm__(".symver sdr_libc_start_main_2_4,__libc_start_main@GLIBC_2.4");
__asm__(".symver sdr_strtoul_2_4,strtoul@GLIBC_2.4");
__asm__(".symver sdr_strtoull_2_4,strtoull@GLIBC_2.4");
__asm__(".symver sdr_fcntl_2_4,fcntl@GLIBC_2.4");

int __wrap___libc_start_main(sdr_main_fn_t main_fn,
                             int argc,
                             char **argv,
                             sdr_init_fn_t init_fn,
                             sdr_init_fn_t fini_fn,
                             sdr_init_fn_t rtld_fini_fn,
                             void *stack_end)
{
    return sdr_libc_start_main_2_4(main_fn,
                                  argc,
                                  argv,
                                  init_fn,
                                  fini_fn,
                                  rtld_fini_fn,
                                  stack_end);
}

unsigned long __wrap___isoc23_strtoul(const char *text, char **end, int base)
{
    return sdr_strtoul_2_4(text, end, base);
}

unsigned long long __wrap___isoc23_strtoull(const char *text,
                                            char **end,
                                            int base)
{
    return sdr_strtoull_2_4(text, end, base);
}

/* The capture agent always supplies the third integer argument for F_GETFL
 * and F_SETFL.  Keep that contract while selecting the pre-2.28 ABI. */
int sdr_fcntl_glibc_2_4(int fd, int command, ...)
{
    va_list arguments;
    int value;

    va_start(arguments, command);
    value = va_arg(arguments, int);
    va_end(arguments);
    return sdr_fcntl_2_4(fd, command, value);
}
