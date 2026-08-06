/*
 * Import-only libdl ABI surface used while linking on glibc >= 2.34 hosts.
 * The resulting DSO is never deployed.  Its SONAME and symbol versions make
 * consumers request the real /lib/libdl.so.2 supplied by the Pluto image.
 */

void *dlopen(const char *path, int mode)
{
    (void)path;
    (void)mode;
    return (void *)0;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    return (void *)0;
}

int dlclose(void *handle)
{
    (void)handle;
    return -1;
}

char *dlerror(void)
{
    return (char *)0;
}
