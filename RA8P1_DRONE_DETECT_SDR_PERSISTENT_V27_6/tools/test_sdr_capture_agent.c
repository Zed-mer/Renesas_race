#define RA8P1_SDR_CAPTURE_AGENT_NO_MAIN 1
#include "sdr_capture_agent.c"

#include <stdio.h>

static void make_request(ra8p1_sdr_control_message_t *request,
                         uint32_t request_id,
                         uint32_t session_id,
                         uint32_t center_index)
{
    memset(request, 0, sizeof(*request));
    request->command = RA8P1_SDR_CONTROL_CAPTURE_REQ;
    request->flags = RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY |
                     RA8P1_SDR_CONTROL_FLAG_WINDOW_CRC32C |
                     RA8P1_SDR_CONTROL_FLAG_FASTLOCK |
                     RA8P1_SDR_CONTROL_FLAG_DOUBLE_BUFFER;
    request->request_id = request_id;
    request->session_id = session_id;
    request->boot_epoch = 0x1122334455667788ULL;
    request->center_index = center_index;
    request->center_frequency_hz =
        ra8p1_sdr_control_center_frequency(center_index);
    request->sample_rate_hz = RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE;
    request->bandwidth_hz = RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH;
    request->sample_count = RA8P1_SDR_CONTROL_DEFAULT_SAMPLES;
    request->target_payload_mbps_x1000 = 320000U;
    request->send_batch = 4U;
    request->retry_limit = 2U;
    request->ack_timeout_ms = 1000U;
    request->request_timeout_ms = 5000U;
    request->credit = 1U;
    request->ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    request->status = RA8P1_SDR_CONTROL_STATUS_OK;
}

static int test_validation_and_identity(void)
{
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t retry;
    make_request(&first, 11U, 21U, 2U);
    if (!agent_capture_request_valid(&first))
    {
        return 1;
    }
    retry = first;
    retry.flags |= RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    retry.attempt = 1U;
    if (!agent_capture_request_valid(&retry) ||
        !agent_same_contract(&first, &retry))
    {
        return 2;
    }
    retry.center_frequency_hz++;
    if (agent_capture_request_valid(&retry) ||
        agent_same_contract(&first, &retry))
    {
        return 3;
    }
    retry = first;
    retry.flags &= (uint16_t)~RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY;
    retry.flags |= RA8P1_SDR_CONTROL_FLAG_COMPAT_6M;
    retry.sample_count = RA8P1_SDR_CONTROL_COMPAT_SAMPLES;
    if (agent_capture_request_valid(&retry))
    {
        return 4;
    }
    return 0;
}

static int test_double_buffer_ownership(void)
{
    capture_agent_t agent;
    agent_slot_t *first;
    agent_slot_t *second;
    memset(&agent, 0, sizeof(agent));
    first = agent_select_slot(&agent);
    if (first != &agent.slots[0])
    {
        return 10;
    }
    first->state = AGENT_SLOT_SEND_QUEUED;
    if (!agent_has_unacknowledged_window(&agent))
    {
        return 16;
    }
    first->state = AGENT_SLOT_SENDING;
    if (!agent_has_unacknowledged_window(&agent))
    {
        return 17;
    }
    first->state = AGENT_SLOT_WAIT_ACK;
    if (!agent_has_unacknowledged_window(&agent))
    {
        return 14;
    }
    second = agent_select_slot(&agent);
    if (second != &agent.slots[1])
    {
        return 11;
    }
    second->state = AGENT_SLOT_WAIT_RETRANSMIT;
    if (agent_select_slot(&agent) != NULL)
    {
        return 12;
    }
    first->state = AGENT_SLOT_RETAINED;
    if (agent_select_slot(&agent) != first)
    {
        return 13;
    }
    second->state = AGENT_SLOT_RETAINED;
    if (agent_has_unacknowledged_window(&agent))
    {
        return 15;
    }
    return 0;
}

