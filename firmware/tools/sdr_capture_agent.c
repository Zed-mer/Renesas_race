/*
 * Passive SDRC/UDP-5004 capture agent for the Pluto/Zynq SDR.
 *
 * This is intentionally a /tmp process.  It does not change the SDR image,
 * boot configuration, network configuration, or IP address.  CPU0 owns scan
 * scheduling; the agent accepts only the four fixed centers and one 590336
 * sample (about 9.84 ms at 60 MSPS) low-latency window per request.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#error "sdr_capture_agent targets the SDR Linux userspace"
#endif

/* Reuse the already hardware-tested IQSC/ACK implementation in one TU. */
#define RA8P1_SDR_IQ_STREAM_EMBEDDED 1
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "sdr_iq_udp_stream.c"

#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>

#include "../shared/sdr_control_protocol.h"

#define AGENT_SLOT_COUNT             (2U)
#define AGENT_HISTORY_COUNT          (8U)
#define AGENT_RETIRED_EPOCH_COUNT    (4U)
#define AGENT_DEFAULT_ADAPTER        "/tmp/sdr_adapter_libiio.so"
#define AGENT_CAPTURE_TIMEOUT_MS     (1000U)
#define AGENT_TRACE_ENV              "RA8P1_SDR_CAPTURE_TRACE"

typedef enum e_agent_slot_state
{
    AGENT_SLOT_FREE = 0,
    AGENT_SLOT_CAPTURE_QUEUED,
    AGENT_SLOT_CAPTURING,
    AGENT_SLOT_READY,
    AGENT_SLOT_SEND_QUEUED,
    AGENT_SLOT_SENDING,
    AGENT_SLOT_WAIT_ACK,
    AGENT_SLOT_WAIT_RETRANSMIT,
    AGENT_SLOT_CANCEL_PENDING,
    AGENT_SLOT_RETAINED
} agent_slot_state_t;

typedef struct st_agent_slot
{
    uint8_t *iq;
    size_t capacity;
    agent_slot_state_t state;
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t last_response;
    send_session_result_t send_result;
    send_crc_precompute_t crc_precompute;
    uint64_t tune_start_us;
    uint64_t tune_complete_us;
    uint64_t capture_start_us;
    uint64_t capture_complete_us;
    uint64_t last_control_tx_us;
    uint64_t send_complete_us;
    uint64_t ack_deadline_us;
    uint64_t request_deadline_us;
    uint64_t accept_order;
    uint64_t generation;
    uint32_t response_retries;
    uint32_t timeout_reported;
    uint32_t ack_applied;
    uint32_t ack_response_ignored;
    uint32_t send_authorized;
    uint32_t capture_started;
    uint32_t capture_started_reported;
    uint32_t capture_done;
    uint32_t capture_status;
    uint32_t send_done;
    uint32_t send_status;
    uint32_t send_retransmit;
    uint32_t adapter_status_valid;
    ra8p1_sdr_adapter_status_t adapter_status;
    ra8p1_sdr_control_message_t accepted_ack;
} agent_slot_t;

typedef struct st_agent_history
{
    uint32_t valid;
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t accepted_ack;
    ra8p1_sdr_control_message_t response;
} agent_history_t;

typedef struct st_capture_agent
{
    socket_handle_t control_socket;
    socket_handle_t iq_socket;
    socket_handle_t ack_socket;
    struct sockaddr_in iq_peer;
    struct sockaddr_in control_peer;
    socklen_t control_peer_length;
    uint32_t control_peer_valid;
    ra8p1_sdr_adapter_runtime_t sdr;
    agent_slot_t slots[AGENT_SLOT_COUNT];
    uint32_t next_slot;
    uint32_t send_credit;
    /* A credit token belongs to one accepted request, never to an arbitrary
     * READY slot.  This prevents a later prefetched slot from consuming the
     * active window's token when worker completions arrive together. */
    uint64_t send_credit_accept_order;
    uint64_t next_accept_order;
    uint64_t active_boot_epoch;
    uint64_t retired_boot_epochs[AGENT_RETIRED_EPOCH_COUNT];
    uint32_t retired_epoch_next;
    agent_history_t history[AGENT_HISTORY_COUNT];
    uint32_t history_next;
    uint32_t ignored_request_valid;
    ra8p1_sdr_control_message_t ignored_request;
    pthread_mutex_t mutex;
    pthread_cond_t capture_cond;
    pthread_cond_t send_cond;
    pthread_cond_t send_claim_cond;
    pthread_cond_t idle_cond;
    pthread_t capture_thread;
    pthread_t send_thread;
    int event_pipe[2];
    uint64_t next_generation;
    uint64_t capture_worker_generation;
    uint64_t send_worker_generation;
    uint32_t capture_worker_slot;
    uint32_t send_worker_slot;
    uint32_t sync_initialized;
    uint32_t capture_thread_started;
    uint32_t send_thread_started;
    uint32_t workers_stop;
    uint32_t capture_worker_active;
    uint32_t send_worker_active;
    uint32_t threaded;
    /* Diagnostics are deliberately opt-in.  Keep the stream outside the
     * control protocol so enabling a trace cannot change wire ABI/layout. */
    uint32_t diagnostics_enabled;
    FILE *diagnostics_stream;
} capture_agent_t;

static volatile sig_atomic_t g_agent_stop;

static void agent_signal_handler(int signal_number)
{
    (void)signal_number;
    g_agent_stop = 1;
}

static uint32_t agent_trace_env_enabled(void)
{
    const char *value = getenv(AGENT_TRACE_ENV);

    if ((value == NULL) || (value[0] == '\0'))
    {
        return 0U;
    }
    /* Accept the usual shell spellings while keeping the default off. */
    if ((strcmp(value, "0") == 0) ||
        (strcmp(value, "off") == 0) ||
        (strcmp(value, "OFF") == 0) ||
        (strcmp(value, "false") == 0) ||
        (strcmp(value, "FALSE") == 0))
    {
        return 0U;
    }
    return 1U;
}

static void agent_lock(capture_agent_t *agent)
{
    if ((agent != NULL) && (agent->sync_initialized != 0U))
    {
        (void)pthread_mutex_lock(&agent->mutex);
    }
}

static void agent_unlock(capture_agent_t *agent)
{
    if ((agent != NULL) && (agent->sync_initialized != 0U))
    {
        (void)pthread_mutex_unlock(&agent->mutex);
    }
}

static void agent_notify_main(capture_agent_t *agent)
{
    uint8_t event = 1U;
    if ((agent != NULL) && (agent->event_pipe[1] >= 0))
    {
        const ssize_t ignored = write(agent->event_pipe[1], &event,
                                      sizeof(event));
        (void)ignored;
    }
}

static void agent_drain_events(capture_agent_t *agent)
{
    uint8_t events[64];
    if ((agent == NULL) || (agent->event_pipe[0] < 0))
    {
        return;
    }
    while (read(agent->event_pipe[0], events, sizeof(events)) > 0)
    {
    }
}

static bool agent_capture_request_valid(
    const ra8p1_sdr_control_message_t *request)
{
    const uint16_t required =
        RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY |
        RA8P1_SDR_CONTROL_FLAG_WINDOW_CRC32C |
        RA8P1_SDR_CONTROL_FLAG_DOUBLE_BUFFER;

    return (request != NULL) &&
           (request->command == RA8P1_SDR_CONTROL_CAPTURE_REQ) &&
           (request->boot_epoch != 0ULL) &&
           (request->request_id != 0U) && (request->session_id != 0U) &&
           (request->center_index < RA8P1_SDR_CONTROL_CENTER_COUNT) &&
           (request->center_frequency_hz ==
            ra8p1_sdr_control_center_frequency(request->center_index)) &&
           (request->sample_rate_hz ==
            RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE) &&
           (request->bandwidth_hz ==
            RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH) &&
           (request->sample_count == RA8P1_SDR_CONTROL_DEFAULT_SAMPLES) &&
           ((request->flags & required) == required) &&
           ((request->flags & RA8P1_SDR_CONTROL_FLAG_COMPAT_6M) == 0U) &&
           (request->target_payload_mbps_x1000 >=
            RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000) &&
           (request->target_payload_mbps_x1000 <=
            RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000) &&
           ((request->target_payload_mbps_x1000 % 1000U) == 0U) &&
           (request->send_batch != 0U) &&
           (request->send_batch <= IQ_SEND_BATCH_MAX) &&
           (request->retry_limit <= 16U) &&
           (request->attempt <= request->retry_limit) &&
           (((request->attempt == 0U) &&
             ((request->flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) == 0U)) ||
            ((request->attempt != 0U) &&
             ((request->flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) != 0U))) &&
           (request->ack_timeout_ms != 0U) &&
           (request->ack_timeout_ms <= 60000U) &&
           (request->request_timeout_ms >= request->ack_timeout_ms) &&
           (request->credit <= 1U) &&
           (request->ring_free <= RA8P1_SDR_CONTROL_RING_SLOTS) &&
           (request->status == RA8P1_SDR_CONTROL_STATUS_OK) &&
           ((request->test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) == 0U);
}

static bool agent_same_contract(const ra8p1_sdr_control_message_t *left,
                                const ra8p1_sdr_control_message_t *right)
{
    const uint16_t identity_flags =
        (uint16_t)~RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    return (left != NULL) && (right != NULL) &&
           (left->request_id == right->request_id) &&
           (left->session_id == right->session_id) &&
           (left->boot_epoch == right->boot_epoch) &&
           (left->center_index == right->center_index) &&
           (left->center_frequency_hz == right->center_frequency_hz) &&
           (left->sample_rate_hz == right->sample_rate_hz) &&
           (left->bandwidth_hz == right->bandwidth_hz) &&
           (left->sample_count == right->sample_count) &&
           ((left->flags & identity_flags) ==
            (right->flags & identity_flags)) &&
           (left->target_payload_mbps_x1000 ==
            right->target_payload_mbps_x1000) &&
           (left->send_batch == right->send_batch) &&
           (left->retry_limit == right->retry_limit) &&
           (left->ack_timeout_ms == right->ack_timeout_ms) &&
           (left->request_timeout_ms == right->request_timeout_ms) &&
           (left->test_fault_flags == right->test_fault_flags);
}

