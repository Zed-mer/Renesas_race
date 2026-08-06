#include "rf_pipeline.h"

#include <netdev_ipaddr.h>
#include <netdev.h>
#include <rtthread.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "../eth_iq_fast.h"
#include "analysis_pipeline.h"
#include "cpu0_trace.h"
#include "display_stream.h"
#include "ipc_bridge.h"
#include "iq_ring.h"
#include "npu_runner.h"
#include "rf_v12_sparse_contract.h"
#include "sdr_control_client.h"
#include "system_protocol.h"

#define RF_PIPELINE_THREAD_PRIORITY      (6U)
/* GCC 13.2 -O2 -fstack-usage gives a 5432-byte known production maximum:
 * rfpipe(704) + ingest(1088) + feed(80) + publish(2928) + IPC(632).
 * 12288 bytes leaves 6856 bytes for RTOS context, interrupts and future
 * compiler drift.  The same worker runs the one-shot boot proof. */
#define RF_PIPELINE_THREAD_STACK         (12288U)
#define RF_PIPELINE_PUBLISH_MS           (100U)
#define RF_PIPELINE_LOW_WATERMARK        (IQ_RING_SLOT_COUNT / 4U)
#define RF_PIPELINE_HIGH_WATERMARK       ((IQ_RING_SLOT_COUNT * 3U) / 4U)
#define RF_PIPELINE_SLICE_BLOCKS_LOW     (128U)
#define RF_PIPELINE_SLICE_BLOCKS_MID     (192U)
#define RF_PIPELINE_SLICE_BLOCKS_HIGH    (256U)
#define RF_PIPELINE_END_QUIET_MS          (20U)
#define RF_PIPELINE_END_GRACE_MS          (100U)
#define RF_SDR_CONTROL_RX_BUDGET          (32U)
#define RF_SDR_CONTROL_SOCKET_BUFFER      (64 * 1024)
#define RF_STREAM_CONFIG_ALLOWED_FLAGS   (RA8P1_IQ_FLAG_SYNTHETIC | \
                                          RA8P1_IQ_FLAG_VALID_BITS_12 | \
                                          RA8P1_IQ_FLAG_WINDOW_CRC)
#define RF_DATA_ALLOWED_FLAGS            (RA8P1_IQ_FLAG_SYNTHETIC | \
                                          RA8P1_IQ_FLAG_DISCONTINUITY | \
                                          RA8P1_IQ_FLAG_VALID_BITS_12 | \
                                          RA8P1_IQ_FLAG_WINDOW_CRC)
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT                     (0x08)
#endif

/* IQSC START is consumed by the priority-5 Ethernet RX thread.  It may
 * preempt rfpipe (priority 6), while rfpipe cannot preempt a reader already
 * evaluating the expected tuple.  Keep that ordering explicit: the local IRQ
 * publication below is sufficient only for this single-core priority model. */
#if defined(RT_USING_SMP)
#error "SDR expected tuple publication requires the CPU0 single-core RTOS"
#endif
#if !defined(RT_LWIP_ETHTHREAD_PRIORITY)
#error "SDR expected tuple publication requires a pinned Ethernet RX priority"
#elif (RT_LWIP_ETHTHREAD_PRIORITY >= RF_PIPELINE_THREAD_PRIORITY)
#error "Ethernet RX must outrank rfpipe for atomic expected tuple reads"
#endif

static struct rt_thread g_rf_pipeline_thread;
static uint8_t g_rf_pipeline_stack[RF_PIPELINE_THREAD_STACK]
    __attribute__((section(".dtcm"), aligned(8)));
static volatile uint32_t g_processed_blocks;
static volatile uint32_t g_last_format = RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED;
static volatile uint32_t g_requested_sample_rate_hz = ANALYSIS_DEFAULT_SAMPLE_RATE;
static volatile uint32_t g_requested_channel_mask = RA8P1_RF_CHANNEL_A_MASK;
static volatile uint32_t g_command_sequence;
static volatile uint32_t g_command_status = RA8P1_COMMAND_NONE;
static volatile uint32_t g_command_reason = RA8P1_COMMAND_REASON_NONE;
static volatile uint32_t g_start_command_sequence;
static volatile uint32_t g_stop_command_sequence;
static volatile uint32_t g_applied_session_id;
static ra8p1_ui_command_t g_pending_command;
static volatile uint32_t g_real_stream_seen;
static volatile uint32_t g_stream_config_pending;
static volatile uint32_t g_stream_end_pending;
static volatile uint32_t g_stream_rx_active;
static volatile uint32_t g_stream_rx_session_id;
static volatile uint32_t g_stream_active_session_id;
static volatile uint64_t g_stream_expected_samples;
static volatile uint64_t g_stream_received_frontier;
static volatile uint32_t g_stream_discontinuity_seen;
static volatile uint32_t g_stream_valid;
static volatile uint32_t g_stream_last_ingress_tick;
static volatile uint32_t g_stream_end_tick;
static volatile uint32_t g_stream_end_session_id;
static ra8p1_iq_stream_config_t g_pending_stream_config;
static volatile uint32_t g_last_publish_tick;
static sdr_control_client_t g_sdr_control_client;
static int g_sdr_control_socket = -1;
static struct sockaddr_in g_sdr_control_peer;
static volatile uint32_t g_sdr_expected_session_id;
static volatile uint32_t g_sdr_expected_center_index;
static volatile uint32_t g_sdr_expected_sample_count;

volatile sdr_control_client_stats_t g_sdr_control_stats
    __attribute__((section(".ram_nocache"), aligned(32)));
volatile rf_pipeline_stack_proof_t g_rf_pipeline_stack_proof
    __attribute__((section(".ram_nocache"), aligned(32)));
static volatile uint32_t g_trace_ingress_session_id;
static volatile uint32_t g_trace_first_packet_cycles;
static volatile uint32_t g_trace_last_packet_cycles;
static volatile uint32_t g_trace_first_packet_valid;
static volatile uint32_t g_trace_last_packet_valid;
static volatile uint32_t g_trace_iqsc_start_session_id;
static volatile uint32_t g_trace_iqsc_start_cycles;
static volatile uint32_t g_trace_iqsc_start_valid;
static uint32_t g_trace_ingress_published_session_id;
static uint32_t g_trace_crc_published_session_id;
static uint32_t g_stack_last_window_count;

static void rf_pipeline_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static void rf_pipeline_sdr_expected_publish(uint32_t center_index,
                                             uint32_t sample_count,
                                             uint32_t session_id)
{
    const rt_base_t irq_level = rt_hw_interrupt_disable();

    g_sdr_expected_center_index = center_index;
    g_sdr_expected_sample_count = sample_count;
    g_sdr_expected_session_id = session_id;
    rf_pipeline_barrier();
    rt_hw_interrupt_enable(irq_level);
}

static void rf_pipeline_sdr_expected_sync(void);

static void rf_pipeline_stack_proof_update(uint32_t windows_completed)
{
    const volatile uint8_t *stack = g_rf_pipeline_stack;
    uint32_t free_bytes = 0U;
    uint32_t used_bytes;

    while ((free_bytes < sizeof(g_rf_pipeline_stack)) &&
           (stack[free_bytes] == (uint8_t)'#'))
    {
        free_bytes++;
    }
    used_bytes = (uint32_t)sizeof(g_rf_pipeline_stack) - free_bytes;
    if ((g_rf_pipeline_stack_proof.observations == 0U) ||
        (used_bytes > g_rf_pipeline_stack_proof.used_high_water_bytes))
    {
        g_rf_pipeline_stack_proof.used_high_water_bytes = used_bytes;
    }
    if ((g_rf_pipeline_stack_proof.observations == 0U) ||
        (free_bytes < g_rf_pipeline_stack_proof.free_low_water_bytes))
    {
        g_rf_pipeline_stack_proof.free_low_water_bytes = free_bytes;
    }
    g_rf_pipeline_stack_proof.observations++;
    g_rf_pipeline_stack_proof.windows_completed = windows_completed;
    rf_pipeline_barrier();
    g_rf_pipeline_stack_proof.completion_magic =
        RF_PIPELINE_STACK_PROOF_DONE_MAGIC;
}