static int test_ack_transitions(void)
{
    capture_agent_t agent;
    agent_slot_t *slot;
    ra8p1_sdr_control_message_t ack;
    ra8p1_sdr_control_message_t response;
    memset(&agent, 0, sizeof(agent));
    slot = &agent.slots[0];
    make_request(&slot->request, 31U, 41U, 1U);
    slot->state = AGENT_SLOT_WAIT_ACK;
    slot->send_authorized = 1U;
    slot->last_response = slot->request;
    slot->last_response.command = RA8P1_SDR_CONTROL_CAPTURE_COMPLETE;
    slot->last_response.window_crc32c = 0x12345678U;
    slot->last_response.actual_payload_mbps_x1000 = 178000U;

    ack = slot->request;
    ack.command = RA8P1_SDR_CONTROL_WINDOW_ACK;
    ack.window_crc32c = slot->last_response.window_crc32c;
    ack.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    ack.status = RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW;
    ack.credit = 0U;
    if (!agent_apply_window_ack(&agent, slot, &ack, &response) ||
        (slot->state != AGENT_SLOT_WAIT_RETRANSMIT) ||
        (response.command != RA8P1_SDR_CONTROL_CREDIT_ACCEPTED))
    {
        return 20;
    }

    if (!agent_apply_window_ack(&agent, slot, &ack, &response) ||
        (slot->state != AGENT_SLOT_WAIT_RETRANSMIT))
    {
        return 23;
    }
    slot->ack_applied = 0U;
    slot->state = AGENT_SLOT_WAIT_ACK;
    slot->last_response = slot->request;
    slot->last_response.command = RA8P1_SDR_CONTROL_CAPTURE_COMPLETE;
    slot->last_response.window_crc32c = 0x12345678U;
    slot->last_response.actual_payload_mbps_x1000 = 178000U;
    ack.status = RA8P1_SDR_CONTROL_STATUS_OK;
    ack.credit = 1U;
    if (!agent_apply_window_ack(&agent, slot, &ack, &response) ||
        (slot->state != AGENT_SLOT_RETAINED) ||
        (agent.send_credit != 1U) ||
        (response.window_crc32c != 0x12345678U))
    {
        return 21;
    }
    slot->ack_applied = 0U;
    slot->state = AGENT_SLOT_WAIT_ACK;
    agent.send_credit = 0U;
    ack.window_crc32c++;
    if (agent_apply_window_ack(&agent, slot, &ack, &response))
    {
        return 22;
    }
    return 0;
}

static int test_boot_epoch_restart(void)
{
    capture_agent_t agent;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t restarted;
    memset(&agent, 0, sizeof(agent));
    make_request(&first, 71U, 81U, 0U);
    if (!agent_accept_epoch(&agent, &first) ||
        (agent.active_boot_epoch != first.boot_epoch) ||
        !agent_accept_epoch(&agent, &first))
    {
        return 40;
    }
    restarted = first;
    restarted.boot_epoch++;
    if (!agent_accept_epoch(&agent, &restarted) ||
        (agent.active_boot_epoch != restarted.boot_epoch) ||
        agent_accept_epoch(&agent, &first))
    {
        return 41;
    }
    return 0;
}

static int test_initial_retransmit_bootstrap(void)
{
    capture_agent_t agent;
    ra8p1_sdr_control_message_t request;

    memset(&agent, 0, sizeof(agent));
    make_request(&request, 91U, 101U, 0U);
    request.attempt = 1U;
    request.flags |= RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    if (!agent_accept_epoch(&agent, &request) ||
        (agent.active_boot_epoch != request.boot_epoch) ||
        !agent_epoch_has_no_session(&agent))
    {
        return 50;
    }
    agent.next_accept_order = 1ULL;
    if (agent_epoch_has_no_session(&agent))
    {
        return 51;
    }
    return 0;
}

static int test_credit_is_bound_to_accept_order(void)
{
    capture_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.send_credit = 1U;
    agent.send_credit_accept_order = 20ULL;
    agent.slots[0].state = AGENT_SLOT_READY;
    agent.slots[0].accept_order = 21ULL;
    agent.slots[1].state = AGENT_SLOT_READY;
    agent.slots[1].accept_order = 20ULL;
    if (agent_find_credit_ready_slot(&agent) != &agent.slots[1] ||
        !agent_slot_has_send_credit(&agent, &agent.slots[1]) ||
        agent_slot_has_send_credit(&agent, &agent.slots[0]))
    {
        return 60;
    }
    agent_release_slot_credit(&agent, &agent.slots[1]);
    if ((agent.send_credit != 0U) ||
        (agent.send_credit_accept_order != 0ULL))
    {
        return 61;
    }
    return 0;
}