static bool agent_same_ack(const ra8p1_sdr_control_message_t *left,
                           const ra8p1_sdr_control_message_t *right)
{
    return agent_same_contract(left, right) &&
           (left->command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
           (right->command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
           (left->status == right->status) &&
           (left->credit == right->credit) &&
           (left->ring_free == right->ring_free) &&
           (left->window_crc32c == right->window_crc32c) &&
           (left->sequence_gaps == right->sequence_gaps) &&
           (left->reordered == right->reordered) &&
           (left->invalid_packets == right->invalid_packets) &&
           (left->ring_full_drops == right->ring_full_drops) &&
           (left->ring_oversize_drops == right->ring_oversize_drops) &&
           (left->crc_errors == right->crc_errors);
}

static agent_slot_t *agent_find_slot(capture_agent_t *agent,
                                     uint32_t request_id,
                                     uint32_t session_id)
{
    uint32_t index;
    if (agent == NULL)
    {
        return NULL;
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        if ((slot->state != AGENT_SLOT_FREE) &&
            (slot->request.request_id == request_id) &&
            (slot->request.session_id == session_id))
        {
            return slot;
        }
    }
    return NULL;
}

static agent_slot_t *agent_select_slot(capture_agent_t *agent)
{
    uint32_t pass;
    uint32_t index;
    if (agent == NULL)
    {
        return NULL;
    }
    for (pass = 0U; pass < 2U; ++pass)
    {
        for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
        {
            const uint32_t selected =
                (agent->next_slot + index) % AGENT_SLOT_COUNT;
            agent_slot_t *slot = &agent->slots[selected];
            const bool reusable = (pass == 0U) ?
                (slot->state == AGENT_SLOT_FREE) :
                (slot->state == AGENT_SLOT_RETAINED);
            if (reusable)
            {
                if ((slot->state == AGENT_SLOT_RETAINED) &&
                    (slot->ack_applied != 0U))
                {
                    agent_history_t *history =
                        &agent->history[agent->history_next];
                    memset(history, 0, sizeof(*history));
                    history->valid = 1U;
                    history->request = slot->request;
                    history->accepted_ack = slot->accepted_ack;
                    history->response = slot->last_response;
                    agent->history_next =
                        (agent->history_next + 1U) % AGENT_HISTORY_COUNT;
                }
                agent->next_slot = (selected + 1U) % AGENT_SLOT_COUNT;
                return slot;
            }
        }
    }
    return NULL;
}

static bool agent_has_unacknowledged_window(const capture_agent_t *agent)
{
    uint32_t index;
    if (agent == NULL)
    {
        return false;
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        const agent_slot_state_t state = agent->slots[index].state;
        if ((state == AGENT_SLOT_SEND_QUEUED) ||
            (state == AGENT_SLOT_SENDING) ||
            (state == AGENT_SLOT_WAIT_ACK) ||
            (state == AGENT_SLOT_WAIT_RETRANSMIT))
        {
            return true;
        }
    }
    return false;
}

static agent_history_t *agent_find_history(
    capture_agent_t *agent, uint64_t boot_epoch,
    uint32_t request_id, uint32_t session_id)
{
    uint32_t index;
    if (agent == NULL)
    {
        return NULL;
    }
    for (index = 0U; index < AGENT_HISTORY_COUNT; ++index)
    {
        agent_history_t *history = &agent->history[index];
        if ((history->valid != 0U) &&
            (history->request.boot_epoch == boot_epoch) &&
            (history->request.request_id == request_id) &&
            (history->request.session_id == session_id))
        {
            return history;
        }
    }
    return NULL;
}

static void agent_reset_epoch_state(capture_agent_t *agent)
{
    uint32_t index;

    if (agent->sync_initialized != 0U)
    {
        for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
        {
            agent_slot_t *slot = &agent->slots[index];
            slot->generation = ++agent->next_generation;
            if ((slot->state == AGENT_SLOT_CAPTURE_QUEUED) ||
                (slot->state == AGENT_SLOT_SEND_QUEUED))
            {
                slot->state = AGENT_SLOT_RETAINED;
            }
        }
        (void)pthread_cond_broadcast(&agent->capture_cond);
        (void)pthread_cond_broadcast(&agent->send_cond);
        while ((agent->capture_worker_active != 0U) ||
               (agent->send_worker_active != 0U))
        {
            (void)pthread_cond_wait(&agent->idle_cond, &agent->mutex);
        }
    }
    agent->send_credit = 0U;
    agent->send_credit_accept_order = 0ULL;
    agent->next_accept_order = 0ULL;
    agent->history_next = 0U;
    agent->ignored_request_valid = 0U;
    memset(&agent->ignored_request, 0, sizeof(agent->ignored_request));
    memset(agent->history, 0, sizeof(agent->history));
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        slot->state = AGENT_SLOT_FREE;
        memset(&slot->request, 0, sizeof(slot->request));
        memset(&slot->last_response, 0, sizeof(slot->last_response));
        memset(&slot->accepted_ack, 0, sizeof(slot->accepted_ack));
        slot->ack_applied = 0U;
        slot->ack_response_ignored = 0U;
        slot->send_authorized = 0U;
        slot->capture_started = 0U;
        slot->capture_started_reported = 0U;
        slot->capture_done = 0U;
        slot->send_done = 0U;
        slot->generation = ++agent->next_generation;
    }
}

static bool agent_epoch_retired(const capture_agent_t *agent, uint64_t epoch)
{
    uint32_t index;
    for (index = 0U; index < AGENT_RETIRED_EPOCH_COUNT; ++index)
    {
        if (agent->retired_boot_epochs[index] == epoch)
        {
            return true;
        }
    }
    return false;
}

static bool agent_epoch_has_no_session(const capture_agent_t *agent)
{
    return (agent != NULL) && (agent->next_accept_order == 0ULL) &&
           (agent->ignored_request_valid == 0U);
}

static bool agent_accept_epoch(capture_agent_t *agent,
                               const ra8p1_sdr_control_message_t *message)
{
    const bool valid_capture = agent_capture_request_valid(message);
    const bool fresh_capture =
        valid_capture &&
        (message->attempt == 0U) &&
        ((message->flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) == 0U);
    const bool bootstrap_retry =
        valid_capture && (message->attempt != 0U) &&
        ((message->flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) != 0U);
    const bool bootstrap_capture = fresh_capture || bootstrap_retry;

    if ((message->boot_epoch == 0ULL) ||
        agent_epoch_retired(agent, message->boot_epoch))
    {
        return false;
    }
    if (agent->active_boot_epoch == 0ULL)
    {
        agent->active_boot_epoch = message->boot_epoch;
        return bootstrap_capture;
    }
    if (agent->active_boot_epoch == message->boot_epoch)
    {
        return true;
    }
    if (!bootstrap_capture)
    {
        return false;
    }
    agent->retired_boot_epochs[agent->retired_epoch_next] =
        agent->active_boot_epoch;
    agent->retired_epoch_next =
        (agent->retired_epoch_next + 1U) % AGENT_RETIRED_EPOCH_COUNT;
    agent_reset_epoch_state(agent);
    agent->active_boot_epoch = message->boot_epoch;
    return true;
}

static void agent_make_response(
    const ra8p1_sdr_control_message_t *request,
    uint16_t command,
    uint32_t status,
    ra8p1_sdr_control_message_t *response)
{
    *response = *request;
    response->command = command;
    response->status = status;
    response->message_crc32c = 0U;
}

static int agent_send_control(capture_agent_t *agent,
                              ra8p1_sdr_control_message_t *response)
{
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    ssize_t sent;
    if ((agent == NULL) || (response == NULL) ||
        (agent->control_peer_valid == 0U) ||
        !ra8p1_sdr_control_encode(response, wire))
    {
        return 0;
    }
    response->message_crc32c = ra8p1_sdr_control_get_le32(
        &wire[RA8P1_SDR_CONTROL_CRC_OFFSET]);
    sent = sendto(agent->control_socket, wire, sizeof(wire), 0,
                  (const struct sockaddr *)&agent->control_peer,
                  agent->control_peer_length);
    return sent == (ssize_t)sizeof(wire);
}

static int agent_send_error(capture_agent_t *agent,
                            const ra8p1_sdr_control_message_t *request,
                            uint32_t status)
{
    ra8p1_sdr_control_message_t response;
    agent_make_response(request, RA8P1_SDR_CONTROL_ERROR, status, &response);
    return agent_send_control(agent, &response);
}

static void agent_fill_options(const ra8p1_sdr_control_message_t *request,
                               options_t *options)
{
    memset(options, 0, sizeof(*options));
    options->source_mode = SOURCE_SDR;
    options->samples_per_session = request->sample_count;
    options->sample_rate_hz = request->sample_rate_hz;
    options->bandwidth_hz = request->bandwidth_hz;
    options->rate_mbps = request->target_payload_mbps_x1000 / 1000U;
    options->capture_chunk_samples = request->sample_count;
    options->capture_timeout_ms = AGENT_CAPTURE_TIMEOUT_MS;
    options->socket_sndbuf_bytes = IQ_DEFAULT_SOCKET_SNDBUF_BYTES;
    options->send_batch = request->send_batch;
    /* The wire ABI remains unchanged; GSO is an SDR-local transport choice
     * selected once per /tmp agent launch for controlled A/B testing. */
    options->udp_gso = sender_env_enabled(IQ_SDR_UDP_GSO_ENV);
    options->ack_timeout_ms = request->ack_timeout_ms;
    options->ack_retries = request->retry_limit;
    options->window_crc = 1;
    options->test_corrupt_end_crc =
        (request->attempt == 0U) &&
        ((request->test_fault_flags & RA8P1_SDR_TEST_FAULT_CRC32C) != 0U);
    options->test_drop_data_packet =
        (request->attempt == 0U) &&
        ((request->test_fault_flags &
          RA8P1_SDR_TEST_FAULT_DROP_DATA_PACKET) != 0U);
    /* SDRC WINDOW_ACK is the only flow-control/reliability authority.  The
     * legacy UDP/5002 query waits for ring drain and would serialize capture
     * behind STFT/NPU.  UDP/5002 remains available for diagnostics. */
    options->ack_enabled = 0;
}

static int agent_send_cached_once(capture_agent_t *agent,
                                  const ra8p1_sdr_control_message_t *request,
                                  const uint8_t *iq,
                                  const send_crc_precompute_t *crc_precompute,
                                  send_session_result_t *result)
{
    options_t options;
    agent_fill_options(request, &options);
    memset(result, 0, sizeof(*result));
    return send_session_once(agent->iq_socket, &agent->iq_peer, &options,
                             request->session_id,
                             request->center_frequency_hz,
                             request->center_index, NULL, iq, NULL,
                             crc_precompute, result);
}

static uint64_t agent_elapsed_us(uint64_t start_us, uint64_t end_us)
{
    return ((start_us != 0ULL) && (end_us >= start_us)) ?
        (end_us - start_us) : 0ULL;
}

/*
 * Emit one complete, machine-searchable line per window.  This is called only
 * after the sender returns to the control thread; it is never called from the
 * RMAC receive path, adapter DMA callback, or packet pacing loop.  Keeping the
 * check at the top makes the normal (trace-off) path a single predictable
 * branch with no formatting or I/O.
 */
static void agent_trace_window(const capture_agent_t *agent,
                               const agent_slot_t *slot)
{
    const char *adapter_name;
    FILE *stream;
    uint32_t adapter_flags = 0U;
    uint32_t adapter_profiles = 0U;
    uint32_t adapter_tune_count = 0U;
    uint32_t adapter_recall_count = 0U;
    uint32_t adapter_fallback_count = 0U;
    int32_t adapter_last_tune_status = 0;
    uint64_t adapter_tune_elapsed_us = 0ULL;
    uint64_t adapter_block_setup_us = 0ULL;
    uint64_t adapter_pre_enable_us = 0ULL;
    uint64_t adapter_dma_wait_us = 0ULL;
    uint64_t adapter_disable_us = 0ULL;
    uint64_t adapter_copy_us = 0ULL;
    uint64_t tune_elapsed_us;
    uint64_t capture_elapsed_us;
    uint64_t actual_payload_mbps_x1000 = 0ULL;
    const char *event;

    if ((agent == NULL) || (slot == NULL) ||
        (agent->diagnostics_enabled == 0U))
    {
        return;
    }

    stream = (agent->diagnostics_stream != NULL) ?
        agent->diagnostics_stream : stderr;
    adapter_name = agent->sdr.api.name;
    if (adapter_name == NULL)
    {
        adapter_name = "unknown";
    }
    if (slot->adapter_status_valid != 0U)
    {
        const ra8p1_sdr_adapter_status_t *adapter_status =
            &slot->adapter_status;
        adapter_flags = adapter_status->flags;
        adapter_profiles = adapter_status->fastlock_profiles;
        adapter_tune_count = adapter_status->tune_count;
        adapter_recall_count = adapter_status->fastlock_recall_count;
        adapter_fallback_count = adapter_status->fallback_count;
        adapter_last_tune_status = adapter_status->last_tune_status;
        adapter_tune_elapsed_us = agent_elapsed_us(
            adapter_status->tune_start_ns / 1000ULL,
            adapter_status->tune_complete_ns / 1000ULL);
        adapter_block_setup_us = agent_elapsed_us(
            adapter_status->capture_prepare_ns / 1000ULL,
            adapter_status->blocks_ready_ns / 1000ULL);
        adapter_pre_enable_us = agent_elapsed_us(
            adapter_status->blocks_ready_ns / 1000ULL,
            adapter_status->buffer_enable_ns / 1000ULL);
        adapter_dma_wait_us = agent_elapsed_us(
            adapter_status->buffer_enable_ns / 1000ULL,
            adapter_status->block_dequeue_ns / 1000ULL);
        adapter_disable_us = agent_elapsed_us(
            adapter_status->block_dequeue_ns / 1000ULL,
            adapter_status->buffer_disable_ns / 1000ULL);
        adapter_copy_us = agent_elapsed_us(
            adapter_status->buffer_disable_ns / 1000ULL,
            adapter_status->copy_complete_ns / 1000ULL);
    }
    tune_elapsed_us = agent_elapsed_us(slot->tune_start_us,
                                       slot->tune_complete_us);
    capture_elapsed_us = agent_elapsed_us(slot->capture_start_us,
                                          slot->capture_complete_us);
    if (slot->send_result.elapsed_us != 0ULL)
    {
        actual_payload_mbps_x1000 =
            (slot->send_result.payload_bytes * 8000ULL) /
            slot->send_result.elapsed_us;
    }
    event = (slot->send_status == RA8P1_SDR_CONTROL_STATUS_OK) ?
        "complete" : "send_failed";

    (void)fprintf(stream,
        "SDRC window_trace event=%s request=%" PRIu32
        " session=%" PRIu32 " boot_epoch=0x%016" PRIX64
        " center_index=%" PRIu32 " center_hz=%" PRIu64
        " attempt=%" PRIu32 " retransmit=%" PRIu32
        " send_batch=%u target_mbps=%" PRIu32
        " target_mbps_x1000=%" PRIu32 " samples=%" PRIu32
        " request_rx_us=%" PRIu64
        " tune_start_us=%" PRIu64 " tune_complete_us=%" PRIu64
        " tune_elapsed_us=%" PRIu64
        " capture_start_us=%" PRIu64 " capture_complete_us=%" PRIu64
        " capture_elapsed_us=%" PRIu64
         " send_elapsed_us=%" PRIu64 " send_complete_us=%" PRIu64
         " payload_bytes=%" PRIu64 " data_packets=%" PRIu32
         " udp_packets=%" PRIu32 " actual_mbps_x1000=%" PRIu64
         " pacing_rebases=%" PRIu32 " pacing_max_late_us=%" PRIu64
         " transport=%s gso_requested=%u gso_attempts=%u"
         " gso_batches=%u gso_packets=%u gso_fallbacks=%u"
         " gso_ineligible=%u gso_errno=%u"
         " crc32c=0x%08" PRIX32
        " crc_backend=%s crc_timing=%s crc_cpu_us=%" PRIu64
        " capture_status=%" PRIu32 " send_status=%" PRIu32
        " adapter=%s adapter_status_valid=%" PRIu32
        " adapter_flags=0x%08" PRIX32
        " fastlock_supported=%u fastlock_ready=%u"
        " last_tune_fastlock=%u fastlock_fallback=%u"
        " last_capture_tune_guarded=%u"
        " fastlock_profiles=%" PRIu32 " fastlock_recall_count=%" PRIu32
        " fallback_count=%" PRIu32 " tune_count=%" PRIu32
        " adapter_tune_elapsed_us=%" PRIu64
        " adapter_last_tune_status=%" PRId32
        " adapter_block_setup_us=%" PRIu64
        " adapter_pre_enable_us=%" PRIu64
        " adapter_dma_wait_us=%" PRIu64
        " adapter_disable_us=%" PRIu64
        " adapter_copy_us=%" PRIu64 "\n",
        event, slot->request.request_id, slot->request.session_id,
        slot->request.boot_epoch, slot->request.center_index,
        slot->request.center_frequency_hz, slot->request.attempt,
        slot->send_retransmit, (unsigned)slot->request.send_batch,
        slot->request.target_payload_mbps_x1000 / 1000U,
        slot->request.target_payload_mbps_x1000, slot->request.sample_count,
        slot->request.agent_request_rx_us, slot->tune_start_us,
        slot->tune_complete_us, tune_elapsed_us, slot->capture_start_us,
        slot->capture_complete_us, capture_elapsed_us,
        slot->send_result.elapsed_us, slot->send_complete_us,
         slot->send_result.payload_bytes, slot->send_result.data_packets,
         slot->send_result.logical_udp_packets, actual_payload_mbps_x1000,
         slot->send_result.pacing_rebases,
         slot->send_result.pacing_max_late_us,
         (slot->send_result.udp_gso_batches != 0U) ? "udp_gso" :
         ((slot->send_result.udp_gso_requested != 0U) ?
          "sendmmsg_gso_fallback" : "sendmmsg"),
         slot->send_result.udp_gso_requested,
         slot->send_result.udp_gso_attempts,
         slot->send_result.udp_gso_batches,
         slot->send_result.udp_gso_packets,
         slot->send_result.udp_gso_fallbacks,
         slot->send_result.udp_gso_ineligible_batches,
         slot->send_result.udp_gso_last_errno,
         slot->send_result.crc32c,
        crc32c_backend_name(
            (sdr_crc_backend_t)slot->send_result.crc_backend),
        slot->send_result.crc_timing_enabled != 0U ? "on" : "off",
        slot->send_result.crc_cpu_us,
        slot->capture_status, slot->send_status, adapter_name,
        slot->adapter_status_valid, adapter_flags,
        (unsigned)((adapter_flags &
                    RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_SUPPORTED) != 0U),
        (unsigned)((adapter_flags &
                    RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY) != 0U),
        (unsigned)((adapter_flags &
                    RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK) != 0U),
        (unsigned)((adapter_flags &
                    RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK) != 0U),
        (unsigned)((adapter_flags &
                    RA8P1_SDR_ADAPTER_STATUS_LAST_CAPTURE_TUNE_GUARDED) != 0U),
        adapter_profiles, adapter_recall_count, adapter_fallback_count,
        adapter_tune_count, adapter_tune_elapsed_us,
        adapter_last_tune_status, adapter_block_setup_us,
        adapter_pre_enable_us, adapter_dma_wait_us, adapter_disable_us,
        adapter_copy_us);
    (void)fflush(stream);
}

static const char *agent_control_event_name(uint16_t command)
{
    switch (command)
    {
        case RA8P1_SDR_CONTROL_CAPTURE_REQ:
            return "CAPTURE_REQ";
        case RA8P1_SDR_CONTROL_WINDOW_ACK:
            return "WINDOW_ACK";
        case RA8P1_SDR_CONTROL_CANCEL:
            return "CANCEL";
        case RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED:
            return "CAPTURE_ACCEPTED";
        case RA8P1_SDR_CONTROL_CAPTURE_STARTED:
            return "CAPTURE_STARTED";
        case RA8P1_SDR_CONTROL_CAPTURE_READY:
            return "CAPTURE_READY";
        case RA8P1_SDR_CONTROL_CAPTURE_COMPLETE:
            return "CAPTURE_COMPLETE";
        case RA8P1_SDR_CONTROL_CREDIT_ACCEPTED:
            return "CREDIT_ACCEPTED";
        case RA8P1_SDR_CONTROL_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

static void agent_trace_control_event(
    const capture_agent_t *agent,
    const agent_slot_t *slot,
    const ra8p1_sdr_control_message_t *message,
    const char *direction)
{
    FILE *stream;
    int32_t slot_index = -1;
    uint32_t slot_state = UINT32_MAX;
    uint32_t index;

    if ((agent == NULL) || (message == NULL) ||
        (agent->diagnostics_enabled == 0U))
    {
        return;
    }
    if (direction == NULL)
    {
        direction = "unknown";
    }
    if (slot != NULL)
    {
        for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
        {
            if (slot == &agent->slots[index])
            {
                slot_index = (int32_t)index;
                slot_state = (uint32_t)slot->state;
                break;
            }
        }
    }
    stream = (agent->diagnostics_stream != NULL) ?
        agent->diagnostics_stream : stderr;
    (void)fprintf(stream,
        "SDRC control_trace direction=%s event=%s command=0x%04X"
        " request=%" PRIu32 " session=%" PRIu32
        " slot=%" PRId32 " slot_state=%" PRIu32
        " status=%" PRIu32 " attempt=%" PRIu32
        " credit=%" PRIu32 " ring_free=%" PRIu32
        " window_crc32c=0x%08" PRIX32
        " gaps=%" PRIu32 " reordered=%" PRIu32
        " invalid=%" PRIu32 " ring_full_drops=%" PRIu32
        " ring_oversize_drops=%" PRIu32 " crc_errors=%" PRIu32 "\n",
        direction, agent_control_event_name(message->command),
        (unsigned)message->command, message->request_id,
        message->session_id, slot_index, slot_state, message->status,
        message->attempt, message->credit, message->ring_free,
        message->window_crc32c, message->sequence_gaps,
        message->reordered, message->invalid_packets,
        message->ring_full_drops, message->ring_oversize_drops,
        message->crc_errors);
    (void)fflush(stream);
}

typedef struct st_agent_capture_work
{
    uint64_t tune_start_us;
    uint64_t tune_complete_us;
    uint64_t capture_start_us;
    uint64_t capture_complete_us;
    uint32_t status;
    uint32_t adapter_status_valid;
    ra8p1_sdr_adapter_status_t adapter_status;
} agent_capture_work_t;

static void agent_snapshot_adapter_status(capture_agent_t *agent,
                                          agent_capture_work_t *work)
{
    ra8p1_sdr_adapter_status_t status;
    if ((agent->sdr.api.struct_size < RA8P1_SDR_ADAPTER_V1_STATUS_SIZE) ||
        (agent->sdr.api.get_status == NULL))
    {
        return;
    }
    memset(&status, 0, sizeof(status));
    status.struct_size = sizeof(status);
    if (agent->sdr.api.get_status(agent->sdr.context, &status) == 0)
    {
        work->adapter_status = status;
        work->adapter_status_valid = 1U;
        if ((status.tune_start_ns != 0ULL) &&
            (status.tune_complete_ns >= status.tune_start_ns))
        {
            work->tune_start_us = status.tune_start_ns / 1000ULL;
            work->tune_complete_us = status.tune_complete_ns / 1000ULL;
        }
    }
}

static int agent_tune_window(
    capture_agent_t *agent,
    const ra8p1_sdr_control_message_t *request,
    agent_capture_work_t *work)
{
    int32_t status;

    work->tune_start_us = monotonic_us();
    status = agent->sdr.api.set_rx(agent->sdr.context,
                                   request->center_frequency_hz,
                                   request->sample_rate_hz,
                                   request->bandwidth_hz);
    work->tune_complete_us = monotonic_us();
    if (status != 0)
    {
        work->status = RA8P1_SDR_CONTROL_STATUS_TUNE_FAILED;
        return 0;
    }
    return 1;
}

static int agent_capture_samples(
    capture_agent_t *agent,
    const ra8p1_sdr_control_message_t *request,
    uint8_t *iq,
    size_t capacity,
    agent_capture_work_t *work)
{
    const uint32_t samples = request->sample_count;
    const size_t staging_bytes =
        (size_t)samples * RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    void *staging = NULL;
    int32_t status;

    if (work->capture_start_us == 0ULL)
    {
        work->capture_start_us = monotonic_us();
    }
    if ((agent->sdr.api.struct_size >= RA8P1_SDR_ADAPTER_V1_RX1_LE_SIZE) &&
        (agent->sdr.api.rx1_capture_le != NULL))
    {
        status = agent->sdr.api.rx1_capture_le(
            agent->sdr.context, iq, samples, AGENT_CAPTURE_TIMEOUT_MS);
    }
    else
    {
        staging = sdr_aligned_allocate(64U, staging_bytes);
        if (staging == NULL)
        {
            work->capture_complete_us = monotonic_us();
            work->status = RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED;
            agent_snapshot_adapter_status(agent, work);
            return 0;
        }
        status = agent->sdr.api.rx_capture(agent->sdr.context, staging,
                                            samples,
                                            AGENT_CAPTURE_TIMEOUT_MS);
        if (status == 0)
        {
            status = ra8p1_sdr_convert_rx1_chunk(
                &agent->sdr.api, staging, samples, iq, capacity);
        }
        sdr_aligned_free(staging);
    }
    work->capture_complete_us = monotonic_us();
    work->status = (status == 0) ? RA8P1_SDR_CONTROL_STATUS_OK :
        RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED;
    agent_snapshot_adapter_status(agent, work);
    return status == 0;
}

static int agent_publish_slot_response(capture_agent_t *agent,
                                       agent_slot_t *slot,
                                       const ra8p1_sdr_control_message_t *response)
{
    int sent;
    slot->last_response = *response;
    slot->last_control_tx_us = monotonic_us();
    slot->response_retries = 0U;
    sent = agent_send_control(agent, &slot->last_response);
    if (agent->diagnostics_enabled != 0U)
    {
        agent_trace_control_event(agent, slot, &slot->last_response,
                                  (sent != 0) ? "tx" : "tx_failed");
    }
    return sent;
}

static agent_slot_t *agent_find_oldest_state(capture_agent_t *agent,
                                              agent_slot_state_t state)
{
    agent_slot_t *selected = NULL;
    uint32_t index;
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        if ((slot->state == state) &&
            ((selected == NULL) || (slot->accept_order < selected->accept_order)))
        {
            selected = slot;
        }
    }
    return selected;
}

static bool agent_slot_has_send_credit(const capture_agent_t *agent,
                                       const agent_slot_t *slot)
{
    return (agent != NULL) && (slot != NULL) &&
           (agent->send_credit != 0U) &&
           (agent->send_credit_accept_order != 0ULL) &&
           (slot->accept_order == agent->send_credit_accept_order);
}

static void agent_release_slot_credit(capture_agent_t *agent,
                                       const agent_slot_t *slot)
{
    if (agent_slot_has_send_credit(agent, slot))
    {
        agent->send_credit = 0U;
        agent->send_credit_accept_order = 0ULL;
    }
}

static agent_slot_t *agent_find_credit_ready_slot(capture_agent_t *agent)
{
    uint32_t index;

    if ((agent == NULL) || (agent->send_credit == 0U))
    {
        return NULL;
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        if ((slot->state == AGENT_SLOT_READY) &&
            agent_slot_has_send_credit(agent, slot))
        {
            return slot;
        }
    }
    return NULL;
}

static agent_slot_t *agent_find_oldest_capture_event(capture_agent_t *agent)
{
    agent_slot_t *selected = NULL;
    uint32_t index;

    if (agent == NULL)
    {
        return NULL;
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        const bool event_pending =
            ((slot->capture_started != 0U) &&
             (slot->capture_started_reported == 0U)) ||
            (slot->capture_done != 0U);
        if ((slot->state == AGENT_SLOT_CAPTURING) && event_pending &&
            ((selected == NULL) ||
             (slot->accept_order < selected->accept_order)))
        {
            selected = slot;
        }
    }
    return selected;
}

static uint32_t agent_slot_index(const capture_agent_t *agent,
                                 const agent_slot_t *slot)
{
    return (uint32_t)(slot - &agent->slots[0]);
}

static void *agent_capture_worker(void *context)
{
    capture_agent_t *agent = (capture_agent_t *)context;

    for (;;)
    {
        agent_slot_t *slot;
        ra8p1_sdr_control_message_t request;
        agent_capture_work_t work;
        uint8_t *iq;
        size_t capacity;
        uint32_t slot_index;
        uint64_t generation;
        int tuned;
        send_crc_precompute_t crc_precompute;

        agent_lock(agent);
        while ((agent->workers_stop == 0U) &&
               (agent_find_oldest_state(agent,
                                        AGENT_SLOT_CAPTURE_QUEUED) == NULL))
        {
            (void)pthread_cond_wait(&agent->capture_cond, &agent->mutex);
        }
        if (agent->workers_stop != 0U)
        {
            agent_unlock(agent);
            break;
        }
        slot = agent_find_oldest_state(agent, AGENT_SLOT_CAPTURE_QUEUED);
        slot_index = agent_slot_index(agent, slot);
        generation = slot->generation;
        request = slot->request;
        iq = slot->iq;
        capacity = slot->capacity;
        slot->state = AGENT_SLOT_CAPTURING;
        agent->capture_worker_active = 1U;
        agent->capture_worker_slot = slot_index;
        agent->capture_worker_generation = generation;
        agent_unlock(agent);

        memset(&work, 0, sizeof(work));
        memset(&crc_precompute, 0, sizeof(crc_precompute));
        tuned = agent_tune_window(agent, &request, &work);
        if (tuned != 0)
        {
            work.capture_start_us = monotonic_us();
            agent_lock(agent);
            slot = &agent->slots[slot_index];
            if ((slot->generation == generation) &&
                (slot->state == AGENT_SLOT_CAPTURING))
            {
                slot->tune_start_us = work.tune_start_us;
                slot->tune_complete_us = work.tune_complete_us;
                slot->capture_start_us = work.capture_start_us;
                slot->capture_started = 1U;
            }
            agent_unlock(agent);
            agent_notify_main(agent);
            (void)agent_capture_samples(agent, &request, iq, capacity, &work);
            if (work.status == RA8P1_SDR_CONTROL_STATUS_OK)
            {
                (void)send_crc_precompute_window(
                    iq, request.sample_count * IQ_BYTES_PER_SAMPLE,
                    &crc_precompute);
            }
        }
        else
        {
            agent_snapshot_adapter_status(agent, &work);
        }

        agent_lock(agent);
        slot = &agent->slots[slot_index];
        if ((slot->generation == generation) &&
            (slot->state == AGENT_SLOT_CAPTURING))
        {
            slot->tune_start_us = work.tune_start_us;
            slot->tune_complete_us = work.tune_complete_us;
            slot->capture_start_us = work.capture_start_us;
            slot->capture_complete_us = work.capture_complete_us;
            slot->capture_status = work.status;
            slot->crc_precompute = crc_precompute;
            slot->adapter_status_valid = work.adapter_status_valid;
            slot->adapter_status = work.adapter_status;
            slot->capture_done = 1U;
        }
        else if (slot->state == AGENT_SLOT_CANCEL_PENDING)
        {
            /* The control plane already acknowledged CANCEL.  Retain the
             * slot until the vendor call returns so its IQ buffer cannot be
             * reused while the worker still owns it. */
            slot->state = AGENT_SLOT_RETAINED;
            slot->capture_done = 0U;
        }
        agent->capture_worker_active = 0U;
        (void)pthread_cond_broadcast(&agent->idle_cond);
        agent_unlock(agent);
        agent_notify_main(agent);
    }
    return NULL;
}

static void *agent_send_worker(void *context)
{
    capture_agent_t *agent = (capture_agent_t *)context;

    for (;;)
    {
        agent_slot_t *slot;
        ra8p1_sdr_control_message_t request;
        send_session_result_t send_result;
        const uint8_t *iq;
        send_crc_precompute_t crc_precompute;
        uint32_t slot_index;
        uint64_t generation;
        int sent;

        agent_lock(agent);
        while ((agent->workers_stop == 0U) &&
               (agent_find_oldest_state(agent,
                                        AGENT_SLOT_SEND_QUEUED) == NULL))
        {
            (void)pthread_cond_wait(&agent->send_cond, &agent->mutex);
        }
        if (agent->workers_stop != 0U)
        {
            agent_unlock(agent);
            break;
        }
        slot = agent_find_oldest_state(agent, AGENT_SLOT_SEND_QUEUED);
        slot_index = agent_slot_index(agent, slot);
        generation = slot->generation;
        request = slot->request;
        iq = slot->iq;
        crc_precompute = slot->crc_precompute;
        slot->state = AGENT_SLOT_SENDING;
        agent->send_worker_active = 1U;
        agent->send_worker_slot = slot_index;
        agent->send_worker_generation = generation;
        /* A WINDOW_ACK handler may be waiting for proof that this worker owns
         * the ready slot.  Signal while holding the mutex; cond_wait will only
         * return after this worker releases it, so IQ sending can overlap the
         * subsequent CREDIT_ACCEPTED control datagram. */
        (void)pthread_cond_broadcast(&agent->send_claim_cond);
        agent_unlock(agent);

        memset(&send_result, 0, sizeof(send_result));
        sent = agent_send_cached_once(agent, &request, iq, &crc_precompute,
                                      &send_result);

        agent_lock(agent);
        slot = &agent->slots[slot_index];
        if ((slot->generation == generation) &&
            (slot->state == AGENT_SLOT_SENDING))
        {
            slot->send_result = send_result;
            slot->send_complete_us = monotonic_us();
            slot->send_status = (sent != 0) ?
                RA8P1_SDR_CONTROL_STATUS_OK :
                RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
            slot->send_done = 1U;
        }
        else if (slot->state == AGENT_SLOT_CANCEL_PENDING)
        {
            slot->state = AGENT_SLOT_RETAINED;
            slot->send_done = 0U;
        }
        agent->send_worker_active = 0U;
        (void)pthread_cond_broadcast(&agent->idle_cond);
        agent_unlock(agent);
        agent_notify_main(agent);
    }
    return NULL;
}

static int agent_finish_send(capture_agent_t *agent, agent_slot_t *slot)
{
    ra8p1_sdr_control_message_t response;

    slot->send_done = 0U;
    slot->request_deadline_us = slot->send_complete_us +
        (uint64_t)slot->request.request_timeout_ms * 1000ULL;
    if (slot->send_status != RA8P1_SDR_CONTROL_STATUS_OK)
    {
        slot->state = AGENT_SLOT_WAIT_RETRANSMIT;
        slot->ack_deadline_us = 0ULL;
        agent_make_response(&slot->request, RA8P1_SDR_CONTROL_ERROR,
                            RA8P1_SDR_CONTROL_STATUS_SEND_FAILED, &response);
        if (agent->diagnostics_enabled != 0U)
        {
            agent_trace_window(agent, slot);
        }
        return agent_publish_slot_response(agent, slot, &response);
    }

    agent_make_response(&slot->request,
                        RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                        RA8P1_SDR_CONTROL_STATUS_OK, &response);
    response.agent_request_rx_us = slot->request.agent_request_rx_us;
    response.tune_start_us = slot->tune_start_us;
    response.tune_complete_us = slot->tune_complete_us;
    response.capture_start_us = slot->capture_start_us;
    response.capture_complete_us = slot->capture_complete_us;
    response.window_crc32c = slot->send_result.crc32c;
    response.actual_payload_mbps_x1000 =
        (slot->send_result.elapsed_us != 0U) ?
        (uint32_t)((slot->send_result.payload_bytes * 8000ULL) /
                   slot->send_result.elapsed_us) : 0U;
    slot->state = AGENT_SLOT_WAIT_ACK;
    slot->timeout_reported = 0U;
    slot->ack_deadline_us = slot->send_complete_us +
        (uint64_t)slot->request.ack_timeout_ms * 1000ULL;
    if (agent->diagnostics_enabled != 0U)
    {
        agent_trace_window(agent, slot);
    }
    return agent_publish_slot_response(agent, slot, &response);
}

static int agent_send_slot_data(capture_agent_t *agent, agent_slot_t *slot,
                                bool retransmit)
{
    int sent;

    if ((slot == NULL) ||
        ((!retransmit) && (!agent_slot_has_send_credit(agent, slot) ||
                           agent_has_unacknowledged_window(agent))) ||
        (retransmit && (slot->send_authorized == 0U)))
    {
        return 0;
    }
    if (!retransmit)
    {
        slot->send_authorized = 1U;
        agent->send_credit = 0U;
        agent->send_credit_accept_order = 0ULL;
    }
    slot->send_retransmit = retransmit ? 1U : 0U;
    slot->send_done = 0U;
    slot->send_status = RA8P1_SDR_CONTROL_STATUS_OK;
    if (agent->threaded != 0U)
    {
        slot->state = AGENT_SLOT_SEND_QUEUED;
        (void)pthread_cond_signal(&agent->send_cond);
        return 1;
    }

    slot->state = AGENT_SLOT_SENDING;
    sent = agent_send_cached_once(agent, &slot->request, slot->iq,
                                  &slot->crc_precompute,
                                  &slot->send_result);
    slot->send_complete_us = monotonic_us();
    slot->send_status = (sent != 0) ? RA8P1_SDR_CONTROL_STATUS_OK :
        RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
    slot->send_done = 1U;
    return agent_finish_send(agent, slot);
}

static int agent_publish_ready(capture_agent_t *agent, agent_slot_t *slot)
{
    ra8p1_sdr_control_message_t response;
    agent_make_response(&slot->request, RA8P1_SDR_CONTROL_CAPTURE_READY,
                        RA8P1_SDR_CONTROL_STATUS_OK, &response);
    response.agent_request_rx_us = slot->request.agent_request_rx_us;
    response.tune_start_us = slot->tune_start_us;
    response.tune_complete_us = slot->tune_complete_us;
    response.capture_start_us = slot->capture_start_us;
    response.capture_complete_us = slot->capture_complete_us;
    slot->state = AGENT_SLOT_READY;
    slot->request_deadline_us =
        ((slot->capture_complete_us != 0ULL) ?
         slot->capture_complete_us : monotonic_us()) +
        (uint64_t)slot->request.request_timeout_ms * 1000ULL;
    return agent_publish_slot_response(agent, slot, &response);
}

static agent_slot_t *agent_find_ready_slot(capture_agent_t *agent)
{
    return agent_find_oldest_state(agent, AGENT_SLOT_READY);
}

static int agent_try_send_ready(capture_agent_t *agent)
{
    agent_slot_t *slot;
    if ((agent->send_credit == 0U) || agent_has_unacknowledged_window(agent))
    {
        return 1;
    }
    slot = agent_find_credit_ready_slot(agent);
    if (slot == NULL)
    {
        return 1;
    }
    return agent_send_slot_data(agent, slot, false);
}

static agent_slot_t *agent_find_accept_order(capture_agent_t *agent,
                                              uint64_t accept_order)
{
    uint32_t index;

    if ((agent == NULL) || (accept_order == 0ULL))
    {
        return NULL;
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        if ((slot->state != AGENT_SLOT_FREE) &&
            (slot->accept_order == accept_order))
        {
            return slot;
        }
    }
    return NULL;
}

/* Called with agent->mutex held.  If the newly credited successor is already
 * READY, queue it and wait only until the send worker claims the slot.  The
 * condition wait releases the mutex, allowing the worker to transition to
 * SENDING; it does not wait for the IQ window to finish. */
static int agent_start_credited_ready_and_wait_claim(capture_agent_t *agent,
                                                      uint64_t accept_order)
{
    agent_slot_t *slot;
    uint64_t generation;
    int wait_status;

    if ((agent == NULL) || (accept_order == 0ULL))
    {
        return 1;
    }
    slot = agent_find_accept_order(agent, accept_order);
    if (slot == NULL)
    {
        return 1;
    }
    generation = slot->generation;
    if (slot->state == AGENT_SLOT_READY)
    {
        if (!agent_send_slot_data(agent, slot, false))
        {
            return 0;
        }
    }
    if (slot->state != AGENT_SLOT_SEND_QUEUED)
    {
        /* Capture may still be in progress, or a duplicate ACK may observe a
         * successor that the worker has already claimed. */
        return 1;
    }
    if ((agent->sync_initialized == 0U) ||
        (agent->send_thread_started == 0U))
    {
        return 0;
    }

    while ((agent->workers_stop == 0U) &&
           (slot->generation == generation) &&
           (slot->state == AGENT_SLOT_SEND_QUEUED))
    {
        wait_status = pthread_cond_wait(&agent->send_claim_cond,
                                        &agent->mutex);
        if (wait_status != 0)
        {
            return 0;
        }
    }
    return (slot->generation == generation) &&
           (slot->state == AGENT_SLOT_SENDING);
}

static int agent_capture_and_send(capture_agent_t *agent,
                                  agent_slot_t *slot,
                                  bool retransmit)
{
    ra8p1_sdr_control_message_t response;
    agent_capture_work_t work;

    agent_make_response(&slot->request,
                        RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                        RA8P1_SDR_CONTROL_STATUS_OK, &response);
    response.agent_request_rx_us = slot->request.agent_request_rx_us;
    /* The CAPTURE_ACCEPTED datagram is advisory and can be recovered by an
     * idempotent duplicate CAPTURE_REQ.  Its local send failure must not leave
     * an already accepted slot stranded in CAPTURE_QUEUED: the worker still
     * owns the capture, and later STARTED/COMPLETE replies can re-establish
     * control-plane progress. */
    (void)agent_publish_slot_response(agent, slot, &response);

    if (retransmit)
    {
        return agent_send_slot_data(agent, slot, true);
    }

    slot->capture_started = 0U;
    slot->capture_started_reported = 0U;
    slot->capture_done = 0U;
    slot->capture_status = RA8P1_SDR_CONTROL_STATUS_OK;
    memset(&slot->crc_precompute, 0, sizeof(slot->crc_precompute));
    if (agent->threaded != 0U)
    {
        slot->state = AGENT_SLOT_CAPTURE_QUEUED;
        (void)pthread_cond_signal(&agent->capture_cond);
        return 1;
    }

    memset(&work, 0, sizeof(work));
    slot->state = AGENT_SLOT_CAPTURING;
    if (!agent_tune_window(agent, &slot->request, &work))
    {
        agent_make_response(&slot->request, RA8P1_SDR_CONTROL_ERROR,
                            RA8P1_SDR_CONTROL_STATUS_TUNE_FAILED, &response);
        agent_release_slot_credit(agent, slot);
        slot->state = AGENT_SLOT_RETAINED;
        return agent_publish_slot_response(agent, slot, &response);
    }
    work.capture_start_us = monotonic_us();
    slot->tune_start_us = work.tune_start_us;
    slot->tune_complete_us = work.tune_complete_us;
    slot->capture_start_us = work.capture_start_us;
    agent_make_response(&slot->request,
                        RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                        RA8P1_SDR_CONTROL_STATUS_OK, &response);
    response.agent_request_rx_us = slot->request.agent_request_rx_us;
    response.tune_start_us = slot->tune_start_us;
    response.tune_complete_us = slot->tune_complete_us;
    response.capture_start_us = slot->capture_start_us;
    (void)agent_publish_slot_response(agent, slot, &response);
    if (!agent_capture_samples(agent, &slot->request, slot->iq,
                               slot->capacity, &work))
    {
        agent_make_response(&slot->request, RA8P1_SDR_CONTROL_ERROR,
                            RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED,
                            &response);
        agent_release_slot_credit(agent, slot);
        slot->state = AGENT_SLOT_RETAINED;
        return agent_publish_slot_response(agent, slot, &response);
    }
    (void)send_crc_precompute_window(
        slot->iq, slot->request.sample_count * IQ_BYTES_PER_SAMPLE,
        &slot->crc_precompute);
    slot->tune_start_us = work.tune_start_us;
    slot->tune_complete_us = work.tune_complete_us;
    slot->capture_start_us = work.capture_start_us;
    slot->capture_complete_us = work.capture_complete_us;
    slot->adapter_status_valid = work.adapter_status_valid;
    slot->adapter_status = work.adapter_status;
    slot->state = AGENT_SLOT_READY;
    if (!agent_slot_has_send_credit(agent, slot) ||
        agent_has_unacknowledged_window(agent))
    {
        return agent_publish_ready(agent, slot);
    }
    return agent_send_slot_data(agent, slot, false);
}

static int agent_publish_capture_started(capture_agent_t *agent,
                                         agent_slot_t *slot)
{
    ra8p1_sdr_control_message_t response;
    agent_make_response(&slot->request,
                        RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                        RA8P1_SDR_CONTROL_STATUS_OK, &response);
    response.agent_request_rx_us = slot->request.agent_request_rx_us;
    response.tune_start_us = slot->tune_start_us;
    response.tune_complete_us = slot->tune_complete_us;
    response.capture_start_us = slot->capture_start_us;
    slot->capture_started_reported = 1U;
    return agent_publish_slot_response(agent, slot, &response);
}

static int agent_finish_capture(capture_agent_t *agent, agent_slot_t *slot)
{
    ra8p1_sdr_control_message_t response;

    slot->capture_done = 0U;
    if (slot->capture_status != RA8P1_SDR_CONTROL_STATUS_OK)
    {
        agent_make_response(&slot->request, RA8P1_SDR_CONTROL_ERROR,
                            slot->capture_status, &response);
        response.agent_request_rx_us = slot->request.agent_request_rx_us;
        response.tune_start_us = slot->tune_start_us;
        response.tune_complete_us = slot->tune_complete_us;
        response.capture_start_us = slot->capture_start_us;
        response.capture_complete_us = slot->capture_complete_us;
        agent_release_slot_credit(agent, slot);
        slot->state = AGENT_SLOT_RETAINED;
        return agent_publish_slot_response(agent, slot, &response);
    }

    slot->state = AGENT_SLOT_READY;
    if (!agent_slot_has_send_credit(agent, slot) ||
        agent_has_unacknowledged_window(agent))
    {
        return agent_publish_ready(agent, slot);
    }
    return agent_send_slot_data(agent, slot, false);
}

static int agent_service_worker_events(capture_agent_t *agent)
{
    agent_slot_t *slot;
    int status = 1;

    /* Completion notifications can become visible together.  Drain them in
     * request acceptance order so a prefetched slot cannot consume the active
     * slot's send credit merely because it occupies a lower array index. */
    while ((slot = agent_find_oldest_capture_event(agent)) != NULL)
    {
        if ((slot->capture_started != 0U) &&
            (slot->capture_started_reported == 0U))
        {
            status = agent_publish_capture_started(agent, slot) && status;
        }
        if ((slot->state == AGENT_SLOT_CAPTURING) &&
            (slot->capture_done != 0U))
        {
            status = agent_finish_capture(agent, slot) && status;
        }
    }

    while ((slot = agent_find_oldest_state(agent, AGENT_SLOT_SENDING)) != NULL)
    {
        if (slot->send_done == 0U)
        {
            break;
        }
        status = agent_finish_send(agent, slot) && status;
    }
    return status;
}

static bool agent_window_ack_valid(const agent_slot_t *slot,
                                   const ra8p1_sdr_control_message_t *ack)
{
    const bool success = ack->status == RA8P1_SDR_CONTROL_STATUS_OK;
    const bool retry = ack->status == RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW;
    const bool clean = (ack->sequence_gaps == 0U) &&
                       (ack->reordered == 0U) &&
                       (ack->invalid_packets == 0U) &&
                       (ack->ring_full_drops == 0U) &&
                       (ack->ring_oversize_drops == 0U) &&
                       (ack->crc_errors == 0U);
    return (slot != NULL) && (ack != NULL) &&
           ((slot->state == AGENT_SLOT_WAIT_ACK) ||
            (slot->state == AGENT_SLOT_WAIT_RETRANSMIT)) &&
           (ack->command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
           agent_same_contract(&slot->request, ack) &&
           (success || retry) &&
           (ack->credit <= 1U) &&
           (ack->ring_free == RA8P1_SDR_CONTROL_RING_SLOTS) &&
           (!success || (clean &&
            (ack->window_crc32c == slot->last_response.window_crc32c))) &&
           (!retry || (ack->credit == 0U));
}

static int agent_apply_window_ack(capture_agent_t *agent,
                                  agent_slot_t *slot,
                                  const ra8p1_sdr_control_message_t *ack,
                                  ra8p1_sdr_control_message_t *response)
{
    uint32_t window_crc32c;
    uint32_t actual_payload_mbps_x1000;

    if ((slot != NULL) && (slot->ack_applied != 0U))
    {
        if (!agent_same_ack(&slot->accepted_ack, ack))
        {
            return 0;
        }
        *response = slot->last_response;
        return 1;
    }
    if (!agent_window_ack_valid(slot, ack))
    {
        return 0;
    }
    if ((ack->credit != 0U) && (agent->send_credit != 0U))
    {
        return 0;
    }
    window_crc32c = slot->last_response.window_crc32c;
    actual_payload_mbps_x1000 =
        slot->last_response.actual_payload_mbps_x1000;
    agent_make_response(ack, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                        RA8P1_SDR_CONTROL_STATUS_OK, response);
    response->window_crc32c = window_crc32c;
    response->actual_payload_mbps_x1000 = actual_payload_mbps_x1000;
    slot->accepted_ack = *ack;
    slot->ack_applied = 1U;
    if (ack->status == RA8P1_SDR_CONTROL_STATUS_OK)
    {
        slot->state = AGENT_SLOT_RETAINED;
        slot->send_authorized = 0U;
        agent->send_credit = ack->credit;
        agent->send_credit_accept_order = (ack->credit != 0U) ?
            (slot->accept_order + 1ULL) : 0ULL;
    }
    else
    {
        slot->state = AGENT_SLOT_WAIT_RETRANSMIT;
    }
    slot->last_response = *response;
    return 1;
}

static int agent_handle_capture_request(
    capture_agent_t *agent,
    const ra8p1_sdr_control_message_t *request)
{
    agent_slot_t *slot;
    agent_history_t *history;
    const bool retransmit =
        (request->flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) != 0U;
    bool ignored_retry = false;

    /* Keep the receive side of every request in the formal trace.  The slot
     * does not exist yet for a fresh request, so agent_trace_control_event()
     * deliberately accepts NULL and records slot=-1. */
    agent_trace_control_event(agent, NULL, request, "rx");

    if (!agent_capture_request_valid(request))
    {
        return agent_send_error(agent, request,
                                RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST);
    }
    if ((request->attempt == 0U) &&
        ((request->test_fault_flags &
          RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_REQUEST) != 0U))
    {
        if (agent->ignored_request_valid != 0U)
        {
            if (!agent_same_contract(&agent->ignored_request, request))
            {
                return agent_send_error(agent, request,
                                        RA8P1_SDR_CONTROL_STATUS_BUSY);
            }
            return 1;
        }
        agent->ignored_request = *request;
        agent->ignored_request_valid = 1U;
        return 1;
    }
    slot = agent_find_slot(agent, request->request_id, request->session_id);
    if (slot != NULL)
    {
        if (!agent_same_contract(&slot->request, request))
        {
            return agent_send_error(agent, request,
                                    RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST);
        }
        if (retransmit && (request->attempt > slot->request.attempt) &&
            ((slot->state == AGENT_SLOT_WAIT_RETRANSMIT) ||
             (slot->state == AGENT_SLOT_WAIT_ACK)))
        {
            const uint64_t first_request_rx_us =
                slot->request.agent_request_rx_us;
            slot->request = *request;
            slot->request.agent_request_rx_us = first_request_rx_us;
            slot->generation = ++agent->next_generation;
            slot->ack_applied = 0U;
            memset(&slot->accepted_ack, 0, sizeof(slot->accepted_ack));
            return agent_capture_and_send(agent, slot, true);
        }
        /* Same attempt is idempotent: return the latest state, never recapture. */
        return agent_send_control(agent, &slot->last_response);
    }
    history = agent_find_history(agent, request->boot_epoch,
                                 request->request_id, request->session_id);
    if (history != NULL)
    {
        if (!agent_same_contract(&history->request, request))
        {
            return agent_send_error(agent, request,
                                    RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST);
        }
        return agent_send_control(agent, &history->response);
    }
    ignored_retry = retransmit &&
        (agent->ignored_request_valid != 0U) &&
        agent_same_contract(&agent->ignored_request, request);
    if (retransmit && !ignored_retry && !agent_epoch_has_no_session(agent))
    {
        return agent_send_error(agent, request,
                                RA8P1_SDR_CONTROL_STATUS_NO_CREDIT);
    }
    if ((request->credit == 0U) && (agent->send_credit == 0U) &&
        !agent_has_unacknowledged_window(agent) &&
        (agent_find_ready_slot(agent) == NULL))
    {
        return agent_send_error(agent, request,
                                RA8P1_SDR_CONTROL_STATUS_NO_CREDIT);
    }
    if ((request->credit != 0U) &&
        ((agent->send_credit != 0U) ||
         agent_has_unacknowledged_window(agent) ||
         (agent_find_ready_slot(agent) != NULL)))
    {
        return agent_send_error(agent, request,
                                RA8P1_SDR_CONTROL_STATUS_NO_CREDIT);
    }
    slot = agent_select_slot(agent);
    if ((slot == NULL) ||
        (slot->capacity < (size_t)request->sample_count * IQ_BYTES_PER_SAMPLE))
    {
        return agent_send_error(agent, request,
                                RA8P1_SDR_CONTROL_STATUS_BUSY);
    }
    memset(&slot->last_response, 0, sizeof(slot->last_response));
    memset(&slot->accepted_ack, 0, sizeof(slot->accepted_ack));
    memset(&slot->crc_precompute, 0, sizeof(slot->crc_precompute));
    slot->request = *request;
    slot->request.agent_request_rx_us = monotonic_us();
    slot->tune_start_us = 0U;
    slot->tune_complete_us = 0U;
    slot->capture_start_us = 0U;
    slot->capture_complete_us = 0U;
    slot->send_complete_us = 0U;
    slot->ack_deadline_us = 0U;
    slot->request_deadline_us = 0U;
    slot->accept_order = ++agent->next_accept_order;
    slot->generation = ++agent->next_generation;
    slot->response_retries = 0U;
    slot->timeout_reported = 0U;
    slot->ack_applied = 0U;
    slot->ack_response_ignored = 0U;
    slot->send_authorized = 0U;
    slot->capture_started = 0U;
    slot->capture_started_reported = 0U;
    slot->capture_done = 0U;
    slot->capture_status = RA8P1_SDR_CONTROL_STATUS_OK;
    slot->send_done = 0U;
    slot->send_status = RA8P1_SDR_CONTROL_STATUS_OK;
    slot->send_retransmit = 0U;
    slot->adapter_status_valid = 0U;
    memset(&slot->adapter_status, 0, sizeof(slot->adapter_status));
    if (ignored_retry)
    {
        agent->ignored_request_valid = 0U;
        memset(&agent->ignored_request, 0, sizeof(agent->ignored_request));
    }
    if (request->credit != 0U)
    {
        agent->send_credit = request->credit;
        agent->send_credit_accept_order = slot->accept_order;
    }
    slot->state = AGENT_SLOT_CAPTURE_QUEUED;
    return agent_capture_and_send(agent, slot, false);
}

static int agent_handle_window_ack(capture_agent_t *agent,
                                   const ra8p1_sdr_control_message_t *ack)
{
    agent_slot_t *slot = agent_find_slot(agent, ack->request_id,
                                         ack->session_id);
    agent_history_t *history;
    ra8p1_sdr_control_message_t response;
    uint64_t credited_accept_order = 0ULL;
    int data_started = 1;
    int sent;
    if (agent->diagnostics_enabled != 0U)
    {
        agent_trace_control_event(agent, slot, ack, "rx");
    }
    if (slot == NULL)
    {
        history = agent_find_history(agent, ack->boot_epoch,
                                     ack->request_id, ack->session_id);
        if ((history != NULL) && agent_same_ack(&history->accepted_ack, ack))
        {
            return agent_send_control(agent, &history->response);
        }
        return agent_send_error(agent, ack,
                                RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST);
    }
    if (!agent_apply_window_ack(agent, slot, ack, &response))
    {
        return agent_send_error(agent, ack,
                                RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST);
    }
    if ((ack->attempt == 0U) &&
        (slot->ack_response_ignored == 0U) &&
        ((slot->request.test_fault_flags &
          RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_ACK_RESPONSE) != 0U))
    {
        /* Apply ownership exactly once, but do not publish CREDIT_ACCEPTED or
         * release a prefetched sender until CPU0 retries this ACK. */
        slot->ack_response_ignored = 1U;
        return 1;
    }
    if ((ack->status == RA8P1_SDR_CONTROL_STATUS_OK) &&
        (ack->credit != 0U))
    {
        credited_accept_order = slot->accept_order + 1ULL;
    }
    /* CPU0 accepts IQSC START as stronger proof than a delayed
     * CREDIT_ACCEPTED datagram.  In threaded mode, wait only for the send
     * worker to claim a ready successor and release this mutex, then publish
     * the control response while the IQ syscall loop runs independently. */
    if ((ack->status == RA8P1_SDR_CONTROL_STATUS_OK) &&
        (agent->threaded != 0U))
    {
        data_started = agent_start_credited_ready_and_wait_claim(
            agent, credited_accept_order);
        if (data_started == 0)
        {
            return 0;
        }
    }
    sent = agent_publish_slot_response(agent, slot, &response);
    if ((sent != 0) && (ack->status == RA8P1_SDR_CONTROL_STATUS_OK) &&
        (agent->threaded == 0U))
    {
        data_started = agent_try_send_ready(agent);
    }
    return sent && data_started;
}

static int agent_handle_cancel(capture_agent_t *agent,
                               const ra8p1_sdr_control_message_t *cancel)
{
    agent_slot_t *slot = agent_find_slot(agent, cancel->request_id,
                                          cancel->session_id);
    if ((agent->ignored_request_valid != 0U) &&
        agent_same_contract(&agent->ignored_request, cancel))
    {
        agent->ignored_request_valid = 0U;
        memset(&agent->ignored_request, 0, sizeof(agent->ignored_request));
    }
    if (slot != NULL)
    {
        slot->generation = ++agent->next_generation;
        agent_release_slot_credit(agent, slot);
        if ((slot->state == AGENT_SLOT_CAPTURE_QUEUED) ||
            (slot->state == AGENT_SLOT_SEND_QUEUED) ||
            (slot->state == AGENT_SLOT_READY) ||
            (slot->state == AGENT_SLOT_WAIT_ACK) ||
            (slot->state == AGENT_SLOT_WAIT_RETRANSMIT))
        {
            slot->state = AGENT_SLOT_RETAINED;
        }
        else if ((slot->state == AGENT_SLOT_CAPTURING) ||
                 (slot->state == AGENT_SLOT_SENDING))
        {
            /* Do not wait under the control mutex for a vendor DMA call. The
             * worker owns the buffer until it returns and will mark it retained
             * through the generation check above. */
            slot->state = AGENT_SLOT_CANCEL_PENDING;
        }
        slot->send_authorized = 0U;
        slot->capture_done = 0U;
        slot->send_done = 0U;
    }
    return agent_send_error(agent, cancel,
                            RA8P1_SDR_CONTROL_STATUS_CANCELLED);
}

static int agent_dispatch(capture_agent_t *agent,
                          const ra8p1_sdr_control_message_t *message)
{
    if (!agent_accept_epoch(agent, message))
    {
        return agent_send_error(agent, message,
                                RA8P1_SDR_CONTROL_STATUS_STALE_EPOCH);
    }
    switch (message->command)
    {
        case RA8P1_SDR_CONTROL_CAPTURE_REQ:
            return agent_handle_capture_request(agent, message);
        case RA8P1_SDR_CONTROL_WINDOW_ACK:
            return agent_handle_window_ack(agent, message);
        case RA8P1_SDR_CONTROL_CANCEL:
            return agent_handle_cancel(agent, message);
        default:
            return agent_send_error(agent, message,
                                    RA8P1_SDR_CONTROL_STATUS_INVALID_MESSAGE);
    }
}

static void agent_service_timeouts(capture_agent_t *agent)
{
    const uint64_t now_us = monotonic_us();
    uint32_t index;

    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent_slot_t *slot = &agent->slots[index];
        const bool waiting = (slot->state == AGENT_SLOT_READY) ||
                             (slot->state == AGENT_SLOT_WAIT_ACK) ||
                             (slot->state == AGENT_SLOT_WAIT_RETRANSMIT);
        if (!waiting)
        {
            continue;
        }
        if ((slot->request_deadline_us != 0ULL) &&
            (now_us >= slot->request_deadline_us))
        {
            if (slot->timeout_reported == 0U)
            {
                ra8p1_sdr_control_message_t timeout_response;
                agent_make_response(
                    &slot->request, RA8P1_SDR_CONTROL_ERROR,
                    (slot->state == AGENT_SLOT_READY) ?
                    RA8P1_SDR_CONTROL_STATUS_WINDOW_NOT_READY :
                    RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT,
                    &timeout_response);
                timeout_response.window_crc32c =
                    slot->last_response.window_crc32c;
                slot->timeout_reported = 1U;
                (void)agent_publish_slot_response(agent, slot,
                                                  &timeout_response);
            }
            else if ((slot->response_retries < slot->request.retry_limit) &&
                     (now_us - slot->last_control_tx_us >=
                      (uint64_t)slot->request.ack_timeout_ms * 1000ULL))
            {
                if (agent_send_control(agent, &slot->last_response))
                {
                    slot->last_control_tx_us = now_us;
                    slot->response_retries++;
                }
            }
            /* Preserve cached IQ until CPU0's terminal path sends CANCEL. */
            continue;
        }
        if ((slot->response_retries < slot->request.retry_limit) &&
            (((slot->state == AGENT_SLOT_WAIT_ACK) &&
              (slot->ack_deadline_us != 0ULL) &&
              (now_us >= slot->ack_deadline_us)) ||
             (((slot->state == AGENT_SLOT_READY) ||
               (slot->state == AGENT_SLOT_WAIT_RETRANSMIT)) &&
              (now_us - slot->last_control_tx_us >=
               (uint64_t)slot->request.ack_timeout_ms * 1000ULL))))
        {
            if (agent_send_control(agent, &slot->last_response))
            {
                slot->last_control_tx_us = now_us;
                slot->response_retries++;
                if (slot->state == AGENT_SLOT_WAIT_ACK)
                {
                    slot->ack_deadline_us = now_us +
                        (uint64_t)slot->request.ack_timeout_ms * 1000ULL;
                }
            }
        }
    }
}

static int agent_open_sockets(capture_agent_t *agent,
                              const char *ra_ip,
                              uint16_t control_port)
{
    struct sockaddr_in local;
    options_t options;
    memset(&agent->iq_peer, 0, sizeof(agent->iq_peer));
    agent->iq_peer.sin_family = AF_INET;
    agent->iq_peer.sin_port = htons(IQ_PORT);
    if (inet_pton(AF_INET, ra_ip, &agent->iq_peer.sin_addr) != 1)
    {
        return 0;
    }
    agent->control_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    agent->iq_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    agent->ack_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((agent->control_socket == INVALID_SOCKET_HANDLE) ||
        (agent->iq_socket == INVALID_SOCKET_HANDLE) ||
        (agent->ack_socket == INVALID_SOCKET_HANDLE))
    {
        return 0;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(control_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(agent->control_socket, (const struct sockaddr *)&local,
             sizeof(local)) != 0)
    {
        return 0;
    }
    if (connect(agent->iq_socket, (const struct sockaddr *)&agent->iq_peer,
                sizeof(agent->iq_peer)) != 0)
    {
        return 0;
    }
    agent->iq_peer.sin_port = htons(IQ_ACK_PORT);
    if (connect(agent->ack_socket, (const struct sockaddr *)&agent->iq_peer,
                sizeof(agent->iq_peer)) != 0)
    {
        return 0;
    }
    agent->iq_peer.sin_port = htons(IQ_PORT);
    memset(&options, 0, sizeof(options));
    options.socket_sndbuf_bytes = IQ_DEFAULT_SOCKET_SNDBUF_BYTES;
    /* UDP_SEGMENT relies on checksum/offload support on the target Linux
     * stack.  Preserve the legacy no-check path for the sendmmsg baseline,
     * but keep checksums enabled by default for the GSO A/B mode. */
    options.udp_no_check = sender_env_enabled(IQ_SDR_UDP_GSO_ENV) ? 0 : 1;
    if (sender_env_enabled(IQ_SDR_UDP_NO_CHECK_ENV))
    {
        options.udp_no_check = 1;
    }
    configure_udp_socket(agent->iq_socket, &options);
    return 1;
}

static void agent_log_thread_fallback(const char *reason)
{
    fprintf(stderr,
            "WARNING: SDRC worker startup failed (%s); using serialized "
            "single-thread fallback without capture/send overlap\n",
            reason);
}

static int agent_start_workers(capture_agent_t *agent)
{
    int mutex_ready = 0;
    int capture_cond_ready = 0;
    int send_cond_ready = 0;
    int send_claim_cond_ready = 0;
    int idle_cond_ready = 0;

    if (pthread_mutex_init(&agent->mutex, NULL) != 0)
    {
        fprintf(stderr, "SDRC pthread_mutex_init failed\n");
        return 0;
    }
    mutex_ready = 1;
    if (pthread_cond_init(&agent->capture_cond, NULL) != 0)
    {
        fprintf(stderr, "SDRC capture pthread_cond_init failed\n");
        goto sync_failed;
    }
    capture_cond_ready = 1;
    if (pthread_cond_init(&agent->send_cond, NULL) != 0)
    {
        fprintf(stderr, "SDRC send pthread_cond_init failed\n");
        goto sync_failed;
    }
    send_cond_ready = 1;
    if (pthread_cond_init(&agent->send_claim_cond, NULL) != 0)
    {
        fprintf(stderr, "SDRC send claim pthread_cond_init failed\n");
        goto sync_failed;
    }
    send_claim_cond_ready = 1;
    if (pthread_cond_init(&agent->idle_cond, NULL) != 0)
    {
        fprintf(stderr, "SDRC idle pthread_cond_init failed\n");
        goto sync_failed;
    }
    idle_cond_ready = 1;
    agent->sync_initialized = 1U;

    if (pipe(agent->event_pipe) != 0)
    {
        perror("SDRC event pipe");
        return 0;
    }
    if ((fcntl(agent->event_pipe[0], F_SETFL,
               fcntl(agent->event_pipe[0], F_GETFL, 0) | O_NONBLOCK) != 0) ||
        (fcntl(agent->event_pipe[1], F_SETFL,
               fcntl(agent->event_pipe[1], F_GETFL, 0) | O_NONBLOCK) != 0))
    {
        perror("SDRC event pipe nonblocking mode");
        (void)close(agent->event_pipe[0]);
        (void)close(agent->event_pipe[1]);
        agent->event_pipe[0] = -1;
        agent->event_pipe[1] = -1;
        return 0;
    }
    if (pthread_create(&agent->capture_thread, NULL,
                       agent_capture_worker, agent) != 0)
    {
        agent_log_thread_fallback("capture pthread_create");
        (void)close(agent->event_pipe[0]);
        (void)close(agent->event_pipe[1]);
        agent->event_pipe[0] = -1;
        agent->event_pipe[1] = -1;
        return 1;
    }
    agent->capture_thread_started = 1U;
    if (pthread_create(&agent->send_thread, NULL,
                       agent_send_worker, agent) != 0)
    {
        agent_log_thread_fallback("send pthread_create");
        agent_lock(agent);
        agent->workers_stop = 1U;
        (void)pthread_cond_broadcast(&agent->capture_cond);
        agent_unlock(agent);
        (void)pthread_join(agent->capture_thread, NULL);
        agent->capture_thread_started = 0U;
        agent->workers_stop = 0U;
        (void)close(agent->event_pipe[0]);
        (void)close(agent->event_pipe[1]);
        agent->event_pipe[0] = -1;
        agent->event_pipe[1] = -1;
        return 1;
    }
    agent->send_thread_started = 1U;
    agent->threaded = 1U;
    return 1;

sync_failed:
    if (idle_cond_ready != 0)
    {
        (void)pthread_cond_destroy(&agent->idle_cond);
    }
    if (send_claim_cond_ready != 0)
    {
        (void)pthread_cond_destroy(&agent->send_claim_cond);
    }
    if (send_cond_ready != 0)
    {
        (void)pthread_cond_destroy(&agent->send_cond);
    }
    if (capture_cond_ready != 0)
    {
        (void)pthread_cond_destroy(&agent->capture_cond);
    }
    if (mutex_ready != 0)
    {
        (void)pthread_mutex_destroy(&agent->mutex);
    }
    return 0;
}

static void agent_stop_workers(capture_agent_t *agent)
{
    if (agent->sync_initialized != 0U)
    {
        agent_lock(agent);
        agent->workers_stop = 1U;
        (void)pthread_cond_broadcast(&agent->capture_cond);
        (void)pthread_cond_broadcast(&agent->send_cond);
        (void)pthread_cond_broadcast(&agent->send_claim_cond);
        agent_unlock(agent);
        agent_notify_main(agent);
    }
    if (agent->capture_thread_started != 0U)
    {
        (void)pthread_join(agent->capture_thread, NULL);
        agent->capture_thread_started = 0U;
    }
    if (agent->send_thread_started != 0U)
    {
        (void)pthread_join(agent->send_thread, NULL);
        agent->send_thread_started = 0U;
    }
    agent->threaded = 0U;
    if (agent->event_pipe[0] >= 0)
    {
        (void)close(agent->event_pipe[0]);
        agent->event_pipe[0] = -1;
    }
    if (agent->event_pipe[1] >= 0)
    {
        (void)close(agent->event_pipe[1]);
        agent->event_pipe[1] = -1;
    }
    if (agent->sync_initialized != 0U)
    {
        (void)pthread_cond_destroy(&agent->idle_cond);
        (void)pthread_cond_destroy(&agent->send_claim_cond);
        (void)pthread_cond_destroy(&agent->send_cond);
        (void)pthread_cond_destroy(&agent->capture_cond);
        (void)pthread_mutex_destroy(&agent->mutex);
        agent->sync_initialized = 0U;
    }
}

static void agent_close(capture_agent_t *agent)
{
    uint32_t index;
    agent_stop_workers(agent);
    if (agent->sdr.loaded != 0U)
    {
        (void)sdr_adapter_close(&agent->sdr);
    }
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        sdr_aligned_free(agent->slots[index].iq);
        agent->slots[index].iq = NULL;
    }
    if (agent->control_socket != INVALID_SOCKET_HANDLE)
    {
        close_socket(agent->control_socket);
    }
    if (agent->iq_socket != INVALID_SOCKET_HANDLE)
    {
        close_socket(agent->iq_socket);
    }
    if (agent->ack_socket != INVALID_SOCKET_HANDLE)
    {
        close_socket(agent->ack_socket);
    }
}

#ifndef RA8P1_SDR_CAPTURE_AGENT_NO_MAIN
static void agent_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <ra8-ip> [--adapter <adapter.so>] "
            "[--control-port 5004] [--trace]\n"
            "       RA8P1_SDR_CAPTURE_TRACE=1 enables the same trace.\n"
            "       RA8P1_SDR_UDP_GSO=1 enables Linux UDP GSO A/B mode.\n"
            "       RA8P1_SDR_UDP_NO_CHECK=1 forces checksum-off testing.\n",
            program);
}

