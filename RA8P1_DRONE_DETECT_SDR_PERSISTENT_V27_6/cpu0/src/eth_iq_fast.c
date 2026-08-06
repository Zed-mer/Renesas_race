/* Fixed-port IQ UDP sink on the RMAC fast path. */

#include <rtthread.h>
#include <hal_data.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_FEATURE_CRC32) && (__ARM_FEATURE_CRC32 > 0)
#include <arm_acle.h>
#define IQ_CRC32C_HAS_HW (1)
#else
#define IQ_CRC32C_HAS_HW (0)
#endif

#include "eth_iq_fast.h"
#include "framework/iq_protocol.h"
#include "framework/iq_ring.h"
#include "framework/rf_pipeline.h"
#include "framework/sdr_control_protocol.h"

#define ETH_HEADER_SIZE          (14U)
#define IPV4_MIN_HEADER_SIZE     (20U)
#define UDP_HEADER_SIZE          (8U)
#define IQ_HEADER_SIZE           (RA8P1_IQ_PACKET_HEADER_SIZE)
#define IQ_STATS_UPDATE_MASK     (0xFFU)
#define IQ_STATS_SCHEMA_VERSION  (ETH_IQ_FAST_STATS_SCHEMA_VERSION)
#define IPV4_FRAGMENT_MASK       (0x3FFFU)
#define IQ_CRC32C_SLICE_COUNT    (8U)
#define IQ_CRC32C_DTCM           __attribute__((section(".dtcm"), aligned(64)))
#define IQ_CRC32C_BACKEND_SOFTWARE  (1U)
#define IQ_CRC32C_BACKEND_RA8P1     (2U)
#define IQ_CRC32C_USE_RA8P1_DATA_PATH (1)
/* FSP crc_polynomial_t assigns selector 5 to CRC-32C. */
#define IQ_CRC32C_RA8P1_GPS         (5U)
#define IQ_CRC32C_SELF_TEST_EXPECTED (0x9F787F65U)
#define ETH_IQ_FAST_CONTROL_QUEUE_SLOTS (8U)
#define ETH_IQ_FAST_CONTROL_QUEUE_MASK  \
    (ETH_IQ_FAST_CONTROL_QUEUE_SLOTS - 1U)
#if defined(BSP_FEATURE_CRC_IS_AVAILABLE) && \
    (BSP_FEATURE_CRC_IS_AVAILABLE == 1U) && \
    defined(BSP_FEATURE_CRC_POLYNOMIAL_MASK) && \
    ((BSP_FEATURE_CRC_POLYNOMIAL_MASK & (1U << IQ_CRC32C_RA8P1_GPS)) != 0U) && \
    defined(BSP_FEATURE_CRC_HAS_CRCCR0_LMS) && \
    (BSP_FEATURE_CRC_HAS_CRCCR0_LMS == 1U)
#define IQ_CRC32C_HAS_RA8P1_PERIPHERAL (1)
#else
#define IQ_CRC32C_HAS_RA8P1_PERIPHERAL (0)
#endif
#if IQ_CRC32C_USE_RA8P1_DATA_PATH && !IQ_CRC32C_HAS_RA8P1_PERIPHERAL
#error "RA8P1 CRC32C data path requested without CRC-32C peripheral support"
#endif
#define IQ_ALL_FLAGS             (RA8P1_IQ_FLAG_SYNTHETIC | \
                                  RA8P1_IQ_FLAG_DISCONTINUITY | \
                                  RA8P1_IQ_FLAG_FREQUENCY_B | \
                                  RA8P1_IQ_FLAG_STREAM_START | \
                                  RA8P1_IQ_FLAG_STREAM_END | \
                                  RA8P1_IQ_FLAG_VALID_BITS_12 | \
                                  RA8P1_IQ_FLAG_WINDOW_CRC)

typedef enum e_iq_crc32c_phase
{
    IQ_CRC32C_PHASE_OFF = 0U,
    IQ_CRC32C_PHASE_RECEIVING,
    IQ_CRC32C_PHASE_END_PENDING,
    IQ_CRC32C_PHASE_VALID,
    IQ_CRC32C_PHASE_MISMATCH,
    IQ_CRC32C_PHASE_ABORTED
} iq_crc32c_phase_t;

static volatile uint32_t g_iq_crc32c_accumulator;
static volatile bool g_iq_crc32c_enabled;
static volatile bool g_iq_crc32c_ra8p1_active;
static volatile iq_crc32c_phase_t g_iq_crc32c_phase;
static volatile uint32_t g_iq_crc32c_session_id;
static volatile uint32_t g_iq_crc32c_consumed_packets;
static volatile uint32_t g_iq_crc32c_end_packets;
static volatile uint32_t g_iq_crc32c_expected;
static bool g_iq_crc32c_slicing_ready;
#if IQ_CRC32C_HAS_RA8P1_PERIPHERAL
static bool g_iq_crc32c_ra8p1_started;
static bool g_iq_crc32c_ra8p1_tested;
static bool g_iq_crc32c_ra8p1_self_test_ok;
static bool g_iq_crc32c_ra8p1_lsb_first;
#endif
static uint32_t g_iq_ring_full_baseline;
static uint32_t g_iq_ring_oversize_baseline;
static uint32_t g_iq_crc32c_slicing_table[IQ_CRC32C_SLICE_COUNT][256]
    IQ_CRC32C_DTCM;