static uint32_t rf_pipeline_now_ms(void)
{
    return (uint32_t)(((uint64_t)(uint32_t)rt_tick_get() * 1000U) /
                      RT_TICK_PER_SECOND);
}

static bool rf_pipeline_network_ready(void)
{
    struct netdev *netdev = netdev_default;

    if ((netdev == RT_NULL) || !netdev_is_up(netdev) ||
        !netdev_is_link_up(netdev))
    {
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP |
                                           NETDEV_FLAG_LINK_UP);
    }
    return (netdev != RT_NULL) && netdev_is_up(netdev) &&
           netdev_is_link_up(netdev);
}

static bool rf_pipeline_sdr_control_send(void *context,
                                         const uint8_t *data,
                                         size_t length)
{
    int socket_fd = *(const int *)context;
    int sent;
    if ((socket_fd < 0) || (data == NULL) ||
        (length != RA8P1_SDR_CONTROL_WIRE_BYTES))
    {
        return false;
    }
    /* The SDR may consume WINDOW_ACK credit and return IQSC START before
     * sendto() returns.  Publish the client state at the transport boundary so
     * the RMAC callback validates that START against the prefetched session,
     * never the just-completed active session. */
    rf_pipeline_sdr_expected_sync();
    sent = sendto(socket_fd,
                  data,
                  length,
                  MSG_DONTWAIT,
                  (const struct sockaddr *)&g_sdr_control_peer,
                  sizeof(g_sdr_control_peer));
    if (sent == (int)length)
    {
        const uint16_t command = ra8p1_sdr_control_get_le16(&data[8]);
        if ((command == RA8P1_SDR_CONTROL_CAPTURE_REQ) ||
            (command == RA8P1_SDR_CONTROL_WINDOW_ACK))
        {
            ra8p1_sdr_control_message_t message;
            uint32_t cycles;
            memset(&message, 0, sizeof(message));
            message.command = command;
            message.flags = ra8p1_sdr_control_get_le16(&data[10]);
            message.request_id = ra8p1_sdr_control_get_le32(&data[12]);
            message.session_id = ra8p1_sdr_control_get_le32(&data[16]);
            message.center_index = ra8p1_sdr_control_get_le32(&data[20]);
            message.sample_count = ra8p1_sdr_control_get_le32(&data[40]);
            message.status = ra8p1_sdr_control_get_le32(&data[68]);
            if (cpu0_trace_cycle_now(&cycles))
            {
                cpu0_trace_control_tx(&message, cycles);
            }
        }
    }
    return sent == (int)length;
}

static bool rf_pipeline_sdr_control_open(void)
{
    struct sockaddr_in local;
    int socket_buffer = RF_SDR_CONTROL_SOCKET_BUFFER;
    if (g_sdr_control_socket >= 0)
    {
        return true;
    }
    g_sdr_control_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sdr_control_socket < 0)
    {
        return false;
    }
    /* Control responses share the RMAC receive path with a 1640-datagram IQ
     * burst.  Give the sparse SDRC socket enough queueing headroom that a
     * CAPTURE_READY/COMPLETE datagram survives an analysis scheduling slice. */
    (void)setsockopt(g_sdr_control_socket, SOL_SOCKET, SO_RCVBUF,
                     &socket_buffer, sizeof(socket_buffer));
    (void)setsockopt(g_sdr_control_socket, SOL_SOCKET, SO_SNDBUF,
                     &socket_buffer, sizeof(socket_buffer));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(RA8P1_SDR_CONTROL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sdr_control_socket,
             (const struct sockaddr *)&local,
             sizeof(local)) < 0)
    {
        closesocket(g_sdr_control_socket);
        g_sdr_control_socket = -1;
        return false;
    }
    memset(&g_sdr_control_peer, 0, sizeof(g_sdr_control_peer));
    g_sdr_control_peer.sin_family = AF_INET;
    g_sdr_control_peer.sin_port = htons(RA8P1_SDR_CONTROL_PORT);
    g_sdr_control_peer.sin_addr.s_addr = inet_addr(SDR_CONTROL_PEER_IP);
    return true;
}

static void rf_pipeline_sdr_expected_sync(void)
{
    const ra8p1_sdr_control_message_t *request =
        sdr_control_client_expected_request(&g_sdr_control_client);
    uint32_t center_index = UINT32_MAX;
    uint32_t sample_count = 0U;
    uint32_t session_id = 0U;

    if (request != NULL)
    {
        center_index = request->center_index;
        sample_count = request->sample_count;
        session_id = request->session_id;
    }
    rf_pipeline_sdr_expected_publish(center_index, sample_count, session_id);
}

static bool rf_pipeline_elapsed_ms(uint32_t start_tick, uint32_t milliseconds)
{
    const uint32_t elapsed_ticks = (uint32_t)rt_tick_get() - start_tick;
    const uint64_t elapsed_ms = ((uint64_t)elapsed_ticks * 1000U) / RT_TICK_PER_SECOND;
    return elapsed_ms >= milliseconds;
}

static uint32_t rf_pipeline_sample_rate_hz(void)
{
    analysis_stats_t stats;
    analysis_pipeline_get_stats(&stats);
    return (stats.sample_rate_hz != 0U) ? stats.sample_rate_hz : g_requested_sample_rate_hz;
}

static void rf_pipeline_sdr_control_consume(const uint8_t *wire,
                                            size_t wire_length,
                                            uint32_t peer_address,
                                            uint16_t peer_port,
                                            uint32_t now_ms)
{
    uint32_t trace_cycles = 0U;
    bool trace_timing_valid;

    if ((wire == NULL) ||
        (peer_port != g_sdr_control_peer.sin_port) ||
        (peer_address != g_sdr_control_peer.sin_addr.s_addr))
    {
        g_sdr_control_client.stats.invalid_datagrams++;
        return;
    }
    trace_timing_valid = cpu0_trace_cycle_now(&trace_cycles);
    if (sdr_control_client_receive(&g_sdr_control_client,
                                   wire,
                                   wire_length,
                                   now_ms))
    {
        cpu0_trace_control_rx(&g_sdr_control_client.last_response,
                              trace_cycles,
                              trace_timing_valid);
    }
    rf_pipeline_sdr_expected_sync();
}

static void rf_pipeline_sdr_control_receive(uint32_t now_ms)
{
    uint32_t count;
    eth_iq_fast_control_datagram_t direct;

    /* IQSC/5003 never enters lwIP. Give the sparse SDRC/5004 reply the same
     * bounded handoff so CAPTURE_COMPLETE is not delayed behind socket work
     * after a 1640-datagram IQ burst. The control state machine still runs
     * only here in the RF worker. */
    for (count = 0U;
         (count < RF_SDR_CONTROL_RX_BUDGET) &&
         eth_iq_fast_control_pop(&direct);
         ++count)
    {
        rf_pipeline_sdr_control_consume(direct.wire,
                                        direct.length,
                                        direct.source_address,
                                        direct.source_port,
                                        now_ms);
    }

    if (g_sdr_control_socket < 0)
    {
        return;
    }
    for (; count < RF_SDR_CONTROL_RX_BUDGET; ++count)
    {
        uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int received = recvfrom(g_sdr_control_socket,
                                wire,
                                sizeof(wire),
                                MSG_DONTWAIT,
                                (struct sockaddr *)&peer,
                                &peer_length);
        if (received <= 0)
        {
            break;
        }
        if (peer.sin_family != AF_INET)
        {
            g_sdr_control_client.stats.invalid_datagrams++;
            continue;
        }
        rf_pipeline_sdr_control_consume(wire,
                                        (size_t)received,
                                        peer.sin_addr.s_addr,
                                        peer.sin_port,
                                        now_ms);
    }
}