static int test_cancel_rejects_late_ack_and_releases_credit(void)
{
    capture_agent_t agent;
    ra8p1_sdr_control_message_t cancel;
    ra8p1_sdr_control_message_t ack;

    memset(&agent, 0, sizeof(agent));
    make_request(&agent.slots[0].request, 111U, 121U, 0U);
    agent.slots[0].state = AGENT_SLOT_WAIT_ACK;
    agent.slots[0].accept_order = 7ULL;
    agent.send_credit = 1U;
    agent.send_credit_accept_order = 7ULL;
    cancel = agent.slots[0].request;
    cancel.command = RA8P1_SDR_CONTROL_CANCEL;
    cancel.credit = 0U;
    cancel.status = RA8P1_SDR_CONTROL_STATUS_CANCELLED;
    (void)agent_handle_cancel(&agent, &cancel);
    if ((agent.slots[0].state != AGENT_SLOT_RETAINED) ||
        (agent.send_credit != 0U))
    {
        return 70;
    }
    ack = agent.slots[0].request;
    ack.command = RA8P1_SDR_CONTROL_WINDOW_ACK;
    ack.status = RA8P1_SDR_CONTROL_STATUS_OK;
    ack.credit = 0U;
    ack.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    if (agent_window_ack_valid(&agent.slots[0], &ack))
    {
        return 71;
    }
    return 0;
}

static int test_worker_events_select_oldest_request(void)
{
    capture_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.slots[0].state = AGENT_SLOT_CAPTURING;
    agent.slots[0].capture_done = 1U;
    agent.slots[0].accept_order = 20ULL;
    agent.slots[1].state = AGENT_SLOT_CAPTURING;
    agent.slots[1].capture_done = 1U;
    agent.slots[1].accept_order = 19ULL;
    if (agent_find_oldest_capture_event(&agent) != &agent.slots[1])
    {
        return 80;
    }
    return 0;
}

static int test_wait_retransmit_timeout_is_serviced(void)
{
    capture_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    make_request(&agent.slots[0].request, 131U, 141U, 0U);
    agent.slots[0].state = AGENT_SLOT_WAIT_RETRANSMIT;
    agent.slots[0].request_deadline_us = 1ULL;
    agent.slots[0].last_control_tx_us = 0ULL;
    agent.slots[0].timeout_reported = 0U;
    if (!agent_has_unacknowledged_window(&agent))
    {
        return 90;
    }
    agent_service_timeouts(&agent);
    if (agent.slots[0].timeout_reported == 0U ||
        (agent.slots[0].last_response.command != RA8P1_SDR_CONTROL_ERROR))
    {
        return 91;
    }
    return 0;
}

static int test_accepted_send_failure_still_queues_capture(void)
{
    capture_agent_t agent;
    agent_slot_t *slot;
    ra8p1_sdr_control_message_t duplicate;

    memset(&agent, 0, sizeof(agent));
    agent.threaded = 1U;
    agent.control_socket = INVALID_SOCKET_HANDLE;
    if (pthread_cond_init(&agent.capture_cond, NULL) != 0)
    {
        return 92;
    }
    slot = &agent.slots[0];
    make_request(&slot->request, 151U, 161U, 0U);
    slot->state = AGENT_SLOT_CAPTURE_QUEUED;

    if (!agent_capture_and_send(&agent, slot, false) ||
        (slot->state != AGENT_SLOT_CAPTURE_QUEUED) ||
        (slot->last_response.command !=
         RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED))
    {
        (void)pthread_cond_destroy(&agent.capture_cond);
        return 93;
    }

    /* A duplicate is only an idempotent recovery response.  It must not
     * re-capture or abandon the queued slot merely because its reply also
     * cannot leave this synthetic agent. */
    duplicate = slot->request;
    if (agent_handle_capture_request(&agent, &duplicate) != 0 ||
        (slot->state != AGENT_SLOT_CAPTURE_QUEUED) ||
        (slot->last_response.command !=
         RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED))
    {
        (void)pthread_cond_destroy(&agent.capture_cond);
        return 94;
    }
    (void)pthread_cond_destroy(&agent.capture_cond);
    return 0;
}