/* Reflected Castagnoli polynomial.  RA8P1 has a native CRC-32C peripheral;
 * this table remains as the boot self-test oracle and software fallback. */
static const uint32_t g_iq_crc32c_byte_table[256] __attribute__((aligned(64))) =
{
    0x00000000U, 0xF26B8303U, 0xE13B70F7U, 0x1350F3F4U,
    0xC79A971FU, 0x35F1141CU, 0x26A1E7E8U, 0xD4CA64EBU,
    0x8AD958CFU, 0x78B2DBCCU, 0x6BE22838U, 0x9989AB3BU,
    0x4D43CFD0U, 0xBF284CD3U, 0xAC78BF27U, 0x5E133C24U,
    0x105EC76FU, 0xE235446CU, 0xF165B798U, 0x030E349BU,
    0xD7C45070U, 0x25AFD373U, 0x36FF2087U, 0xC494A384U,
    0x9A879FA0U, 0x68EC1CA3U, 0x7BBCEF57U, 0x89D76C54U,
    0x5D1D08BFU, 0xAF768BBCU, 0xBC267848U, 0x4E4DFB4BU,
    0x20BD8EDEU, 0xD2D60DDDU, 0xC186FE29U, 0x33ED7D2AU,
    0xE72719C1U, 0x154C9AC2U, 0x061C6936U, 0xF477EA35U,
    0xAA64D611U, 0x580F5512U, 0x4B5FA6E6U, 0xB93425E5U,
    0x6DFE410EU, 0x9F95C20DU, 0x8CC531F9U, 0x7EAEB2FAU,
    0x30E349B1U, 0xC288CAB2U, 0xD1D83946U, 0x23B3BA45U,
    0xF779DEAEU, 0x05125DADU, 0x1642AE59U, 0xE4292D5AU,
    0xBA3A117EU, 0x4851927DU, 0x5B016189U, 0xA96AE28AU,
    0x7DA08661U, 0x8FCB0562U, 0x9C9BF696U, 0x6EF07595U,
    0x417B1DBCU, 0xB3109EBFU, 0xA0406D4BU, 0x522BEE48U,
    0x86E18AA3U, 0x748A09A0U, 0x67DAFA54U, 0x95B17957U,
    0xCBA24573U, 0x39C9C670U, 0x2A993584U, 0xD8F2B687U,
    0x0C38D26CU, 0xFE53516FU, 0xED03A29BU, 0x1F682198U,
    0x5125DAD3U, 0xA34E59D0U, 0xB01EAA24U, 0x42752927U,
    0x96BF4DCCU, 0x64D4CECFU, 0x77843D3BU, 0x85EFBE38U,
    0xDBFC821CU, 0x2997011FU, 0x3AC7F2EBU, 0xC8AC71E8U,
    0x1C661503U, 0xEE0D9600U, 0xFD5D65F4U, 0x0F36E6F7U,
    0x61C69362U, 0x93AD1061U, 0x80FDE395U, 0x72966096U,
    0xA65C047DU, 0x5437877EU, 0x4767748AU, 0xB50CF789U,
    0xEB1FCBADU, 0x197448AEU, 0x0A24BB5AU, 0xF84F3859U,
    0x2C855CB2U, 0xDEEEDFB1U, 0xCDBE2C45U, 0x3FD5AF46U,
    0x7198540DU, 0x83F3D70EU, 0x90A324FAU, 0x62C8A7F9U,
    0xB602C312U, 0x44694011U, 0x5739B3E5U, 0xA55230E6U,
    0xFB410CC2U, 0x092A8FC1U, 0x1A7A7C35U, 0xE811FF36U,
    0x3CDB9BDDU, 0xCEB018DEU, 0xDDE0EB2AU, 0x2F8B6829U,
    0x82F63B78U, 0x709DB87BU, 0x63CD4B8FU, 0x91A6C88CU,
    0x456CAC67U, 0xB7072F64U, 0xA457DC90U, 0x563C5F93U,
    0x082F63B7U, 0xFA44E0B4U, 0xE9141340U, 0x1B7F9043U,
    0xCFB5F4A8U, 0x3DDE77ABU, 0x2E8E845FU, 0xDCE5075CU,
    0x92A8FC17U, 0x60C37F14U, 0x73938CE0U, 0x81F80FE3U,
    0x55326B08U, 0xA759E80BU, 0xB4091BFFU, 0x466298FCU,
    0x1871A4D8U, 0xEA1A27DBU, 0xF94AD42FU, 0x0B21572CU,
    0xDFEB33C7U, 0x2D80B0C4U, 0x3ED04330U, 0xCCBBC033U,
    0xA24BB5A6U, 0x502036A5U, 0x4370C551U, 0xB11B4652U,
    0x65D122B9U, 0x97BAA1BAU, 0x84EA524EU, 0x7681D14DU,
    0x2892ED69U, 0xDAF96E6AU, 0xC9A99D9EU, 0x3BC21E9DU,
    0xEF087A76U, 0x1D63F975U, 0x0E330A81U, 0xFC588982U,
    0xB21572C9U, 0x407EF1CAU, 0x532E023EU, 0xA145813DU,
    0x758FE5D6U, 0x87E466D5U, 0x94B49521U, 0x66DF1622U,
    0x38CC2A06U, 0xCAA7A905U, 0xD9F75AF1U, 0x2B9CD9F2U,
    0xFF56BD19U, 0x0D3D3E1AU, 0x1E6DCDEEU, 0xEC064EEDU,
    0xC38D26C4U, 0x31E6A5C7U, 0x22B65633U, 0xD0DDD530U,
    0x0417B1DBU, 0xF67C32D8U, 0xE52CC12CU, 0x1747422FU,
    0x49547E0BU, 0xBB3FFD08U, 0xA86F0EFCU, 0x5A048DFFU,
    0x8ECEE914U, 0x7CA56A17U, 0x6FF599E3U, 0x9D9E1AE0U,
    0xD3D3E1ABU, 0x21B862A8U, 0x32E8915CU, 0xC083125FU,
    0x144976B4U, 0xE622F5B7U, 0xF5720643U, 0x07198540U,
    0x590AB964U, 0xAB613A67U, 0xB831C993U, 0x4A5A4A90U,
    0x9E902E7BU, 0x6CFBAD78U, 0x7FAB5E8CU, 0x8DC0DD8FU,
    0xE330A81AU, 0x115B2B19U, 0x020BD8EDU, 0xF0605BEEU,
    0x24AA3F05U, 0xD6C1BC06U, 0xC5914FF2U, 0x37FACCF1U,
    0x69E9F0D5U, 0x9B8273D6U, 0x88D28022U, 0x7AB90321U,
    0xAE7367CAU, 0x5C18E4C9U, 0x4F48173DU, 0xBD23943EU,
    0xF36E6F75U, 0x0105EC76U, 0x12551F82U, 0xE03E9C81U,
    0x34F4F86AU, 0xC69F7B69U, 0xD5CF889DU, 0x27A40B9EU,
    0x79B737BAU, 0x8BDCB4B9U, 0x988C474DU, 0x6AE7C44EU,
    0xBE2DA0A5U, 0x4C4623A6U, 0x5F16D052U, 0xAD7D5351U
};