static void rf_pipeline_sdr_control_service(void)
{
    const uint32_t now_ms = rf_pipeline_now_ms();
    uint32_t expected_session;
    sdr_control_client_stats_t client_stats;

    rf_pipeline_sdr_control_receive(now_ms);
    rf_pipeline_sdr_expected_sync();
    expected_session = g_sdr_expected_session_id;

    /* IQSC END is published by the RMAC path before the analysis worker has
     * drained the copied ring.  Latch it into the control client here, before
     * the polling state machine runs, so its one-shot idempotent completion
     * probe can recover CAPTURE_COMPLETE while CRC/STFT continue.  This does
     * not ACK or grant credit: observe_window() still requires a clean,
     * analysis-complete result that CPU1 has made visible.  The pending flag
     * is the publication validity bit; tick value zero is a valid timestamp.
     *
     * Keep this in the existing pipeline worker and at the existing bounded
     * control-service cadence.  Do not add socket work to the RMAC callback
     * or to every IQ ring block. */
    if ((g_stream_end_pending != 0U) &&
        (g_stream_end_session_id == expected_session))
    {
        const uint32_t end_ms = (uint32_t)(
            ((uint64_t)g_stream_end_tick * 1000U) /
            RT_TICK_PER_SECOND);
        sdr_control_client_notify_iqsc_end(&g_sdr_control_client,
                                           expected_session,
                                           g_sdr_expected_center_index,
                                           end_ms);
    }

    sdr_control_client_poll(&g_sdr_control_client, now_ms);
    rf_pipeline_sdr_expected_sync();
    expected_session = g_sdr_expected_session_id;

    if (expected_session != 0U)
    {
        eth_iq_fast_stats_t iq_stats;
        iq_ring_stats_t ring;
        analysis_stats_t analysis;
        sdr_control_window_evidence_t evidence;
        uint32_t ring_full_drops = 0U;
        uint32_t ring_oversize_drops = 0U;

        memset(&iq_stats, 0, sizeof(iq_stats));
        memcpy(&iq_stats,
               (const void *)&g_eth_iq_fast_stats,
               sizeof(iq_stats));
        iq_ring_stats_get(&ring);
        analysis_pipeline_get_stats(&analysis);
        eth_iq_fast_session_ring_drops(&ring_full_drops,
                                       &ring_oversize_drops);
        memset(&evidence, 0, sizeof(evidence));
        evidence.session_id = expected_session;
        evidence.ring_free = (ring.queued < IQ_RING_SLOT_COUNT) ?
                             (IQ_RING_SLOT_COUNT - ring.queued) : 0U;
        evidence.sequence_gaps = iq_stats.sequence_gaps;
        evidence.reordered = iq_stats.reordered;
        evidence.invalid_packets = iq_stats.invalid;
        evidence.ring_drops = ring_full_drops + ring_oversize_drops;
        evidence.ring_full_drops = ring_full_drops;
        evidence.ring_oversize_drops = ring_oversize_drops;
        evidence.crc_errors = iq_stats.crc_errors;
        evidence.crc32c = iq_stats.crc32c;
        evidence.actual_payload_mbps_x1000 = iq_stats.mbps_x1000;
        evidence.iqsc_complete =
            (iq_stats.session_id == expected_session) &&
            (iq_stats.active == 0U) &&
            (ring.queued == 0U) &&
            ((iq_stats.flags & RA8P1_IQ_FLAG_STREAM_END) != 0U);
        evidence.payload_complete =
            iq_stats.payload_bytes ==
            ((uint64_t)g_sdr_expected_sample_count * 4ULL);
        evidence.crc_present =
            (iq_stats.crc_flags & RA8P1_IQ_ACK_FLAG_CRC_PRESENT) != 0U;
        evidence.crc_valid =
            (iq_stats.crc_flags & RA8P1_IQ_ACK_FLAG_CRC_VALID) != 0U;
        if (evidence.iqsc_complete &&
            (g_applied_session_id == expected_session) &&
            (analysis.windows_completed == 0U))
        {
            const bool transport_valid =
                evidence.payload_complete &&
                evidence.crc_present &&
                evidence.crc_valid &&
                (evidence.sequence_gaps == 0U) &&
                (evidence.reordered == 0U) &&
                (evidence.invalid_packets == 0U) &&
                (evidence.ring_drops == 0U) &&
                (evidence.crc_errors == 0U) &&
                (g_stream_discontinuity_seen == 0U) &&
                (g_stream_received_frontier ==
                 g_stream_expected_samples);

            if (transport_valid && analysis_pipeline_window_ready())
            {
                if (!analysis_pipeline_commit_stream())
                {
                    analysis_pipeline_reject_stream(
                        RF_V12_TILE_CAPTURE_TIMEOUT);
                }
            }
            else
            {
                uint8_t tile_flags = 0U;
                if (!evidence.crc_present || !evidence.crc_valid ||
                    (evidence.crc_errors != 0U))
                {
                    tile_flags |= RF_V12_TILE_CRC_ERROR;
                }
                if ((evidence.sequence_gaps != 0U) ||
                    (evidence.reordered != 0U) ||
                    (g_stream_discontinuity_seen != 0U))
                {
                    tile_flags |= RF_V12_TILE_PACKET_GAP;
                }
                if (evidence.ring_drops != 0U)
                {
                    tile_flags |= RF_V12_TILE_RING_OVERFLOW;
                }
                if (!evidence.payload_complete ||
                    (evidence.invalid_packets != 0U) ||
                    !analysis_pipeline_window_ready())
                {
                    tile_flags |= RF_V12_TILE_CAPTURE_TIMEOUT;
                }
                analysis_pipeline_reject_stream(tile_flags);
            }
            analysis_pipeline_get_stats(&analysis);
        }
        evidence.analysis_complete =
            (g_applied_session_id == expected_session) &&
            (analysis.windows_completed != 0U);
        evidence.cpu1_visible =
            ipc_bridge_cpu0_latency_result_visible(expected_session, 0U);
        if ((g_trace_ingress_published_session_id != expected_session) &&
            (g_trace_ingress_session_id == expected_session) &&
            (g_trace_first_packet_valid != 0U) &&
            (g_trace_last_packet_valid != 0U))
        {
            cpu0_trace_ingress(expected_session,
                               g_trace_first_packet_cycles,
                               g_trace_last_packet_cycles);
            g_trace_ingress_published_session_id = expected_session;
        }
        if ((g_trace_crc_published_session_id != expected_session) &&
            (iq_stats.session_id == expected_session) &&
            ((iq_stats.crc_timing_flags &
              ETH_IQ_FAST_CRC_TIMING_COMPLETE_VALID) != 0U))
        {
            uint32_t crc_cycles = (iq_stats.crc_cycles_total > UINT32_MAX) ?
                                  UINT32_MAX :
                                  (uint32_t)iq_stats.crc_cycles_total;
            cpu0_trace_crc(expected_session,
                           iq_stats.crc_complete_cpu0_cycles,
                           crc_cycles,
                           iq_stats.crc32c,
                           evidence.crc_valid,
                           iq_stats.mbps_x1000,
                           iq_stats.sequence_gaps,
                           iq_stats.reordered,
                           iq_stats.invalid,
                           ring_full_drops,
                           ring_oversize_drops,
                           ring.high_watermark,
                           evidence.ring_free);
            g_trace_crc_published_session_id = expected_session;
        }
        sdr_control_client_observe_window(&g_sdr_control_client,
                                          &evidence,
                                          now_ms);
        rf_pipeline_sdr_expected_sync();
    }

    sdr_control_client_stats_get(&g_sdr_control_client,
                                 now_ms,
                                 &client_stats);
    memcpy((void *)&g_sdr_control_stats,
           &client_stats,
           sizeof(client_stats));
    if ((g_stop_command_sequence != 0U) &&
        (g_command_sequence == g_stop_command_sequence))
    {
        if (client_stats.state == SDR_CONTROL_CLIENT_CANCELLED)
        {
            g_command_status = RA8P1_COMMAND_APPLIED;
            g_command_reason = RA8P1_COMMAND_REASON_STOPPED;
            g_stop_command_sequence = 0U;
        }
        else if (client_stats.state == SDR_CONTROL_CLIENT_ERROR)
        {
            g_command_status = RA8P1_COMMAND_REJECTED;
            if ((client_stats.last_status ==
                 RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT) ||
                (client_stats.last_status ==
                 RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT))
            {
                g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_TIMEOUT;
            }
            else if (client_stats.last_status ==
                     RA8P1_SDR_CONTROL_STATUS_SEND_FAILED)
            {
                g_command_reason =
                    RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED;
            }
            else
            {
                g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_ERROR;
            }
            g_stop_command_sequence = 0U;
        }
        else
        {
            g_command_status =
                RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY;
            g_command_reason = RA8P1_COMMAND_REASON_STOPPED;
        }
        return;
    }
    if ((g_stop_command_sequence != 0U) &&
        ((client_stats.state ==
          SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED) ||
         (client_stats.state == SDR_CONTROL_CLIENT_CANCELLED) ||
         (client_stats.state == SDR_CONTROL_CLIENT_ERROR)))
    {
        /* A newer mailbox command owns telemetry.  Retire the old STOP
         * transaction when it becomes terminal, but never publish its result
         * under the newer sequence. */
        if (client_stats.state !=
            SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED)
        {
            g_stop_command_sequence = 0U;
        }
        return;
    }
    if ((client_stats.state == SDR_CONTROL_CLIENT_ERROR) &&
        (g_command_sequence != 0U) &&
        (g_command_sequence == g_start_command_sequence))
    {
        g_command_status = RA8P1_COMMAND_REJECTED;
        if ((client_stats.last_status ==
             RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT) ||
            (client_stats.last_status ==
             RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT))
        {
            g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_TIMEOUT;
        }
        else if (client_stats.last_status ==
                 RA8P1_SDR_CONTROL_STATUS_SEND_FAILED)
        {
            g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED;
        }
        else
        {
            g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_ERROR;
        }
    }
}