int main(int argc, char **argv)
{
    capture_agent_t agent;
    const char *adapter = getenv(IQ_SDR_ADAPTER_ENV);
    const char *ra_ip;
    uint16_t control_port = RA8P1_SDR_CONTROL_PORT;
    size_t window_bytes =
        (size_t)RA8P1_SDR_CONTROL_DEFAULT_SAMPLES * IQ_BYTES_PER_SAMPLE;
    uint32_t index;
    int argument;
    int result = 1;
    uint32_t diagnostics_enabled = agent_trace_env_enabled();

    if (argc < 2)
    {
        agent_usage(argv[0]);
        return 2;
    }
    ra_ip = argv[1];
    if ((adapter == NULL) || (adapter[0] == '\0'))
    {
        adapter = AGENT_DEFAULT_ADAPTER;
    }
    for (argument = 2; argument < argc; ++argument)
    {
        if ((strcmp(argv[argument], "--adapter") == 0) &&
            (++argument < argc))
        {
            adapter = argv[argument];
        }
        else if ((strcmp(argv[argument], "--control-port") == 0) &&
                 (++argument < argc))
        {
            uint32_t value;
            if (!parse_u32(argv[argument], &value) || (value == 0U) ||
                (value > UINT16_MAX))
            {
                agent_usage(argv[0]);
                return 2;
            }
            control_port = (uint16_t)value;
        }
        else if (strcmp(argv[argument], "--trace") == 0)
        {
            diagnostics_enabled = 1U;
        }
        else if (strcmp(argv[argument], "--no-trace") == 0)
        {
            diagnostics_enabled = 0U;
        }
        else
        {
            agent_usage(argv[0]);
            return 2;
        }
    }

    memset(&agent, 0, sizeof(agent));
    agent.control_socket = INVALID_SOCKET_HANDLE;
    agent.iq_socket = INVALID_SOCKET_HANDLE;
    agent.ack_socket = INVALID_SOCKET_HANDLE;
    agent.event_pipe[0] = -1;
    agent.event_pipe[1] = -1;
    agent.diagnostics_enabled = diagnostics_enabled;
    agent.diagnostics_stream = stderr;
    for (index = 0U; index < AGENT_SLOT_COUNT; ++index)
    {
        agent.slots[index].iq = sdr_aligned_allocate(64U, window_bytes);
        agent.slots[index].capacity = window_bytes;
        if (agent.slots[index].iq == NULL)
        {
            fprintf(stderr, "cannot allocate SDR window slot %" PRIu32 "\n",
                    index);
            goto cleanup;
        }
    }
    if (!agent_open_sockets(&agent, ra_ip, control_port))
    {
        perror("capture-agent socket");
        goto cleanup;
    }
    if (!sdr_adapter_open(&agent.sdr, adapter,
                          ra8p1_sdr_control_center_frequency(0U),
                          RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE,
                          RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH))
    {
        goto cleanup;
    }
    if (!agent_start_workers(&agent))
    {
        goto cleanup;
    }
    (void)signal(SIGINT, agent_signal_handler);
    (void)signal(SIGTERM, agent_signal_handler);
    setvbuf(stdout, NULL, _IOLBF, 0U);
    printf("SDRC passive agent ready ra_ip=%s control_port=%u "
           "slots=%u samples=%u adapter=%s mode=%s\n",
           ra_ip, (unsigned)control_port, (unsigned)AGENT_SLOT_COUNT,
           (unsigned)RA8P1_SDR_CONTROL_DEFAULT_SAMPLES, adapter,
           (agent.threaded != 0U) ? "capture-send-overlap" :
           "serialized-fallback");
    if (agent.diagnostics_enabled != 0U)
    {
        printf("SDRC diagnostics enabled: window trace on stderr\n");
    }

    while (!g_agent_stop)
    {
        struct pollfd wait_fds[2];
        nfds_t wait_count = 1U;
        uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES + 1U];
        ra8p1_sdr_control_message_t message;
        ssize_t received;
        int ready;
        memset(wait_fds, 0, sizeof(wait_fds));
        wait_fds[0].fd = agent.control_socket;
        wait_fds[0].events = POLLIN;
        if ((agent.threaded != 0U) && (agent.event_pipe[0] >= 0))
        {
            wait_fds[1].fd = agent.event_pipe[0];
            wait_fds[1].events = POLLIN;
            wait_count = 2U;
        }
        ready = poll(wait_fds, wait_count, 250);
        agent_lock(&agent);
        if ((wait_count == 2U) &&
            ((wait_fds[1].revents & POLLIN) != 0))
        {
            agent_drain_events(&agent);
        }
        (void)agent_service_worker_events(&agent);
        agent_service_timeouts(&agent);
        agent_unlock(&agent);
        if ((ready < 0) && (errno == EINTR))
        {
            continue;
        }
        if (ready < 0)
        {
            perror("capture-agent poll");
            goto cleanup;
        }
        if ((ready == 0) || ((wait_fds[0].revents & POLLIN) == 0))
        {
            continue;
        }
        agent.control_peer_length = sizeof(agent.control_peer);
        received = recvfrom(agent.control_socket, wire, sizeof(wire), 0,
                            (struct sockaddr *)&agent.control_peer,
                            &agent.control_peer_length);
        if ((received != (ssize_t)RA8P1_SDR_CONTROL_WIRE_BYTES) ||
            !ra8p1_sdr_control_decode(wire, (size_t)received, &message))
        {
            continue;
        }
        if (agent.control_peer.sin_addr.s_addr !=
            agent.iq_peer.sin_addr.s_addr)
        {
            continue;
        }
        agent.control_peer_valid = 1U;
        agent_lock(&agent);
        if (!agent_dispatch(&agent, &message))
        {
            fprintf(stderr,
                    "SDRC dispatch/send failed request=%" PRIu32
                    " session=%" PRIu32 " command=0x%04X\n",
                    message.request_id, message.session_id,
                    (unsigned)message.command);
        }
        (void)agent_service_worker_events(&agent);
        agent_service_timeouts(&agent);
        agent_unlock(&agent);
    }
    result = 0;

cleanup:
    agent_close(&agent);
    return result;
}
#endif