volatile eth_iq_fast_stats_t g_eth_iq_fast_stats
    __attribute__((section(".ram_nocache"), aligned(32))) =
{
    .magic = ETH_IQ_FAST_MAGIC,
    .schema_version = IQ_STATS_SCHEMA_VERSION
};

typedef struct st_eth_iq_fast_control_queue
{
    volatile uint32_t head;
    volatile uint32_t tail;
} eth_iq_fast_control_queue_t;

static eth_iq_fast_control_datagram_t
    g_eth_iq_fast_control_queue[ETH_IQ_FAST_CONTROL_QUEUE_SLOTS]
    __attribute__((section(".sdram_noinit"), aligned(32), used));
static eth_iq_fast_control_queue_t g_eth_iq_fast_control_queue_state
    __attribute__((section(".ram_nocache"), aligned(32)));

typedef char eth_iq_fast_control_queue_must_be_power_of_two[
    ((ETH_IQ_FAST_CONTROL_QUEUE_SLOTS &
      (ETH_IQ_FAST_CONTROL_QUEUE_SLOTS - 1U)) == 0U) ? 1 : -1];
typedef char eth_iq_fast_control_port_must_match_sdrc[
    (ETH_IQ_FAST_CONTROL_PORT == RA8P1_SDR_CONTROL_PORT) ? 1 : -1];
typedef char eth_iq_fast_control_wire_must_match_sdrc[
    (ETH_IQ_FAST_CONTROL_WIRE_BYTES ==
     RA8P1_SDR_CONTROL_WIRE_BYTES) ? 1 : -1];

static uint16_t iq_read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t iq_read_le32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint64_t iq_read_le64(const uint8_t *data)
{
    return ((uint64_t)iq_read_le32(&data[4]) << 32U) | iq_read_le32(data);
}

static void iq_fast_control_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

void eth_iq_fast_control_init(void)
{
    memset((void *)&g_eth_iq_fast_control_queue_state,
           0,
           sizeof(g_eth_iq_fast_control_queue_state));
    iq_fast_control_barrier();
}

static bool iq_fast_control_push(uint32_t source_address,
                                 uint16_t source_port,
                                 const uint8_t *wire,
                                 uint16_t length)
{
    uint32_t head;
    uint32_t tail;
    eth_iq_fast_control_datagram_t *slot;

    if ((wire == NULL) || (length != ETH_IQ_FAST_CONTROL_WIRE_BYTES))
    {
        return false;
    }
    head = g_eth_iq_fast_control_queue_state.head;
    tail = g_eth_iq_fast_control_queue_state.tail;
    if ((head - tail) >= ETH_IQ_FAST_CONTROL_QUEUE_SLOTS)
    {
        return false;
    }
    slot = &g_eth_iq_fast_control_queue[
        head & ETH_IQ_FAST_CONTROL_QUEUE_MASK];
    slot->source_address = source_address;
    slot->source_port = source_port;
    slot->length = length;
    memcpy(slot->wire, wire, length);
    iq_fast_control_barrier();
    g_eth_iq_fast_control_queue_state.head = head + 1U;
    return true;
}