static void rf_pipeline_stop_stream_local(void)
{
    /* Stop ingress first.  Once expected_request() has been withdrawn, the
     * higher-priority RMAC path cannot publish another START for this owner. */
    g_stream_rx_active = 0U;
    rf_pipeline_barrier();
    g_stream_config_pending = 0U;
    g_stream_end_pending = 0U;
    g_stream_rx_session_id = 0U;
    g_stream_active_session_id = 0U;
    g_stream_expected_samples = 0U;
    g_stream_received_frontier = 0U;
    g_stream_discontinuity_seen = 0U;
    g_stream_valid = 0U;
    g_stream_last_ingress_tick = 0U;
    g_stream_end_tick = 0U;
    g_stream_end_session_id = 0U;
    g_applied_session_id = 0U;
    g_real_stream_seen = 0U;
    memset(&g_pending_stream_config, 0, sizeof(g_pending_stream_config));
    analysis_pipeline_abort_stream();
    rf_pipeline_barrier();
}

static bool rf_pipeline_command_valid(const ra8p1_ui_command_t *command,
                                      uint32_t *reason)
{
    uint32_t failure = RA8P1_COMMAND_REASON_NONE;
    if (command == NULL)
    {
        if (reason != NULL) *reason = RA8P1_COMMAND_REASON_INVALID_FORMAT;
        return false;
    }
    if ((command->action != RA8P1_COMMAND_ACTION_START) &&
        (command->action != RA8P1_COMMAND_ACTION_STOP))
        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;
    else if (command->action == RA8P1_COMMAND_ACTION_STOP)
        failure = RA8P1_COMMAND_REASON_NONE;
    else if (command->requested_iq_format != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED)
        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;
    else if (command->requested_sample_rate_hz !=
             RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE)
        failure = RA8P1_COMMAND_REASON_INVALID_RATE;
    else if (command->requested_channel_mask != RA8P1_RF_CHANNEL_A_MASK)
        failure = RA8P1_COMMAND_REASON_INVALID_CHANNEL;
    else if ((command->requested_fft_interval_samples != 0U) &&
             (command->requested_fft_interval_samples != ANALYSIS_MODEL_WINDOW_SAMPLES))
        failure = RA8P1_COMMAND_REASON_INVALID_WINDOW;
    else if (command->requested_bandwidth_hz !=
             RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH)
        failure = RA8P1_COMMAND_REASON_INVALID_BANDWIDTH;
    else if ((command->target_payload_mbps_x1000 <
              RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000) ||
             (command->target_payload_mbps_x1000 >
              RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000) ||
             ((command->target_payload_mbps_x1000 % 1000U) != 0U))
        failure = RA8P1_COMMAND_REASON_INVALID_RATE;
    else if ((command->test_fault_flags &
              ~RA8P1_SDR_TEST_FAULT_ALL) != 0U)
        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;
    else if (((command->flags & RA8P1_COMMAND_FLAG_SCAN_ALL) == 0U) &&
             (ra8p1_center_index_from_hz(
                  ra8p1_ui_command_center_a_hz(command)) < 0))
        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;
    else if (((command->flags & RA8P1_COMMAND_FLAG_SCAN_ALL) != 0U) &&
             (ra8p1_ui_command_center_a_hz(command) != 0U) &&
             (ra8p1_ui_command_center_a_hz(command) !=
              RA8P1_CENTER_2420_HZ))
        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;
    if (reason != NULL) *reason = failure;
    return failure == RA8P1_COMMAND_REASON_NONE;
}

