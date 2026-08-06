/*
 * Import-only pthread ABI surface used while linking on glibc >= 2.34 hosts.
 * This DSO is never deployed.  Its SONAME and GLIBC_2.4 symbol versions make
 * consumers request the real /lib/libpthread.so.0 from the Pluto image.
 */

#include <pthread.h>

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*entry)(void *), void *argument)
{
    (void)thread;
    (void)attributes;
    (void)entry;
    (void)argument;
    return -1;
}

int pthread_join(pthread_t thread, void **result)
{
    (void)thread;
    (void)result;
    return -1;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attributes)
{
    (void)mutex;
    (void)attributes;
    return -1;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return -1;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return -1;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return -1;
}

int pthread_cond_init(pthread_cond_t *condition,
                      const pthread_condattr_t *attributes)
{
    (void)condition;
    (void)attributes;
    return -1;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
    (void)condition;
    return -1;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    (void)condition;
    (void)mutex;
    return -1;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
    (void)condition;
    return -1;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
    (void)condition;
    return -1;
}