bool eth_iq_fast_control_pop(eth_iq_fast_control_datagram_t *datagram)
{
    uint32_t tail;
    eth_iq_fast_control_datagram_t *slot;

    if (datagram == NULL)
    {
        return false;
    }
    tail = g_eth_iq_fast_control_queue_state.tail;
    if (tail == g_eth_iq_fast_control_queue_state.head)
    {
        return false;
    }
    iq_fast_control_barrier();
    slot = &g_eth_iq_fast_control_queue[
        tail & ETH_IQ_FAST_CONTROL_QUEUE_MASK];
    *datagram = *slot;
    iq_fast_control_barrier();
    g_eth_iq_fast_control_queue_state.tail = tail + 1U;
    return true;
}

static void iq_crc32c_prepare_slicing_table(void)
{
    uint32_t slice;
    uint32_t index;

    if (g_iq_crc32c_slicing_ready)
    {
        return;
    }
    memcpy(g_iq_crc32c_slicing_table[0],
           g_iq_crc32c_byte_table,
           sizeof(g_iq_crc32c_byte_table));
    for (slice = 1U; slice < IQ_CRC32C_SLICE_COUNT; slice++)
    {
        for (index = 0U; index < 256U; index++)
        {
            const uint32_t previous =
                g_iq_crc32c_slicing_table[slice - 1U][index];
            g_iq_crc32c_slicing_table[slice][index] =
                (previous >> 8U) ^
                g_iq_crc32c_slicing_table[0][previous & 0xFFU];
        }
    }
    g_iq_crc32c_slicing_ready = true;
}

static uint32_t iq_crc32c_reference_update(uint32_t crc,
                                           const uint8_t *data,
                                           uint32_t length)
{
    while (length != 0U)
    {
        crc = (crc >> 8U) ^ g_iq_crc32c_byte_table[(crc ^ *data++) & 0xFFU];
        length--;
    }
    return crc;
}

static uint32_t iq_crc32c_software_update(uint32_t crc,
                                          const uint8_t *data,
                                          uint32_t length)
{
    if ((data == NULL) || (length == 0U))
    {
        return crc;
    }
#if IQ_CRC32C_HAS_HW
    while (length >= 4U)
    {
        uint32_t word;
        memcpy(&word, data, sizeof(word));
        crc = __crc32cw(crc, word);
        data += 4U;
        length -= 4U;
    }
    while (length != 0U)
    {
        crc = __crc32cb(crc, *data++);
        length--;
    }
#else
    /* Reflected slicing-by-8 removes the byte-to-byte dependency chain.  The
     * tables are built once in DTCM so the RMAC receive thread can return each
     * descriptor before the next paced packet arrives. */
    while (length >= 8U)
    {
        uint32_t first;
        uint32_t second;
        memcpy(&first, data, sizeof(first));
        memcpy(&second, &data[4], sizeof(second));
        first ^= crc;
        crc = g_iq_crc32c_slicing_table[7][first & 0xFFU] ^
              g_iq_crc32c_slicing_table[6][(first >> 8U) & 0xFFU] ^
              g_iq_crc32c_slicing_table[5][(first >> 16U) & 0xFFU] ^
              g_iq_crc32c_slicing_table[4][first >> 24U] ^
              g_iq_crc32c_slicing_table[3][second & 0xFFU] ^
              g_iq_crc32c_slicing_table[2][(second >> 8U) & 0xFFU] ^
              g_iq_crc32c_slicing_table[1][(second >> 16U) & 0xFFU] ^
              g_iq_crc32c_slicing_table[0][second >> 24U];
        data += 8U;
        length -= 8U;
    }
    while (length != 0U)
    {
        crc = (crc >> 8U) ^ g_iq_crc32c_byte_table[(crc ^ *data++) & 0xFFU];
        length--;
    }
#endif
    return crc;
}

#if IQ_CRC32C_HAS_RA8P1_PERIPHERAL
static void iq_crc32c_ra8p1_reset(uint32_t seed, bool lsb_first)
{
    if (!g_iq_crc32c_ra8p1_started)
    {
        R_BSP_MODULE_START(FSP_IP_CRC, 0U);
        g_iq_crc32c_ra8p1_started = true;
    }
    R_CRC->CRCCR1 = 0U;
    R_CRC->CRCCR0 = (uint8_t)((IQ_CRC32C_RA8P1_GPS <<
                               R_CRC_CRCCR0_GPS_Pos) |
                              (lsb_first ? R_CRC_CRCCR0_LMS_Msk : 0U) |
                              R_CRC_CRCCR0_DORCLR_Msk);
    R_CRC->CRCDOR = seed;
}

static uint32_t iq_crc32c_ra8p1_update(const uint8_t *data,
                                       uint32_t length)
{
    while (length >= sizeof(uint32_t))
    {
        uint32_t word;
        memcpy(&word, data, sizeof(word));
        R_CRC->CRCDIR = word;
        data += sizeof(word);
        length -= sizeof(word);
    }
    return R_CRC->CRCDOR;
}