static void rf_pipeline_poll_command(void)
{
    ra8p1_ui_command_t command;
    sdr_control_capture_options_t options;
    int32_t center_index;
    bool started;
    uint32_t reason = RA8P1_COMMAND_REASON_NONE;

    /* CPU1 can publish its boot command before lwIP/RMAC finishes link-up.
     * Leave the mailbox unconsumed until SDRC/5004 can be transmitted. */
    if (!rf_pipeline_network_ready())
    {
        return;
    }
    if (!ipc_bridge_cpu0_command_get(&command))
    {
        return;
    }
    g_command_sequence = command.sequence;
    g_pending_command = command;
    g_start_command_sequence = 0U;
    if (!rf_pipeline_command_valid(&command, &reason))
    {
        g_command_status = RA8P1_COMMAND_REJECTED;
        g_command_reason = reason;
        return;
    }
    if (command.action == RA8P1_COMMAND_ACTION_STOP)
    {
        /* A completion may arrive after CPU1 has issued a newer command.  Only
         * the STOP sequence that currently owns telemetry may finalize it. */
        const uint32_t control_state = g_sdr_control_client.stats.state;
        const bool cancel_required =
            (control_state != SDR_CONTROL_CLIENT_IDLE) &&
            (control_state != SDR_CONTROL_CLIENT_COMPLETE) &&
            (control_state != SDR_CONTROL_CLIENT_CANCELLED);
        const bool cancelled = !cancel_required ||
            sdr_control_client_cancel(&g_sdr_control_client,
                                      rf_pipeline_now_ms());
        rf_pipeline_sdr_expected_sync();
        rf_pipeline_stop_stream_local();
        if (!cancelled)
        {
            g_stop_command_sequence = 0U;
            g_command_status = RA8P1_COMMAND_REJECTED;
            g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED;
            return;
        }
        if (cancel_required)
        {
            g_stop_command_sequence = command.sequence;
            g_command_status =
                RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY;
            g_command_reason = RA8P1_COMMAND_REASON_STOPPED;
        }
        else
        {
            g_stop_command_sequence = 0U;
            g_command_status = RA8P1_COMMAND_APPLIED;
            g_command_reason = RA8P1_COMMAND_REASON_STOPPED;
        }
        return;
    }
    g_requested_sample_rate_hz = command.requested_sample_rate_hz;
    g_requested_channel_mask = command.requested_channel_mask;
    g_last_format = command.requested_iq_format;
    if (!rf_pipeline_sdr_control_open())
    {
        g_command_status = RA8P1_COMMAND_REJECTED;
        g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED;
        return;
    }
    sdr_control_capture_options_default(&options);
    options.sample_rate_hz = command.requested_sample_rate_hz;
    options.bandwidth_hz = command.requested_bandwidth_hz;
    options.sample_count = ANALYSIS_MODEL_WINDOW_SAMPLES;
    options.target_payload_mbps_x1000 =
        command.target_payload_mbps_x1000;
    options.test_fault_flags = command.test_fault_flags;
    center_index = ra8p1_center_index_from_hz(
        ra8p1_ui_command_center_a_hz(&command));
    if ((command.flags & RA8P1_COMMAND_FLAG_SCAN_ALL) != 0U)
    {
        if ((command.flags & RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS) != 0U)
        {
            started = sdr_control_client_start_continuous_scan(
                &g_sdr_control_client,
                &options,
                rf_pipeline_now_ms());
        }
        else
        {
            started = sdr_control_client_start_scan(&g_sdr_control_client,
                                                    &options,
                                                    rf_pipeline_now_ms());
        }
    }
    else
    {
        if ((command.flags & RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS) != 0U)
        {
            started = (center_index >= 0) &&
                      sdr_control_client_start_continuous_single(
                          &g_sdr_control_client,
                          (uint32_t)center_index,
                          &options,
                          rf_pipeline_now_ms());
        }
        else
        {
            started = (center_index >= 0) &&
                      sdr_control_client_start_single(
                          &g_sdr_control_client,
                          (uint32_t)center_index,
                          &options,
                          rf_pipeline_now_ms());
        }
    }
    if (!started)
    {
        g_command_status = RA8P1_COMMAND_REJECTED;
        g_command_reason = RA8P1_COMMAND_REASON_SDR_CONTROL_BUSY;
        return;
    }
    g_stop_command_sequence = 0U;
    g_start_command_sequence = command.sequence;
    rf_pipeline_sdr_expected_sync();
    g_command_status = RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY;
    g_command_reason = RA8P1_COMMAND_REASON_WAITING_STREAM_START;
}

static void rf_pipeline_apply_pending_stream_config(void)
{
    ra8p1_iq_stream_config_t config;

    if (g_stream_config_pending == 0U)
    {
        return;
    }

    /* The RX path publishes the complete config before raising the flag.  A
     * barrier here makes the copy visible to the pipeline thread without
     * putting analysis/NPU work in the Ethernet receive context. */
    rf_pipeline_barrier();
    config = g_pending_stream_config;
    if ((g_trace_iqsc_start_valid != 0U) &&
        (g_trace_iqsc_start_session_id == config.session_id))
    {
        cpu0_trace_iqsc_start(config.session_id,
                              g_trace_iqsc_start_cycles);
        g_trace_iqsc_start_valid = 0U;
    }
    analysis_pipeline_configure(config.source_sample_rate_hz,
                                config.sample_rate_hz,
                                config.center_frequency_hz,
                                config.bandwidth_hz,
                                config.window_samples,
                                config.valid_bits,
                                config.flags);
    analysis_pipeline_set_session(config.session_id);
    analysis_pipeline_set_stream_info(((uint64_t)config.total_samples_high << 32U) |
                                      config.total_samples_low,
                                      config.center_index);
    ipc_bridge_cpu0_display_session_set(config.session_id);
    g_requested_sample_rate_hz = config.sample_rate_hz;
    g_requested_channel_mask = config.channel_mask;
    g_last_format = config.format;
    g_stream_active_session_id = config.session_id;
    g_applied_session_id = config.session_id;
    g_real_stream_seen = 1U;
    sdr_control_client_notify_iqsc_start(&g_sdr_control_client,
                                         config.session_id,
                                         config.center_index,
                                         rf_pipeline_now_ms());
    if ((g_command_sequence == g_start_command_sequence) &&
        (g_command_status == RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY) &&
        ((g_pending_command.requested_sample_rate_hz == 0U) ||
         (g_pending_command.requested_sample_rate_hz == config.sample_rate_hz)) &&
        ((g_pending_command.requested_bandwidth_hz == 0U) ||
         (g_pending_command.requested_bandwidth_hz == config.bandwidth_hz)) &&
        ((ra8p1_ui_command_center_a_hz(&g_pending_command) == 0U) ||
         (ra8p1_ui_command_center_a_hz(&g_pending_command) == config.center_frequency_hz)) &&
        ((g_pending_command.requested_fft_interval_samples == 0U) ||
         (g_pending_command.requested_fft_interval_samples == config.window_samples)) &&
        (g_pending_command.requested_channel_mask == config.channel_mask))
    {
        g_command_status = RA8P1_COMMAND_APPLIED;
        g_command_reason = RA8P1_COMMAND_REASON_APPLIED;
    }
    else if ((g_command_sequence != 0U) &&
             (g_command_sequence == g_start_command_sequence) &&
             (g_command_status == RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY))
    {
        g_command_status = RA8P1_COMMAND_REJECTED;
        g_command_reason = RA8P1_COMMAND_REASON_STREAM_MISMATCH;
    }
    rf_pipeline_barrier();
    g_stream_config_pending = 0U;
}