static int test_credit_release_waits_for_worker_claim_before_control_reply(void)
{
    capture_agent_t agent;
    agent_slot_t *completed;
    agent_slot_t *ready;
    ra8p1_sdr_control_message_t ack;
    int handled;
    int claimed;

    memset(&agent, 0, sizeof(agent));
    agent.control_socket = INVALID_SOCKET_HANDLE;
    agent.iq_socket = INVALID_SOCKET_HANDLE;
    agent.ack_socket = INVALID_SOCKET_HANDLE;
    agent.event_pipe[0] = -1;
    agent.event_pipe[1] = -1;
    if (!agent_start_workers(&agent) || (agent.threaded == 0U))
    {
        agent_stop_workers(&agent);
        return 140;
    }

    agent_lock(&agent);
    completed = &agent.slots[0];
    ready = &agent.slots[1];
    make_request(&completed->request, 201U, 211U, 0U);
    completed->state = AGENT_SLOT_WAIT_ACK;
    completed->accept_order = 40ULL;
    completed->generation = 70ULL;
    completed->send_authorized = 1U;
    completed->last_response = completed->request;
    completed->last_response.command = RA8P1_SDR_CONTROL_CAPTURE_COMPLETE;
    completed->last_response.window_crc32c = 0x31415926U;

    make_request(&ready->request, 202U, 212U, 0U);
    ready->request.credit = 0U;
    ready->state = AGENT_SLOT_READY;
    ready->accept_order = 41ULL;
    ready->generation = 71ULL;

    ack = completed->request;
    ack.command = RA8P1_SDR_CONTROL_WINDOW_ACK;
    ack.status = RA8P1_SDR_CONTROL_STATUS_OK;
    ack.credit = 1U;
    ack.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    ack.window_crc32c = completed->last_response.window_crc32c;

    handled = agent_handle_window_ack(&agent, &ack);
    claimed = (completed->state == AGENT_SLOT_RETAINED) &&
              (ready->state == AGENT_SLOT_SENDING) &&
              (ready->send_authorized != 0U) &&
              (agent.send_credit == 0U) &&
              (agent.send_credit_accept_order == 0ULL) &&
              (agent.send_worker_slot == 1U) &&
              (agent.send_worker_generation == ready->generation);
    agent_unlock(&agent);
    agent_stop_workers(&agent);

    /* The synthetic control socket is invalid, so the final response fails.
     * The send worker must nevertheless have claimed SENDING first. */
    return ((handled == 0) && claimed) ? 0 : 141;
}

static int test_capture_request_trace_is_recorded(void)
{
    capture_agent_t agent;
    ra8p1_sdr_control_message_t request;
    FILE *stream;
    char output[2048];
    size_t count;

    memset(&agent, 0, sizeof(agent));
    make_request(&request, 171U, 181U, 1U);
    stream = tmpfile();
    if (stream == NULL)
    {
        return 95;
    }
    agent.diagnostics_enabled = 1U;
    agent.diagnostics_stream = stream;
    agent.control_socket = INVALID_SOCKET_HANDLE;

    /* Capacity is intentionally absent: the request is rejected after the
     * receive trace, without touching SDR IIO hardware in this host test. */
    (void)agent_handle_capture_request(&agent, &request);
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_SET) != 0)
    {
        (void)fclose(stream);
        return 96;
    }
    count = fread(output, 1U, sizeof(output) - 1U, stream);
    output[count] = '\0';
    (void)fclose(stream);
    if ((strstr(output, "direction=rx event=CAPTURE_REQ") == NULL) ||
        (strstr(output, "request=171 session=181") == NULL) ||
        (strstr(output, "slot=-1") == NULL))
    {
        return 97;
    }
    return 0;
}