static bool iq_crc32c_ra8p1_prepare(void)
{
    static const uint8_t vector[8] __attribute__((aligned(4))) =
        { '1', '2', '3', '4', '5', '6', '7', '8' };
    if (!g_iq_crc32c_ra8p1_tested)
    {
        const uint32_t software = iq_crc32c_reference_update(
            RA8P1_IQ_CRC32C_INIT, vector, sizeof(vector));
        uint32_t hardware;

        /* The register reference exposes LMS but does not encode the CRC32C
         * reflection convention in the generated headers.  Probe both modes
         * against the byte-at-a-time reference instead of assuming one. */
        iq_crc32c_ra8p1_reset(RA8P1_IQ_CRC32C_INIT, true);
        hardware = iq_crc32c_ra8p1_update(vector, sizeof(vector));
        g_iq_crc32c_ra8p1_lsb_first =
            hardware == IQ_CRC32C_SELF_TEST_EXPECTED;
        if (!g_iq_crc32c_ra8p1_lsb_first)
        {
            iq_crc32c_ra8p1_reset(RA8P1_IQ_CRC32C_INIT, false);
            hardware = iq_crc32c_ra8p1_update(vector, sizeof(vector));
        }
        g_iq_crc32c_ra8p1_self_test_ok =
            (hardware == IQ_CRC32C_SELF_TEST_EXPECTED) &&
            (software == IQ_CRC32C_SELF_TEST_EXPECTED);
        g_iq_crc32c_ra8p1_tested = true;
    }
    iq_crc32c_ra8p1_reset(RA8P1_IQ_CRC32C_INIT,
                          g_iq_crc32c_ra8p1_lsb_first);
    return g_iq_crc32c_ra8p1_self_test_ok;
}
#endif

static bool iq_cpu0_cycle_now(uint32_t *cycles)
{
    const bool enabled =
        ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
        ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
    if (enabled && (cycles != NULL))
    {
        *cycles = DWT->CYCCNT;
    }
    return enabled;
}

static uint32_t iq_crc32c_update(uint32_t crc,
                                 const uint8_t *data,
                                 uint32_t length)
{
    uint32_t start_cycles = 0U;
    uint32_t elapsed_cycles = 0U;
    uint32_t result;
    const bool dwt_enabled = iq_cpu0_cycle_now(&start_cycles);

    if ((data == NULL) || (length == 0U))
    {
        return crc;
    }
#if IQ_CRC32C_HAS_RA8P1_PERIPHERAL
    if (g_iq_crc32c_ra8p1_active && ((length & 3U) == 0U))
    {
        result = iq_crc32c_ra8p1_update(data, length);
    }
    else
#endif
    {
        result = iq_crc32c_software_update(crc, data, length);
    }
    if (dwt_enabled)
    {
        elapsed_cycles = DWT->CYCCNT - start_cycles;
        g_eth_iq_fast_stats.crc_cycles_total += elapsed_cycles;
        if (elapsed_cycles > g_eth_iq_fast_stats.crc_cycles_max)
        {
            g_eth_iq_fast_stats.crc_cycles_max = elapsed_cycles;
        }
    }
    g_eth_iq_fast_stats.crc_updates++;
    return result;
}

static uint32_t iq_crc32c_final(void)
{
    return g_iq_crc32c_accumulator ^ RA8P1_IQ_CRC32C_XOROUT;
}

static void iq_crc32c_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static void iq_crc32c_abort(void)
{
    if (!g_iq_crc32c_enabled)
    {
        return;
    }
    if ((g_iq_crc32c_phase != IQ_CRC32C_PHASE_ABORTED) &&
        (g_iq_crc32c_phase != IQ_CRC32C_PHASE_MISMATCH))
    {
        g_eth_iq_fast_stats.crc_errors++;
    }
    g_eth_iq_fast_stats.crc_flags = RA8P1_IQ_ACK_FLAG_CRC_PRESENT;
    g_iq_crc32c_phase = IQ_CRC32C_PHASE_ABORTED;
    iq_crc32c_barrier();
    g_eth_iq_fast_stats.active = 0U;
}

static void iq_crc32c_try_complete(void)
{
    uint32_t actual;
    uint32_t complete_cycles;

    if (!g_iq_crc32c_enabled ||
        (g_iq_crc32c_phase != IQ_CRC32C_PHASE_END_PENDING) ||
        (g_iq_crc32c_consumed_packets != g_iq_crc32c_end_packets))
    {
        return;
    }

    actual = iq_crc32c_final();
    g_eth_iq_fast_stats.crc32c = actual;
    g_eth_iq_fast_stats.expected_crc32c = g_iq_crc32c_expected;
    if (actual == g_iq_crc32c_expected)
    {
        g_eth_iq_fast_stats.crc_flags =
            RA8P1_IQ_ACK_FLAG_CRC_PRESENT |
            RA8P1_IQ_ACK_FLAG_CRC_VALID;
        g_iq_crc32c_phase = IQ_CRC32C_PHASE_VALID;
    }
    else
    {
        g_eth_iq_fast_stats.crc_errors++;
        g_eth_iq_fast_stats.crc_flags = RA8P1_IQ_ACK_FLAG_CRC_PRESENT;
        g_iq_crc32c_phase = IQ_CRC32C_PHASE_MISMATCH;
    }

    if (iq_cpu0_cycle_now(&complete_cycles))
    {
        g_eth_iq_fast_stats.crc_complete_cpu0_cycles = complete_cycles;
        g_eth_iq_fast_stats.crc_timing_flags |=
            ETH_IQ_FAST_CRC_TIMING_COMPLETE_VALID;
        if ((g_eth_iq_fast_stats.crc_timing_flags &
             ETH_IQ_FAST_CRC_TIMING_END_VALID) != 0U)
        {
            g_eth_iq_fast_stats.crc_after_end_cycles =
                complete_cycles - g_eth_iq_fast_stats.end_packet_cpu0_cycles;
        }
    }

    /* ACK observes active before it interprets the CRC fields.  Publish every
     * terminal field first, then clear active as the final ownership handoff. */
    iq_crc32c_barrier();
    g_eth_iq_fast_stats.active = 0U;
}