static void rf_pipeline_publish(void)
{
    iq_ring_stats_t ring;
    npu_runner_stats_t npu;
    analysis_stats_t analysis;
    ra8p1_system_telemetry_t telemetry;
    uint32_t now = (uint32_t)rt_tick_get();

    iq_ring_stats_get(&ring);
    analysis_pipeline_set_queue(ring.queued,
                                ring.full_drops + ring.oversize_drops +
                                g_eth_iq_fast_stats.sequence_gaps);
    analysis_pipeline_get_stats(&analysis);
    npu_runner_stats_get(&npu);
    memset(&telemetry, 0, sizeof(telemetry));
    telemetry.magic = RA8P1_SYSTEM_PROTOCOL_MAGIC;
    telemetry.version = RA8P1_SYSTEM_PROTOCOL_VERSION;
    telemetry.size = (uint16_t)sizeof(telemetry);
    telemetry.sequence = g_processed_blocks;
    telemetry.pipeline_state = (g_eth_iq_fast_stats.packets == 0U &&
                                analysis.windows_completed == 0U) ?
                               RA8P1_PIPELINE_WAITING_FOR_IQ :
                               ((ring.queued >= RF_PIPELINE_HIGH_WATERMARK) ?
                                RA8P1_PIPELINE_OVERLOAD : RA8P1_PIPELINE_RUNNING);
    telemetry.ethernet_link_mbps = 1000U;
    telemetry.iq_payload_mbps_x1000 = g_eth_iq_fast_stats.mbps_x1000;
    telemetry.iq_format = g_last_format;
    telemetry.iq_sample_rate_hz = rf_pipeline_sample_rate_hz();
    telemetry.ingress_packets = g_eth_iq_fast_stats.packets;
    telemetry.ingress_drops = ring.full_drops + ring.oversize_drops +
                              g_eth_iq_fast_stats.sequence_gaps;
    telemetry.ring_high_watermark = ring.high_watermark;
    telemetry.processed_blocks = g_processed_blocks;
    telemetry.fft_frames = analysis.stft_frames;
    telemetry.inference_count = npu.inference_count;
    telemetry.inference_cycles = analysis.npu_cycles;
    telemetry.result_class = npu.last_class;
    telemetry.result_score_q15 = npu.last_score_q15;
    if (analysis.end_to_end_cycles != 0U)
    {
        uint64_t busy_cycles = (uint64_t)analysis.stft_cycles + analysis.npu_cycles;
        uint64_t load = (busy_cycles * 1000U) / analysis.end_to_end_cycles;
        telemetry.cpu0_load_permille = (load > 1000U) ? 1000U : (uint32_t)load;
    }
    else
    {
        telemetry.cpu0_load_permille = 0U;
    }
    telemetry.display_fps_millihz = 0U;
    telemetry.flags = 3U; /* Q15 CMSIS-DSP path initialized. */
    if (analysis.synthetic != 0U) telemetry.flags |= RA8P1_SYSTEM_FLAG_SYNTHETIC;
    else if (g_real_stream_seen != 0U) telemetry.flags |= RA8P1_SYSTEM_FLAG_REAL_STREAM;
    if (npu.ready == 0U) telemetry.flags |= RA8P1_SYSTEM_FLAG_NPU_NOT_READY;
    if (analysis.preprocessing_valid == 0U)
        telemetry.flags |= RA8P1_SYSTEM_FLAG_PREPROCESS_INVALID;
    telemetry.fft_size = ANALYSIS_FFT_SIZE;
    telemetry.fft_peak_bin = analysis.peak_bin;
    telemetry.fft_peak_power_q16 = analysis.peak_power_q16;
    telemetry.fft_capture_interval_samples = analysis.window_samples;
    telemetry.channel_mask = (analysis.windows_completed != 0U) ?
                             RA8P1_RF_CHANNEL_A_MASK : 0U;
    telemetry.channel_a_fft_frames = analysis.stft_frames;
    telemetry.command_sequence = g_command_sequence;
    telemetry.command_status = g_command_status;
    telemetry.command_reason = g_command_reason;
    telemetry.applied_session_id = g_applied_session_id;
    telemetry.model_revision = 13U;
    telemetry.model_flags = RA8P1_MODEL_FLAG_TRAINED_INT8 |
                            RA8P1_MODEL_FLAG_CENTER_HEATMAP |
                            RA8P1_MODEL_FLAG_ADAPTIVE_BASELINE |
                            RA8P1_MODEL_FLAG_FIVE_CLASS_FUSED_UI |
                            RA8P1_MODEL_FLAG_CONSERVATIVE_ALERT_GUARD |
                            RA8P1_MODEL_FLAG_DUAL_NPU_MODELS |
                            RA8P1_MODEL_FLAG_VIDEO_VISIBLE_MASK |
                            RA8P1_MODEL_FLAG_PER_CENTER_BASELINE |
                            RA8P1_MODEL_FLAG_NO_ACCURACY_CLAIM;
    if (analysis.preprocessing_valid != 0U)
        telemetry.model_flags |= RA8P1_MODEL_FLAG_BASELINE_READY;
    ipc_bridge_cpu0_publish(&telemetry);
    g_last_publish_tick = now;
    if (analysis.windows_completed != g_stack_last_window_count)
    {
        g_stack_last_window_count = analysis.windows_completed;
        rf_pipeline_stack_proof_update(analysis.windows_completed);
    }
}

static void rf_pipeline_thread_entry(void *parameter)
{
    uint32_t last_publish = (uint32_t)rt_tick_get();
    (void)parameter;

#if RA8P1_STFT_BOOT_PROOF_ENABLE
    /* Run on the analysis worker stack, never on RT-Thread's 2048-byte main
     * stack.  No real analysis can race this one-shot proof because the
     * worker has not entered its ring-consumer loop yet. */
    (void)analysis_pipeline_run_stft_proof();
    analysis_pipeline_configure(ANALYSIS_DEFAULT_SAMPLE_RATE,
                                ANALYSIS_DEFAULT_SAMPLE_RATE,
                                0U,
                                ANALYSIS_FORMAL_BANDWIDTH_HZ,
                                ANALYSIS_MODEL_WINDOW_SAMPLES,
                                12U,
                                RA8P1_IQ_FLAG_VALID_BITS_12);
    analysis_pipeline_set_session(1U);
    analysis_pipeline_set_stream_info(0U, 0U);
    rf_pipeline_stack_proof_update(0U);
#endif

    for (;;)
    {
        iq_ring_view_t view;
        iq_ring_stats_t before;
        iq_ring_stats_t after;
        uint32_t processed = 0U;
        uint32_t budget;

        rf_pipeline_poll_command();
        rf_pipeline_sdr_control_service();
        rf_pipeline_apply_pending_stream_config();
        iq_ring_stats_get(&before);
        if (before.queued >= RF_PIPELINE_HIGH_WATERMARK)
            budget = RF_PIPELINE_SLICE_BLOCKS_HIGH;
        else if (before.queued >= RF_PIPELINE_LOW_WATERMARK)
            budget = RF_PIPELINE_SLICE_BLOCKS_MID;
        else
            budget = RF_PIPELINE_SLICE_BLOCKS_LOW;

        while ((processed < budget) &&
               (g_stream_config_pending == 0U) &&
               iq_ring_pop_begin(&view))
        {
            uint32_t complex_samples = 0U;
            /* A START can arrive after the pending check but before the pop.
             * If this is already a new-session slot, apply its configuration
             * before deciding whether the slot belongs to the active stream. */
            if ((view.session_id != g_stream_active_session_id) &&
                (g_stream_config_pending != 0U))
            {
                rf_pipeline_apply_pending_stream_config();
            }
            if ((view.session_id == g_stream_active_session_id) &&
                (view.format == RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED) &&
                (view.length >= 4U) && ((view.length & 3U) == 0U))
            {
                eth_iq_fast_crc_consume(view.session_id,
                                        view.data,
                                        view.length);
                complex_samples = view.length / 4U;
                analysis_pipeline_ingest_s16((const int16_t *)view.data,
                                             complex_samples,
                                             view.sample_index,
                                             view.flags);
            }
            if (complex_samples != 0U)
            {
                g_last_format = view.format;
                g_processed_blocks++;
            }
            iq_ring_pop_end();
            processed++;
            if ((processed & 0x0FU) == 0U)
            {
                ipc_bridge_cpu0_latency_poll();
            }
        }

        if (processed != 0U)
        {
            /* The data consumer can spend a full slice in CRC/STFT feed work.
             * Drain SDRC again before yielding so terminal responses and
             * prefetch credit are not delayed behind the next IQ slice. */
            rf_pipeline_sdr_control_service();
        }

        /* One acknowledgement scan per scheduling slice bounds the CPU1
         * visibility proof without adding cache maintenance to every packet. */
        ipc_bridge_cpu0_latency_poll();

        if (processed == 0U)
        {
            iq_ring_stats_t idle_stats;
            iq_ring_stats_get(&idle_stats);
            if ((g_stream_end_pending != 0U) &&
                (idle_stats.queued == 0U) &&
                rf_pipeline_elapsed_ms(g_stream_last_ingress_tick,
                                       RF_PIPELINE_END_QUIET_MS) &&
                (((g_stream_discontinuity_seen == 0U) &&
                  (g_stream_received_frontier >= g_stream_expected_samples)) ||
                 rf_pipeline_elapsed_ms(g_stream_end_tick,
                                        RF_PIPELINE_END_GRACE_MS)))
            {
                g_stream_rx_active = 0U;
                rf_pipeline_barrier();
                iq_ring_stats_get(&idle_stats);
                if (idle_stats.queued == 0U)
                {
                    analysis_pipeline_finish_stream();
                    g_stream_end_pending = 0U;
                    g_real_stream_seen = 0U;
                    g_stream_active_session_id = 0U;
                    rf_pipeline_barrier();
                }
            }
            rt_thread_mdelay(1U);
        }
        else if (processed >= budget)
        {
            iq_ring_stats_get(&after);
            if (after.queued == 0U)
            {
                rt_thread_yield();
            }
        }

        if (((uint32_t)rt_tick_get() - last_publish) >= RF_PIPELINE_PUBLISH_MS)
        {
            last_publish = (uint32_t)rt_tick_get();
            rf_pipeline_publish();
        }
    }
}

