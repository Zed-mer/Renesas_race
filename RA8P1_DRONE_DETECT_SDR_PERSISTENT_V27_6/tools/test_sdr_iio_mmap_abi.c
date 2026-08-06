/*
 * Host-only ABI smoke test for sdr_adapter_iio_mmap.so.
 *
 * The test deliberately does not call api.open(): passing here proves that
 * the shared object loads and exposes the expected table, not that an SDR's
 * IIO block/mmap implementation has been validated.
 */

#include "sdr_adapter.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    (void)fprintf(stderr, "iio_mmap ABI check failed: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    void *module;
    void *symbol;
    ra8p1_sdr_adapter_get_api_fn get_api = NULL;
    ra8p1_sdr_adapter_api_t api;
    const char *loader_error;
    int32_t status;

    if (argc != 2)
    {
        (void)fprintf(stderr, "usage: %s <sdr_adapter_iio_mmap.so>\n",
                      argv[0]);
        return 2;
    }

    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == NULL)
    {
        loader_error = dlerror();
        return fail(loader_error != NULL ? loader_error : "dlopen failed");
    }

    (void)dlerror();
    symbol = dlsym(module, RA8P1_SDR_ADAPTER_GET_API_SYMBOL);
    loader_error = dlerror();
    if ((loader_error != NULL) || (symbol == NULL))
    {
        (void)dlclose(module);
        return fail(loader_error != NULL ? loader_error : "API symbol missing");
    }
    if (sizeof(get_api) != sizeof(symbol))
    {
        (void)dlclose(module);
        return fail("function and object pointer sizes differ");
    }
    memcpy(&get_api, &symbol, sizeof(get_api));

    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    status = get_api(RA8P1_SDR_ADAPTER_ABI_VERSION, &api);
    if (status != 0)
    {
        (void)dlclose(module);
        return fail("API entry point rejected ABI v1");
    }
    if ((api.struct_size < RA8P1_SDR_ADAPTER_V1_STATUS_SIZE) ||
        (api.abi_version != RA8P1_SDR_ADAPTER_ABI_VERSION) ||
        (api.capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2) ||
        (api.sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES))
    {
        (void)dlclose(module);
        return fail("API metadata does not match the capture-agent ABI");
    }
    if ((api.name == NULL) || (api.name[0] == '\0') ||
        (api.open == NULL) || (api.set_rx == NULL) ||
        (api.rx_capture == NULL) || (api.close == NULL) ||
        (api.rx1_capture_le == NULL) || (api.get_status == NULL))
    {
        (void)dlclose(module);
        return fail("API table is missing a required callback");
    }

    (void)printf("iio_mmap host ABI table validated: %s\n", api.name);
    if (dlclose(module) != 0)
    {
        return fail("dlclose failed");
    }
    return 0;
}