void eth_iq_fast_crc_consume(uint32_t session_id,
                             const uint8_t *data,
                             uint32_t length)
{
    iq_crc32c_phase_t phase;

    if (!g_iq_crc32c_enabled)
    {
        return;
    }
    phase = g_iq_crc32c_phase;
    if ((data == NULL) || (length == 0U) || ((length & 3U) != 0U) ||
        (session_id != g_iq_crc32c_session_id) ||
        ((phase != IQ_CRC32C_PHASE_RECEIVING) &&
         (phase != IQ_CRC32C_PHASE_END_PENDING)) ||
        ((phase == IQ_CRC32C_PHASE_END_PENDING) &&
         (g_iq_crc32c_consumed_packets >= g_iq_crc32c_end_packets)))
    {
        g_eth_iq_fast_stats.invalid++;
        iq_crc32c_abort();
        return;
    }

    g_iq_crc32c_accumulator = iq_crc32c_update(g_iq_crc32c_accumulator,
                                               data,
                                               length);
    g_eth_iq_fast_stats.crc32c = iq_crc32c_final();
    g_iq_crc32c_consumed_packets++;
    iq_crc32c_barrier();
    iq_crc32c_try_complete();
}

static void iq_reset_stats(uint32_t tick, uint32_t session_id)
{
    iq_ring_stats_t ring;
    iq_ring_stats_get(&ring);
    g_iq_ring_full_baseline = ring.full_drops;
    g_iq_ring_oversize_baseline = ring.oversize_drops;
    memset((void *)&g_eth_iq_fast_stats, 0, sizeof(g_eth_iq_fast_stats));
    g_eth_iq_fast_stats.magic = ETH_IQ_FAST_MAGIC;
    g_eth_iq_fast_stats.schema_version = IQ_STATS_SCHEMA_VERSION;
    g_eth_iq_fast_stats.active = 1U;
    g_eth_iq_fast_stats.first_tick = tick;
    g_eth_iq_fast_stats.last_tick = tick;
    g_eth_iq_fast_stats.next_sequence = 1U;
    g_eth_iq_fast_stats.session_id = session_id;
    g_iq_crc32c_accumulator = RA8P1_IQ_CRC32C_INIT;
    g_iq_crc32c_enabled = false;
    g_iq_crc32c_ra8p1_active = false;
    g_iq_crc32c_phase = IQ_CRC32C_PHASE_OFF;
    g_iq_crc32c_session_id = session_id;
    g_iq_crc32c_consumed_packets = 0U;
    g_iq_crc32c_end_packets = 0U;
    g_iq_crc32c_expected = 0U;
}

void eth_iq_fast_session_ring_drops(uint32_t *full_drops,
                                    uint32_t *oversize_drops)
{
    iq_ring_stats_t ring;
    iq_ring_stats_get(&ring);
    if (full_drops != NULL)
    {
        *full_drops = ring.full_drops - g_iq_ring_full_baseline;
    }
    if (oversize_drops != NULL)
    {
        *oversize_drops = ring.oversize_drops - g_iq_ring_oversize_baseline;
    }
}

static bool iq_accept_sequence(uint32_t sequence, uint32_t *flags)
{
    const uint32_t expected = g_eth_iq_fast_stats.next_sequence;
    const int32_t delta = (int32_t)(sequence - expected);
    if (delta < 0)
    {
        g_eth_iq_fast_stats.reordered++;
        g_eth_iq_fast_stats.invalid++;
        return false;
    }
    if (delta > 0)
    {
        g_eth_iq_fast_stats.sequence_gaps += (uint32_t)delta;
        if (flags != NULL) *flags |= RA8P1_IQ_FLAG_DISCONTINUITY;
    }
    g_eth_iq_fast_stats.next_sequence = sequence + 1U;
    return true;
}

static void iq_update_rate(uint32_t tick)
{
    g_eth_iq_fast_stats.last_tick = tick;
    g_eth_iq_fast_stats.elapsed_ms =
        (uint32_t)(((uint64_t)(tick - g_eth_iq_fast_stats.first_tick) * 1000U) /
                   RT_TICK_PER_SECOND);
    if (g_eth_iq_fast_stats.elapsed_ms > 0U)
    {
        g_eth_iq_fast_stats.mbps_x1000 =
            (uint32_t)((g_eth_iq_fast_stats.payload_bytes * 8U) /
                       g_eth_iq_fast_stats.elapsed_ms);
    }
}

static bool iq_control_config_valid(const ra8p1_iq_stream_config_t *config,
                                    uint32_t header_session_id,
                                    uint32_t header_format)
{
    return (config != NULL) &&
           (config->magic == RA8P1_IQ_STREAM_CONFIG_MAGIC) &&
           (config->version == RA8P1_IQ_STREAM_CONFIG_VERSION) &&
           (config->size == sizeof(*config)) &&
           (config->session_id == header_session_id) &&
           (config->format == header_format);
}