void rf_pipeline_start(void)
{
    sdr_control_transport_t control_transport;
    uint32_t trace_cycles = 0U;
    uint32_t seed;
    uint64_t boot_epoch;

    iq_ring_init();
    eth_iq_fast_control_init();
    g_processed_blocks = 0U;
    g_last_format = RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED;
    g_requested_sample_rate_hz = ANALYSIS_DEFAULT_SAMPLE_RATE;
    g_requested_channel_mask = RA8P1_RF_CHANNEL_A_MASK;
    g_command_sequence = 0U;
    g_command_status = RA8P1_COMMAND_NONE;
    g_command_reason = RA8P1_COMMAND_REASON_NONE;
    g_start_command_sequence = 0U;
    g_stop_command_sequence = 0U;
    g_applied_session_id = 0U;
    memset(&g_pending_command, 0, sizeof(g_pending_command));
    g_real_stream_seen = 0U;
    g_stream_config_pending = 0U;
    g_stream_end_pending = 0U;
    g_stream_rx_active = 0U;
    g_stream_rx_session_id = 0U;
    g_stream_active_session_id = 0U;
    g_stream_expected_samples = 0U;
    g_stream_received_frontier = 0U;
    g_stream_discontinuity_seen = 0U;
    g_stream_valid = 0U;
    g_stream_last_ingress_tick = 0U;
    g_stream_end_tick = 0U;
    g_stream_end_session_id = 0U;
    memset(&g_pending_stream_config, 0, sizeof(g_pending_stream_config));
    g_sdr_control_socket = -1;
    memset(&g_sdr_control_peer, 0, sizeof(g_sdr_control_peer));
    memset((void *)&g_sdr_control_stats, 0, sizeof(g_sdr_control_stats));
    memset((void *)&g_rf_pipeline_stack_proof,
           0,
           sizeof(g_rf_pipeline_stack_proof));
    g_rf_pipeline_stack_proof.magic = RF_PIPELINE_STACK_PROOF_MAGIC;
    g_rf_pipeline_stack_proof.version = RF_PIPELINE_STACK_PROOF_VERSION;
    g_rf_pipeline_stack_proof.stack_bytes = sizeof(g_rf_pipeline_stack);
    g_rf_pipeline_stack_proof.free_low_water_bytes =
        sizeof(g_rf_pipeline_stack);
    g_stack_last_window_count = 0U;
    g_trace_ingress_session_id = 0U;
    g_trace_first_packet_cycles = 0U;
    g_trace_last_packet_cycles = 0U;
    g_trace_first_packet_valid = 0U;
    g_trace_last_packet_valid = 0U;
    g_trace_iqsc_start_session_id = 0U;
    g_trace_iqsc_start_cycles = 0U;
    g_trace_iqsc_start_valid = 0U;
    g_trace_ingress_published_session_id = 0U;
    g_trace_crc_published_session_id = 0U;
    g_sdr_expected_session_id = 0U;
    g_sdr_expected_center_index = UINT32_MAX;
    g_sdr_expected_sample_count = 0U;
    control_transport.send = rf_pipeline_sdr_control_send;
    control_transport.context = &g_sdr_control_socket;
    cpu0_trace_init();
    (void)cpu0_trace_cycle_now(&trace_cycles);
    seed = rf_pipeline_now_ms() ^ trace_cycles;
    boot_epoch = ((uint64_t)g_cpu0_trace_ring.control.boot_count << 32U) |
                 (uint64_t)seed;
    if (boot_epoch == 0ULL)
    {
        boot_epoch = 1ULL;
    }
    sdr_control_client_init_with_epoch(&g_sdr_control_client,
                                       &control_transport,
                                       0x43500000UL ^ seed,
                                       0x53400000UL ^ seed,
                                       boot_epoch);
    (void)rf_pipeline_sdr_control_open();
    npu_runner_init();
    analysis_pipeline_init();
    analysis_pipeline_configure(ANALYSIS_DEFAULT_SAMPLE_RATE,
                                ANALYSIS_DEFAULT_SAMPLE_RATE,
                                0U,
                                ANALYSIS_FORMAL_BANDWIDTH_HZ,
                                ANALYSIS_MODEL_WINDOW_SAMPLES,
                                12U,
                                RA8P1_IQ_FLAG_VALID_BITS_12);
    analysis_pipeline_set_session(1U);
    analysis_pipeline_set_stream_info(0U, 0U);
    ipc_bridge_cpu0_init();
    if (RT_EOK == rt_thread_init(&g_rf_pipeline_thread,
                                 "rfpipe",
                                 rf_pipeline_thread_entry,
                                 RT_NULL,
                                 g_rf_pipeline_stack,
                                 sizeof(g_rf_pipeline_stack),
                                 RF_PIPELINE_THREAD_PRIORITY,
                                 2U))
    {
        (void)rt_thread_startup(&g_rf_pipeline_thread);
    }
}