static int test_optional_trace_summary(void)
{
    capture_agent_t agent;
    agent_slot_t slot;
    FILE *stream;
    char output[4096];
    size_t count;
    static const char *const required_fields[] =
    {
        "request=171",
        "session=181",
        "center_index=3",
        "send_batch=16",
        "target_mbps=600",
        "tune_elapsed_us=20",
        "capture_elapsed_us=880",
        "send_elapsed_us=27123",
        "pacing_rebases=7",
        "pacing_max_late_us=88",
        "transport=udp_gso",
        "gso_requested=1",
        "gso_attempts=103",
        "gso_batches=102",
        "gso_packets=1632",
        "gso_fallbacks=1",
        "gso_ineligible=2",
        "gso_errno=22",
        "adapter_flags=0x00000017",
        "last_capture_tune_guarded=1",
        "adapter_block_setup_us=100",
        "adapter_pre_enable_us=1000",
        "adapter_dma_wait_us=10000",
        "fastlock_profiles=4",
        "fastlock_recall_count=12",
        "fallback_count=2"
    };
    size_t index;

    memset(&agent, 0, sizeof(agent));
    memset(&slot, 0, sizeof(slot));
    make_request(&slot.request, 171U, 181U, 3U);
    slot.request.send_batch = 16U;
    slot.request.target_payload_mbps_x1000 = 600000U;
    slot.request.agent_request_rx_us = 90ULL;
    slot.tune_start_us = 100ULL;
    slot.tune_complete_us = 120ULL;
    slot.capture_start_us = 130ULL;
    slot.capture_complete_us = 1010ULL;
    slot.send_complete_us = 30000ULL;
    slot.capture_status = RA8P1_SDR_CONTROL_STATUS_OK;
    slot.send_status = RA8P1_SDR_CONTROL_STATUS_OK;
    slot.send_result.data_packets = 1640U;
    slot.send_result.logical_udp_packets = 1642U;
    slot.send_result.payload_bytes = 2361344ULL;
    slot.send_result.elapsed_us = 27123ULL;
    slot.send_result.crc32c = 0x12345678U;
    slot.send_result.pacing_rebases = 7U;
    slot.send_result.pacing_max_late_us = 88ULL;
    slot.send_result.udp_gso_requested = 1U;
    slot.send_result.udp_gso_attempts = 103U;
    slot.send_result.udp_gso_batches = 102U;
    slot.send_result.udp_gso_packets = 1632U;
    slot.send_result.udp_gso_fallbacks = 1U;
    slot.send_result.udp_gso_ineligible_batches = 2U;
    slot.send_result.udp_gso_last_errno = 22U;
    slot.adapter_status_valid = 1U;
    slot.adapter_status.flags =
        RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_SUPPORTED |
        RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY |
        RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK |
        RA8P1_SDR_ADAPTER_STATUS_LAST_CAPTURE_TUNE_GUARDED;
    slot.adapter_status.fastlock_profiles = 4U;
    slot.adapter_status.fastlock_recall_count = 12U;
    slot.adapter_status.fallback_count = 2U;
    slot.adapter_status.tune_count = 14U;
    slot.adapter_status.tune_start_ns = 1000000000ULL;
    slot.adapter_status.tune_complete_ns = 1014000000ULL;
    slot.adapter_status.capture_prepare_ns = 2000000000ULL;
    slot.adapter_status.blocks_ready_ns = 2000100000ULL;
    slot.adapter_status.buffer_enable_ns = 2001100000ULL;
    slot.adapter_status.block_dequeue_ns = 2011100000ULL;
    slot.adapter_status.buffer_disable_ns = 2011200000ULL;
    slot.adapter_status.copy_complete_ns = 2012200000ULL;
    slot.adapter_status.last_tune_status = 0;
    agent.sdr.api.name = "mock-adapter";
    stream = tmpfile();
    if (stream == NULL)
    {
        return 100;
    }
    agent.diagnostics_stream = stream;

    /* The default is silent, including no formatting or flush side effect. */
    agent_trace_window(&agent, &slot);
    if (ftell(stream) != 0L)
    {
        (void)fclose(stream);
        return 101;
    }

    agent.diagnostics_enabled = 1U;
    agent_trace_window(&agent, &slot);
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_SET) != 0)
    {
        (void)fclose(stream);
        return 102;
    }
    count = fread(output, 1U, sizeof(output) - 1U, stream);
    output[count] = '\0';
    (void)fclose(stream);
    for (index = 0U;
         index < (sizeof(required_fields) / sizeof(required_fields[0]));
         ++index)
    {
        if (strstr(output, required_fields[index]) == NULL)
        {
            return 103 + (int)index;
        }
    }
    return 0;
}