int eth_iq_fast_consume(const uint8_t *frame, uint32_t frame_length)
{
    uint32_t ip_header_size;
    uint32_t ip_total_length;
    uint32_t udp_offset;
    uint32_t udp_length;
    uint16_t udp_destination_port;
    uint32_t iq_length;
    uint32_t sequence;
    uint32_t data_length;
    uint32_t format;
    uint32_t flags;
    uint32_t session_id;
    uint64_t sample_index;
    const uint8_t *iq_header;
    const uint8_t *iq_data;

    if ((frame == NULL) ||
        (frame_length < (ETH_HEADER_SIZE + IPV4_MIN_HEADER_SIZE +
                         UDP_HEADER_SIZE)) ||
        (frame[12] != 0x08U) || (frame[13] != 0x00U) ||
        ((frame[14] >> 4U) != 4U) || (frame[23] != 17U))
    {
        return 0;
    }
    ip_header_size = (uint32_t)(frame[14] & 0x0FU) * 4U;
    ip_total_length = iq_read_be16(&frame[16]);
    if ((ip_header_size < IPV4_MIN_HEADER_SIZE) ||
        ((iq_read_be16(&frame[20]) & IPV4_FRAGMENT_MASK) != 0U) ||
        (ip_total_length < (ip_header_size + UDP_HEADER_SIZE)) ||
        (frame_length < (ETH_HEADER_SIZE + ip_total_length)))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    udp_offset = ETH_HEADER_SIZE + ip_header_size;
    udp_destination_port = iq_read_be16(&frame[udp_offset + 2U]);
    udp_length = iq_read_be16(&frame[udp_offset + 4U]);
    if ((udp_length < UDP_HEADER_SIZE) ||
        (udp_length > (ip_total_length - ip_header_size)) ||
        ((udp_offset + udp_length) > frame_length))
    {
        if ((udp_destination_port == ETH_IQ_FAST_PORT) ||
            (udp_destination_port == ETH_IQ_FAST_CONTROL_PORT))
        {
            g_eth_iq_fast_stats.invalid++;
            return 1;
        }
        return 0;
    }
    if (udp_destination_port == ETH_IQ_FAST_CONTROL_PORT)
    {
        uint32_t source_address;
        uint16_t source_port;
        if (udp_length !=
            (UDP_HEADER_SIZE + ETH_IQ_FAST_CONTROL_WIRE_BYTES))
        {
            return 1;
        }
        memcpy(&source_address, &frame[26], sizeof(source_address));
        memcpy(&source_port, &frame[udp_offset], sizeof(source_port));
        /* If the bounded mailbox is momentarily full, leave the datagram on
         * the established lwIP path rather than dropping a control response. */
        if (iq_fast_control_push(source_address,
                                 source_port,
                                 &frame[udp_offset + UDP_HEADER_SIZE],
                                 ETH_IQ_FAST_CONTROL_WIRE_BYTES))
        {
            return 1;
        }
        return 0;
    }
    if (udp_destination_port != ETH_IQ_FAST_PORT)
    {
        return 0;
    }
    if (udp_length < (UDP_HEADER_SIZE + IQ_HEADER_SIZE))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    iq_header = &frame[udp_offset + UDP_HEADER_SIZE];
    iq_length = udp_length - UDP_HEADER_SIZE;
    if (iq_read_le32(&iq_header[0]) != RA8P1_IQ_PACKET_MAGIC)
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    sequence = iq_read_le32(&iq_header[4]);
    data_length = iq_read_le32(&iq_header[8]);
    flags = iq_read_le32(&iq_header[12]);
    sample_index = iq_read_le64(&iq_header[16]);
    session_id = iq_read_le32(&iq_header[24]);
    format = iq_read_le32(&iq_header[28]);
    if ((session_id == 0U) ||
        ((flags & ~IQ_ALL_FLAGS) != 0U) ||
        (data_length != (iq_length - IQ_HEADER_SIZE)) ||
        (format != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED) ||
        ((data_length & 3U) != 0U))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    iq_data = &iq_header[IQ_HEADER_SIZE];

    if ((flags & RA8P1_IQ_FLAG_STREAM_START) != 0U)
    {
        ra8p1_iq_stream_config_t config;
        iq_ring_stats_t ring;
        if (((flags & RA8P1_IQ_FLAG_STREAM_END) != 0U) ||
            (sequence != 0U) || (sample_index != 0U) ||
            (data_length != sizeof(config)))
        {
            g_eth_iq_fast_stats.invalid++;
            return 1;
        }
        memcpy(&config, iq_data, sizeof(config));
        iq_ring_stats_get(&ring);
        if (!iq_control_config_valid(&config, session_id, format) ||
            ((flags & ~RA8P1_IQ_FLAG_STREAM_START) != config.flags) ||
            (ring.queued != 0U) ||
            !rf_pipeline_stream_configure(&config))
        {
            g_eth_iq_fast_stats.invalid++;
            return 1;
        }
        /* The control plane binds the only session allowed here.  Let that
         * authorized START replace a stale partial window after cancel/error. */
        iq_reset_stats((uint32_t)rt_tick_get(), session_id);
        g_iq_crc32c_enabled =
            (config.flags & RA8P1_IQ_FLAG_WINDOW_CRC) != 0U;
        if (g_iq_crc32c_enabled)
        {
#if IQ_CRC32C_HAS_RA8P1_PERIPHERAL
            g_iq_crc32c_ra8p1_active = iq_crc32c_ra8p1_prepare();
            g_eth_iq_fast_stats.crc_hw_self_test =
                g_iq_crc32c_ra8p1_active ? 1U : 0U;
            g_iq_crc32c_ra8p1_active =
                g_iq_crc32c_ra8p1_active &&
                (IQ_CRC32C_USE_RA8P1_DATA_PATH != 0);
#endif
            if (g_iq_crc32c_ra8p1_active)
            {
                g_eth_iq_fast_stats.crc_backend = IQ_CRC32C_BACKEND_RA8P1;
            }
            else
            {
                iq_crc32c_prepare_slicing_table();
                g_eth_iq_fast_stats.crc_backend = IQ_CRC32C_BACKEND_SOFTWARE;
            }
            g_eth_iq_fast_stats.crc_flags = RA8P1_IQ_ACK_FLAG_CRC_PRESENT;
            g_iq_crc32c_phase = IQ_CRC32C_PHASE_RECEIVING;
            iq_crc32c_barrier();
        }
        g_eth_iq_fast_stats.flags = flags;
        return 1;
    }

    if ((g_eth_iq_fast_stats.active == 0U) ||
        (session_id != g_eth_iq_fast_stats.session_id))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    if (!iq_accept_sequence(sequence, &flags)) return 1;

    if ((flags & RA8P1_IQ_FLAG_STREAM_END) != 0U)
    {
        ra8p1_iq_stream_config_t config;
        const uint32_t expected_length = sizeof(config) +
            (g_iq_crc32c_enabled ? RA8P1_IQ_CRC32C_TRAILER_BYTES : 0U);
        uint32_t expected_crc = 0U;
        if ((data_length != expected_length) ||
            (((flags & RA8P1_IQ_FLAG_WINDOW_CRC) != 0U) != g_iq_crc32c_enabled))
        {
            g_eth_iq_fast_stats.invalid++;
            if (g_iq_crc32c_enabled)
            {
                iq_crc32c_abort();
            }
            return 1;
        }
        memcpy(&config, iq_data, sizeof(config));
        if (g_iq_crc32c_enabled)
        {
            expected_crc = iq_read_le32(&iq_data[sizeof(config)]);
            g_eth_iq_fast_stats.expected_crc32c = expected_crc;
        }
        if (!iq_control_config_valid(&config, session_id, format) ||
            (sample_index != (((uint64_t)config.total_samples_high << 32U) |
                              config.total_samples_low)) ||
            /* The CRC bit is part of the stream configuration and must be
             * retained on END.  DISCONTINUITY is the only receiver-side
             * annotation that may be carried on an END marker. */
            ((flags & ~(RA8P1_IQ_FLAG_STREAM_END |
                        RA8P1_IQ_FLAG_DISCONTINUITY)) != config.flags) ||
            !rf_pipeline_stream_end(&config))
        {
            g_eth_iq_fast_stats.invalid++;
            if (g_iq_crc32c_enabled)
            {
                iq_crc32c_abort();
            }
            return 1;
        }
        {
            uint32_t end_cycles;
            if (iq_cpu0_cycle_now(&end_cycles))
            {
                g_eth_iq_fast_stats.end_packet_cpu0_cycles = end_cycles;
                g_eth_iq_fast_stats.crc_timing_flags |=
                    ETH_IQ_FAST_CRC_TIMING_END_VALID;
            }
        }
        iq_update_rate((uint32_t)rt_tick_get());
        g_eth_iq_fast_stats.flags = flags;
        if (g_iq_crc32c_enabled)
        {
            g_iq_crc32c_expected = expected_crc;
            g_iq_crc32c_end_packets = g_eth_iq_fast_stats.packets;
            iq_crc32c_barrier();
            g_iq_crc32c_phase = IQ_CRC32C_PHASE_END_PENDING;
            iq_crc32c_barrier();
            iq_crc32c_try_complete();
        }
        else
        {
            g_eth_iq_fast_stats.active = 0U;
        }
        return 1;
    }

    if ((flags & (RA8P1_IQ_FLAG_STREAM_START | RA8P1_IQ_FLAG_STREAM_END |
                  RA8P1_IQ_FLAG_FREQUENCY_B)) != 0U)
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    if (((flags & RA8P1_IQ_FLAG_WINDOW_CRC) != 0U) != g_iq_crc32c_enabled)
    {
        g_eth_iq_fast_stats.invalid++;
        if (g_iq_crc32c_enabled)
        {
            g_eth_iq_fast_stats.crc_errors++;
        }
        return 1;
    }
    if (!rf_pipeline_ingest(iq_data,
                            data_length,
                            sequence,
                            flags,
                            session_id,
                            sample_index,
                            format))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }
    g_eth_iq_fast_stats.packets++;
    g_eth_iq_fast_stats.payload_bytes += data_length;
    if (data_length >= sizeof(uint32_t))
    {
        g_eth_iq_fast_stats.data_checksum =
            ((g_eth_iq_fast_stats.data_checksum << 1U) |
             (g_eth_iq_fast_stats.data_checksum >> 31U)) ^ iq_read_le32(iq_data);
    }
    if ((g_eth_iq_fast_stats.packets & IQ_STATS_UPDATE_MASK) == 0U)
    {
        iq_update_rate((uint32_t)rt_tick_get());
    }
    g_eth_iq_fast_stats.flags = flags;
    return 1;
}