bool rf_pipeline_stream_configure(const ra8p1_iq_stream_config_t *config)
{
    uint32_t iqsc_start_cycles = 0U;
    bool iqsc_start_timing_valid;
    uint64_t total_samples;
    if (config == NULL)
    {
        return false;
    }
    total_samples = ((uint64_t)config->total_samples_high << 32U) |
                    config->total_samples_low;
    if ((config == NULL) ||
        (config->session_id == 0U) ||
        (config->session_id != g_sdr_expected_session_id) ||
        (config->center_index != g_sdr_expected_center_index) ||
        (config->format != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED) ||
        (config->channel_mask != RA8P1_RF_CHANNEL_A_MASK) ||
        (config->source_sample_rate_hz != ANALYSIS_FORMAL_SAMPLE_RATE_HZ) ||
        (config->sample_rate_hz != ANALYSIS_FORMAL_SAMPLE_RATE_HZ) ||
        (config->bandwidth_hz != ANALYSIS_FORMAL_BANDWIDTH_HZ) ||
        (config->window_samples != ANALYSIS_MODEL_WINDOW_SAMPLES) ||
        (config->tile_stride_samples != ANALYSIS_MODEL_STRIDE_SAMPLES) ||
        (total_samples != g_sdr_expected_sample_count) ||
        (config->valid_bits != 12U) ||
        (config->center_index >= 4U) ||
        (config->center_frequency_hz !=
         ra8p1_sdr_control_center_frequency(config->center_index)) ||
        ((config->flags & RA8P1_IQ_FLAG_VALID_BITS_12) == 0U) ||
        ((config->flags & RA8P1_IQ_FLAG_WINDOW_CRC) == 0U) ||
        ((config->flags & RA8P1_IQ_FLAG_SYNTHETIC) != 0U) ||
        ((config->flags & ~RF_STREAM_CONFIG_ALLOWED_FLAGS) != 0U))
    {
        return false;
    }
    iqsc_start_timing_valid = cpu0_trace_cycle_now(&iqsc_start_cycles);
    ipc_bridge_cpu0_latency_session_begin(config->session_id,
                                           total_samples,
                                           config->window_samples,
                                           config->tile_stride_samples);
    g_stream_rx_active = 0U;
    rf_pipeline_barrier();
    g_pending_stream_config = *config;
    g_stream_rx_session_id = config->session_id;
    g_stream_expected_samples = total_samples;
    g_stream_received_frontier = 0U;
    g_stream_discontinuity_seen = 0U;
    g_stream_valid = 1U;
    g_stream_last_ingress_tick = (uint32_t)rt_tick_get();
    g_stream_end_tick = 0U;
    g_stream_end_session_id = 0U;
    g_stream_end_pending = 0U;
    g_real_stream_seen = 1U;
    g_trace_ingress_session_id = config->session_id;
    g_trace_first_packet_cycles = 0U;
    g_trace_last_packet_cycles = 0U;
    g_trace_first_packet_valid = 0U;
    g_trace_last_packet_valid = 0U;
    g_trace_iqsc_start_session_id = config->session_id;
    g_trace_iqsc_start_cycles = iqsc_start_cycles;
    g_trace_iqsc_start_valid = iqsc_start_timing_valid ? 1U : 0U;
    g_trace_ingress_published_session_id = 0U;
    g_trace_crc_published_session_id = 0U;
    rf_pipeline_barrier();
    g_stream_config_pending = 1U;
    g_stream_rx_active = 1U;
    return true;
}

bool rf_pipeline_stream_end(const ra8p1_iq_stream_config_t *config)
{
    if ((config == NULL) ||
        (g_stream_rx_active == 0U) ||
        (config->session_id != g_stream_rx_session_id) ||
        ((((uint64_t)config->total_samples_high << 32U) |
          config->total_samples_low) != g_stream_expected_samples) ||
        (config->format != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED) ||
        (config->channel_mask != RA8P1_RF_CHANNEL_A_MASK) ||
        (config->source_sample_rate_hz != g_pending_stream_config.source_sample_rate_hz) ||
        (config->sample_rate_hz != g_pending_stream_config.sample_rate_hz) ||
        (config->center_frequency_hz != g_pending_stream_config.center_frequency_hz) ||
        (config->bandwidth_hz != g_pending_stream_config.bandwidth_hz) ||
        (config->window_samples != g_pending_stream_config.window_samples) ||
        (config->tile_stride_samples != g_pending_stream_config.tile_stride_samples) ||
        (config->valid_bits != g_pending_stream_config.valid_bits) ||
        (config->flags != g_pending_stream_config.flags) ||
        (config->center_index != g_pending_stream_config.center_index))
    {
        return false;
    }
    g_stream_end_tick = (uint32_t)rt_tick_get();
    g_stream_end_session_id = config->session_id;
    rf_pipeline_barrier();
    g_stream_end_pending = 1U;
    return true;
}

bool rf_pipeline_ingest(const uint8_t *data,
                        uint32_t length,
                        uint32_t sequence,
                        uint32_t flags,
                        uint32_t session_id,
                        uint64_t sample_index,
                        uint32_t format)
{
    uint32_t complex_samples;
    uint32_t ingress_cycles = 0U;
    uint32_t ingress_window_mask;
    uint32_t trace_packet_cycles = 0U;
    uint64_t sample_end;
    bool trace_first_candidate;
    bool trace_last_candidate;
    bool trace_timing_valid = false;
    bool pushed;

    rf_pipeline_barrier();
    if ((g_stream_rx_active == 0U) ||
        (data == NULL) ||
        (session_id != g_stream_rx_session_id) ||
        (format != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED) ||
        ((flags & ~RF_DATA_ALLOWED_FLAGS) != 0U) ||
        ((flags & RA8P1_IQ_FLAG_VALID_BITS_12) == 0U) ||
        (length == 0U) ||
        ((length & 3U) != 0U))
    {
        return false;
    }
    complex_samples = length / 4U;
    if (sample_index > (UINT64_MAX - (uint64_t)complex_samples))
    {
        return false;
    }
    sample_end = sample_index + (uint64_t)complex_samples;
    if (sample_end > g_stream_expected_samples)
    {
        return false;
    }
    trace_first_candidate = (session_id == g_trace_ingress_session_id) &&
                            (sample_index == 0U) &&
                            (g_trace_first_packet_valid == 0U);
    trace_last_candidate = (session_id == g_trace_ingress_session_id) &&
                           (sample_end == g_stream_expected_samples) &&
                           (g_trace_last_packet_valid == 0U);
    if (trace_first_candidate || trace_last_candidate)
    {
        trace_timing_valid = cpu0_trace_cycle_now(&trace_packet_cycles);
    }
    if (sample_index < g_stream_received_frontier)
    {
        /* Late or duplicate data cannot be inserted into the STFT timeline. */
        g_stream_discontinuity_seen = 1U;
        return false;
    }
    if (sample_index > g_stream_received_frontier)
    {
        flags |= RA8P1_IQ_FLAG_DISCONTINUITY;
        g_stream_discontinuity_seen = 1U;
    }
    /* Pure integer range matching runs for every packet. DWT barriers run only
     * for a packet containing one of at most 19 formal window starts. */
    ingress_window_mask = ipc_bridge_cpu0_latency_ingress_prepare(session_id,
                                                                  sample_index,
                                                                  complex_samples,
                                                                  &ingress_cycles);
    pushed = iq_ring_push_copy(data, length, sequence, flags,
                               sample_index, format, session_id);
    g_stream_last_ingress_tick = (uint32_t)rt_tick_get();
    g_real_stream_seen = 1U;
    if (!pushed)
    {
        g_stream_discontinuity_seen = 1U;
        rf_pipeline_barrier();
        return false;
    }
    if (trace_timing_valid && trace_first_candidate)
    {
        g_trace_first_packet_cycles = trace_packet_cycles;
        rf_pipeline_barrier();
        g_trace_first_packet_valid = 1U;
    }
    if (trace_timing_valid && trace_last_candidate)
    {
        g_trace_last_packet_cycles = trace_packet_cycles;
        rf_pipeline_barrier();
        g_trace_last_packet_valid = 1U;
    }
    ipc_bridge_cpu0_latency_ingress_commit(session_id,
                                           ingress_window_mask,
                                           ingress_cycles);
    if ((flags & RA8P1_IQ_FLAG_DISCONTINUITY) != 0U)
    {
        g_stream_discontinuity_seen = 1U;
    }
    g_stream_received_frontier = sample_end;
    rf_pipeline_barrier();
    return true;
}