static int test_control_event_trace(void)
{
    capture_agent_t agent;
    ra8p1_sdr_control_message_t message;
    FILE *stream;
    char output[8192];
    size_t count;
    size_t index;
    static const uint16_t commands[] =
    {
        RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
        RA8P1_SDR_CONTROL_CAPTURE_STARTED,
        RA8P1_SDR_CONTROL_CAPTURE_READY,
        RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
        RA8P1_SDR_CONTROL_WINDOW_ACK,
        RA8P1_SDR_CONTROL_CREDIT_ACCEPTED
    };
    static const char *const events[] =
    {
        "event=CAPTURE_ACCEPTED",
        "event=CAPTURE_STARTED",
        "event=CAPTURE_READY",
        "event=CAPTURE_COMPLETE",
        "direction=rx event=WINDOW_ACK",
        "event=CREDIT_ACCEPTED"
    };

    memset(&agent, 0, sizeof(agent));
    make_request(&agent.slots[1].request, 191U, 201U, 2U);
    agent.slots[1].state = AGENT_SLOT_WAIT_ACK;
    stream = tmpfile();
    if (stream == NULL)
    {
        return 120;
    }
    agent.diagnostics_enabled = 1U;
    agent.diagnostics_stream = stream;
    for (index = 0U; index < (sizeof(commands) / sizeof(commands[0]));
         ++index)
    {
        message = agent.slots[1].request;
        message.command = commands[index];
        message.status = RA8P1_SDR_CONTROL_STATUS_OK;
        message.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
        agent_trace_control_event(
            &agent, &agent.slots[1], &message,
            (commands[index] == RA8P1_SDR_CONTROL_WINDOW_ACK) ? "rx" : "tx");
    }
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_SET) != 0)
    {
        (void)fclose(stream);
        return 121;
    }
    count = fread(output, 1U, sizeof(output) - 1U, stream);
    output[count] = '\0';
    (void)fclose(stream);
    if ((strstr(output, "request=191") == NULL) ||
        (strstr(output, "session=201") == NULL) ||
        (strstr(output, "slot=1") == NULL) ||
        (strstr(output, "status=0") == NULL))
    {
        return 122;
    }
    for (index = 0U; index < (sizeof(events) / sizeof(events[0])); ++index)
    {
        if (strstr(output, events[index]) == NULL)
        {
            return 123 + (int)index;
        }
    }
    return 0;
}

static int test_wire_crc(void)
{
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t decoded;
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    make_request(&request, 51U, 61U, 3U);
    if (!ra8p1_sdr_control_encode(&request, wire) ||
        !ra8p1_sdr_control_decode(wire, sizeof(wire), &decoded) ||
        !agent_same_contract(&request, &decoded))
    {
        return 30;
    }
    wire[40] ^= 1U;
    if (ra8p1_sdr_control_decode(wire, sizeof(wire), &decoded))
    {
        return 31;
    }
    return 0;
}

int main(void)
{
    int status = 0;

#define RUN_TEST(test_function)                                                \
    do                                                                         \
    {                                                                          \
        if (status == 0)                                                       \
        {                                                                      \
            fprintf(stderr, "RUN %s\n", #test_function);                     \
            fflush(stderr);                                                    \
            status = test_function();                                          \
            if (status == 0)                                                   \
            {                                                                  \
                fprintf(stderr, "PASS %s\n", #test_function);                \
                fflush(stderr);                                                \
            }                                                                  \
        }                                                                      \
    } while (0)

    RUN_TEST(test_validation_and_identity);
    RUN_TEST(test_double_buffer_ownership);
    RUN_TEST(test_ack_transitions);
    RUN_TEST(test_wire_crc);
    RUN_TEST(test_boot_epoch_restart);
    RUN_TEST(test_initial_retransmit_bootstrap);
    RUN_TEST(test_credit_is_bound_to_accept_order);
    RUN_TEST(test_cancel_rejects_late_ack_and_releases_credit);
    RUN_TEST(test_worker_events_select_oldest_request);
    RUN_TEST(test_wait_retransmit_timeout_is_serviced);
    RUN_TEST(test_accepted_send_failure_still_queues_capture);
    RUN_TEST(test_credit_release_waits_for_worker_claim_before_control_reply);
    RUN_TEST(test_capture_request_trace_is_recorded);
    RUN_TEST(test_optional_trace_summary);
    RUN_TEST(test_control_event_trace);

#undef RUN_TEST
    if (status != 0)
    {
        fprintf(stderr, "SDR capture agent state test failed: %d\n", status);
        return status;
    }
    puts("SDRC passive agent validation, idempotence, double-buffer, ACK and CRC tests passed");
    return 0;
}
