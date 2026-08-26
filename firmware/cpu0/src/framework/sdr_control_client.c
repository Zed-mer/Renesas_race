#include "sdr_control_client.h"

#include <string.h>

static bool sdr_control_elapsed(uint32_t now_ms,
                                uint32_t then_ms,
                                uint32_t interval_ms)
{
    return (uint32_t)(now_ms - then_ms) >= interval_ms;
}

static uint32_t sdr_control_next_nonzero(uint32_t *counter)
{
    *counter += 1U;
    if (*counter == 0U)
    {
        *counter = 1U;
    }
    return *counter;
}

static bool sdr_control_options_valid(
    const sdr_control_capture_options_t *options)
{
    bool low_latency;
    bool compat;
    if (options == NULL)
    {
        return false;
    }
    low_latency =
        (options->flags & RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY) != 0U;
    compat =
        (options->flags & RA8P1_SDR_CONTROL_FLAG_COMPAT_6M) != 0U;
    if ((options->sample_rate_hz != RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE) ||
        (options->bandwidth_hz != RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH) ||
        (options->target_payload_mbps_x1000 <
         RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000) ||
        (options->target_payload_mbps_x1000 >
         RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000) ||
        ((options->target_payload_mbps_x1000 % 1000U) != 0U) ||
        (options->send_batch == 0U) || (options->send_batch > 64U) ||
        (options->retry_limit > 32U) ||
        (options->ack_timeout_ms == 0U) ||
        (options->request_timeout_ms < options->ack_timeout_ms) ||
        ((options->flags & ~RA8P1_SDR_CONTROL_FLAG_ALL) != 0U) ||
        ((options->test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) != 0U) ||
        (low_latency == compat))
    {
        return false;
    }
    if (low_latency)
    {
        return options->sample_count == RA8P1_SDR_CONTROL_DEFAULT_SAMPLES;
    }
    return options->sample_count == RA8P1_SDR_CONTROL_COMPAT_SAMPLES;
}

static bool sdr_control_state_active(uint32_t state)
{
    return ((state >= SDR_CONTROL_CLIENT_WAIT_ACCEPTED) &&
            (state <= SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED)) ||
           (state == SDR_CONTROL_CLIENT_WAIT_CANCELLED) ||
           (state == SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
}

static bool sdr_control_identity_matches(
    const ra8p1_sdr_control_message_t *left,
    const ra8p1_sdr_control_message_t *right)
{
    return (left != NULL) && (right != NULL) &&
           (left->boot_epoch == right->boot_epoch) &&
           (left->request_id == right->request_id) &&
           (left->session_id == right->session_id) &&
           (left->center_index == right->center_index) &&
           (left->center_frequency_hz == right->center_frequency_hz);
}

static bool sdr_control_cancel_target_matches(
    const ra8p1_sdr_control_message_t *left,
    const ra8p1_sdr_control_message_t *right)
{
    return sdr_control_identity_matches(left, right) &&
           (left->attempt == right->attempt);
}

static bool sdr_control_send_message(sdr_control_client_t *client,
                                     ra8p1_sdr_control_message_t *message,
                                     uint32_t now_ms)
{
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    if ((client == NULL) || (message == NULL) ||
        (client->transport.send == NULL) ||
        !ra8p1_sdr_control_encode(message, wire))
    {
        return false;
    }
    message->message_crc32c = ra8p1_sdr_control_get_le32(
        &wire[RA8P1_SDR_CONTROL_CRC_OFFSET]);
    if (!client->transport.send(client->transport.context,
                                wire,
                                sizeof(wire)))
    {
        return false;
    }
    client->stats.last_tx_ms = now_ms;
    client->stats.last_message_crc32c = message->message_crc32c;
    client->stats.tx_datagrams++;
    return true;
}

static void sdr_control_prepare_capture_request(
    sdr_control_client_t *client,
    ra8p1_sdr_control_message_t *request,
    uint32_t center_index,
    uint32_t credit)
{
    memset(request, 0, sizeof(*request));
    request->command = RA8P1_SDR_CONTROL_CAPTURE_REQ;
    request->flags = client->options.flags;
    request->request_id = sdr_control_next_nonzero(&client->next_request_id);
    request->session_id = sdr_control_next_nonzero(&client->next_session_id);
    request->center_index = center_index;
    request->center_frequency_hz =
        ra8p1_sdr_control_center_frequency(center_index);
    request->sample_rate_hz = client->options.sample_rate_hz;
    request->bandwidth_hz = client->options.bandwidth_hz;
    request->sample_count = client->options.sample_count;
    request->target_payload_mbps_x1000 =
        client->options.target_payload_mbps_x1000;
    request->send_batch = client->options.send_batch;
    request->retry_limit = client->options.retry_limit;
    request->ack_timeout_ms = client->options.ack_timeout_ms;
    request->request_timeout_ms = client->options.request_timeout_ms;
    request->test_fault_flags = client->options.test_fault_flags;
    request->credit = credit;
    request->ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    request->status = RA8P1_SDR_CONTROL_STATUS_OK;
    request->boot_epoch = client->boot_epoch;
}

static void sdr_control_make_capture_request(sdr_control_client_t *client,
                                             uint32_t center_index)
{
    sdr_control_prepare_capture_request(client,
                                        &client->active_request,
                                        center_index,
                                        1U);
    client->current_center_index = center_index;
    client->agent_complete = 0U;
    client->agent_complete_ms = 0U;
    client->iqsc_started = 0U;
    client->pending_retransmit = 0U;
    client->request_sent = 0U;
    client->active_control_retries = 0U;
    client->active_from_prefetch = 0U;
    client->expect_prefetched_iq = 0U;
    client->active_result_ready = 0U;
    client->active_data_valid = 0U;
    client->iqsc_end_ms = 0U;
    client->iqsc_end_seen = 0U;
    client->missing_complete_counted = 0U;
    client->fast_complete_probe_sent = 0U;
    client->completion_probe_last_tx_ms = 0U;
    client->completion_probe_retries = 0U;
}

static void sdr_control_prefetch_clear(sdr_control_client_t *client)
{
    memset(&client->prefetched_request, 0,
           sizeof(client->prefetched_request));
    client->prefetch_valid = 0U;
    client->prefetch_ready = 0U;
    client->prefetch_request_sent = 0U;
    client->prefetch_control_retries = 0U;
    client->prefetch_state = SDR_CONTROL_CLIENT_IDLE;
    client->prefetch_agent_complete = 0U;
    client->prefetch_agent_complete_ms = 0U;
    client->stats.prefetched_request_id = 0U;
    client->stats.prefetched_session_id = 0U;
    client->stats.prefetched_center_index = 0U;
    client->stats.prefetch_state = SDR_CONTROL_CLIENT_IDLE;
}

static void sdr_control_fallback_clear(sdr_control_client_t *client)
{
    if (client == NULL)
    {
        return;
    }
    memset(&client->fallback_cancel_request, 0,
           sizeof(client->fallback_cancel_request));
    client->fallback_center_index = 0U;
    client->fallback_cancel_last_tx_ms = 0U;
    client->fallback_cancel_generation = 0U;
    client->fallback_cancel_retries = 0U;
    client->prefetch_abandoned = 0U;
    client->fallback_pending = 0U;
    client->fallback_cancel_pending = 0U;
    client->fallback_cancel_confirmed = 0U;
    client->fallback_wait_credit = 0U;
}

static void sdr_control_terminal_cancel_clear(sdr_control_client_t *client)
{
    if (client == NULL)
    {
        return;
    }
    memset(client->terminal_cancel_requests,
           0,
           sizeof(client->terminal_cancel_requests));
    client->terminal_cancel_last_tx_ms = 0U;
    client->terminal_cancel_count = 0U;
    client->terminal_cancel_index = 0U;
    client->terminal_cancel_retries = 0U;
    client->terminal_cancel_tx_succeeded = 0U;
}

static bool sdr_control_terminal_cancel_append(
    sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *request)
{
    uint32_t index;

    if ((client == NULL) || (request == NULL) ||
        (request->request_id == 0U) || (request->session_id == 0U))
    {
        return false;
    }
    for (index = 0U; index < client->terminal_cancel_count; ++index)
    {
        if (sdr_control_cancel_target_matches(
                request, &client->terminal_cancel_requests[index]))
        {
            return true;
        }
    }
    if (client->terminal_cancel_count >=
        SDR_CONTROL_TERMINAL_CANCEL_TARGETS)
    {
        return false;
    }
    client->terminal_cancel_requests[client->terminal_cancel_count] =
        *request;
    client->terminal_cancel_count++;
    return true;
}

/* CANCEL is idempotent for one generation.  A later post-credit CANCEL uses a
 * new attempt value so a delayed pre-credit CANCELLED response cannot release
 * the tombstone early.  The SDR agent echoes this field verbatim. */
static void sdr_control_new_fallback_cancel_generation(
    sdr_control_client_t *client)
{
    if (client == NULL)
    {
        return;
    }
    client->fallback_cancel_generation++;
    if (client->fallback_cancel_generation == 0U)
    {
        client->fallback_cancel_generation = 1U;
    }
    client->fallback_cancel_request.attempt =
        client->fallback_cancel_generation;
    client->fallback_cancel_pending = 0U;
    client->fallback_cancel_confirmed = 0U;
    client->fallback_cancel_retries = 0U;
}

static bool sdr_control_send_cancel_for_request(
    sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *request,
    uint32_t now_ms)
{
    ra8p1_sdr_control_message_t cancel;

    if ((client == NULL) || (request == NULL) ||
        (request->request_id == 0U) || (request->session_id == 0U))
    {
        return false;
    }
    cancel = *request;
    cancel.command = RA8P1_SDR_CONTROL_CANCEL;
    cancel.credit = 0U;
    cancel.status = RA8P1_SDR_CONTROL_STATUS_CANCELLED;
    return sdr_control_send_message(client, &cancel, now_ms);
}

static bool sdr_control_terminal_cancel_send_current(
    sdr_control_client_t *client,
    uint32_t now_ms)
{
    bool sent;

    if ((client == NULL) ||
        (client->stats.state !=
         SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED) ||
        (client->terminal_cancel_index >= client->terminal_cancel_count))
    {
        return false;
    }
    client->terminal_cancel_last_tx_ms = now_ms;
    sent = sdr_control_send_cancel_for_request(
        client,
        &client->terminal_cancel_requests[client->terminal_cancel_index],
        now_ms);
    if (sent)
    {
        client->terminal_cancel_tx_succeeded = 1U;
    }
    return sent;
}

static void sdr_control_terminal_cancel_finish(
    sdr_control_client_t *client)
{
    if (client == NULL)
    {
        return;
    }
    sdr_control_prefetch_clear(client);
    sdr_control_fallback_clear(client);
    memset(&client->pending_ack, 0, sizeof(client->pending_ack));
    memset(&client->credit_proof_request,
           0,
           sizeof(client->credit_proof_request));
    client->credit_proof_pending = 0U;
    client->pending_retransmit = 0U;
    client->expect_prefetched_iq = 0U;
    client->repeat_scan = 0U;
    client->scan_all = 0U;
    sdr_control_terminal_cancel_clear(client);
    client->stats.state = SDR_CONTROL_CLIENT_CANCELLED;
    client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_CANCELLED;
}

static void sdr_control_terminal_cancel_record_response(
    sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *response,
    uint32_t now_ms)
{
    client->last_response = *response;
    client->last_rx_ms = now_ms;
    client->stats.last_rx_ms = now_ms;
    client->stats.rx_datagrams++;
    client->stats.last_status = response->status;
    client->stats.last_message_crc32c = response->message_crc32c;
}

static bool sdr_control_receive_terminal_cancel(
    sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *response,
    uint32_t now_ms)
{
    const ra8p1_sdr_control_message_t *current;
    uint32_t index;
    bool known_identity = false;

    if ((client == NULL) || (response == NULL) ||
        (client->stats.state !=
         SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED) ||
        (client->terminal_cancel_index >= client->terminal_cancel_count))
    {
        return false;
    }
    current = &client->terminal_cancel_requests[
        client->terminal_cancel_index];
    for (index = 0U; index < client->terminal_cancel_count; ++index)
    {
        if (sdr_control_identity_matches(
                response, &client->terminal_cancel_requests[index]))
        {
            known_identity = true;
            break;
        }
    }

    if ((response->command == RA8P1_SDR_CONTROL_ERROR) &&
        (response->status == RA8P1_SDR_CONTROL_STATUS_CANCELLED) &&
        sdr_control_identity_matches(response, current))
    {
        sdr_control_terminal_cancel_record_response(client, response, now_ms);
        if (response->attempt != current->attempt)
        {
            /* A fallback cancellation may have used the same identity with a
             * different generation.  Its delayed confirmation cannot advance
             * this ordered terminal transaction. */
            client->stats.invalid_datagrams++;
            return true;
        }
        client->terminal_cancel_index++;
        if (client->terminal_cancel_index >= client->terminal_cancel_count)
        {
            sdr_control_terminal_cancel_finish(client);
        }
        else
        {
            /* The dependent request is cancelled only after active CANCELLED.
             * This is the post-credit fence for a delayed credit=1 ACK. */
            client->terminal_cancel_retries = 0U;
            client->terminal_cancel_tx_succeeded = 0U;
            (void)sdr_control_terminal_cancel_send_current(client, now_ms);
        }
        return true;
    }

    if (known_identity ||
        sdr_control_identity_matches(response, &client->pending_ack) ||
        sdr_control_identity_matches(response,
                                     &client->credit_proof_request))
    {
        /* CREDIT_ACCEPTED and terminal CAPTURE_* responses can cross CANCEL on
         * UDP.  They are evidence about an old owner only; ordered CANCELLED
         * confirmations remain the sole completion condition. */
        sdr_control_terminal_cancel_record_response(client, response, now_ms);
        return true;
    }
    client->stats.invalid_datagrams++;
    return false;
}

static bool sdr_control_credit_grant_pending(
    const sdr_control_client_t *client)
{
    return (client != NULL) &&
           (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED) &&
           (client->pending_ack.command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
           (client->pending_ack.credit != 0U);
}

static bool sdr_control_send_fallback_cancel(sdr_control_client_t *client,
                                             uint32_t now_ms)
{
    bool sent;

    if ((client == NULL) || (client->fallback_pending == 0U))
    {
        return false;
    }
    client->fallback_cancel_last_tx_ms = now_ms;
    client->fallback_cancel_pending = 1U;
    sent = sdr_control_send_cancel_for_request(
        client, &client->fallback_cancel_request, now_ms);
    return sent;
}

/* Preserve the request identity after a speculative failure.  A CANCEL sent
 * before the active WINDOW_ACK is not sufficient: the SDR may process the
 * WINDOW_ACK later and bind its newly granted credit to this already-cancelled
 * accept-order.  The tombstone is retained until a post-credit CANCEL is
 * acknowledged, so serial fallback can never race an orphan token. */
static void sdr_control_abandon_prefetch(sdr_control_client_t *client,
                                         uint32_t now_ms)
{
    if ((client == NULL) || (client->prefetch_valid == 0U) ||
        (client->fallback_pending != 0U))
    {
        return;
    }
    client->fallback_cancel_request = client->prefetched_request;
    client->fallback_center_index = client->prefetched_request.center_index;
    client->prefetch_abandoned = 1U;
    client->fallback_pending = 1U;
    client->fallback_wait_credit =
        sdr_control_credit_grant_pending(client) ? 1U : 0U;
    client->fallback_cancel_confirmed = 0U;
    client->fallback_cancel_retries = 0U;
    client->fallback_cancel_generation = 0U;
    client->expect_prefetched_iq = 0U;
    client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
    client->stats.prefetch_state = client->prefetch_state;
    /* The active client no longer uses this slot for IQ matching, but its
     * identity remains in fallback_cancel_request for late CANCEL/ERROR. */
    sdr_control_prefetch_clear(client);
    sdr_control_new_fallback_cancel_generation(client);
    (void)sdr_control_send_fallback_cancel(client, now_ms);
}

static bool sdr_control_send_terminal_cancels(sdr_control_client_t *client,
                                               uint32_t now_ms)
{
    bool sent;
    bool all_sent;

    if (client == NULL)
    {
        return false;
    }
    all_sent = sdr_control_send_cancel_for_request(
        client, &client->active_request, now_ms);
    if (client->prefetch_valid != 0U)
    {
        sent = sdr_control_send_cancel_for_request(
            client, &client->prefetched_request, now_ms);
        all_sent = sent && all_sent;
    }
    if (client->fallback_pending != 0U)
    {
        sent = sdr_control_send_cancel_for_request(
            client, &client->fallback_cancel_request, now_ms);
        all_sent = sent && all_sent;
    }
    /* This helper is used only as best-effort cleanup when an unrelated
     * operation fails.  Preserve every identity even when all datagrams reach
     * the local UDP stack: a later explicit STOP must still perform ordered,
     * confirmed cancellation. */
    return all_sent;
}

static void sdr_control_fail(sdr_control_client_t *client,
                             uint32_t status,
                             uint32_t now_ms)
{
    if (client == NULL)
    {
        return;
    }
    /* Best-effort cleanup is sent before ERROR becomes terminal locally.  It
     * releases an SDR WAIT_ACK/WAIT_RETRANSMIT slot even if CPU1 never issues
     * a later explicit cancel. */
    (void)sdr_control_send_terminal_cancels(client, now_ms);
    client->stats.state = SDR_CONTROL_CLIENT_ERROR;
    client->stats.last_status = status;
}

static bool sdr_control_request_cached_retry(sdr_control_client_t *client,
                                             uint32_t now_ms)
{
    if ((client == NULL) ||
        (client->active_request.attempt >= client->options.retry_limit))
    {
        return false;
    }
    client->pending_ack = client->active_request;
    client->pending_ack.command = RA8P1_SDR_CONTROL_WINDOW_ACK;
    client->pending_ack.flags &=
        (uint16_t)~RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    client->pending_ack.credit = 0U;
    client->pending_ack.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    client->pending_ack.status = RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW;
    client->pending_ack.attempt = 0U;
    client->pending_ack.window_crc32c = 0U;
    client->pending_ack.sequence_gaps = 1U;
    client->pending_ack.reordered = 0U;
    client->pending_ack.invalid_packets = 0U;
    client->pending_ack.ring_full_drops = 0U;
    client->pending_ack.ring_oversize_drops = 0U;
    client->pending_ack.crc_errors = 0U;
    client->pending_retransmit = 1U;
    client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW;
    client->last_tx_ms = now_ms;
    if (!sdr_control_send_message(client, &client->pending_ack, now_ms))
    {
        client->pending_retransmit = 0U;
        sdr_control_fail(client,
                         RA8P1_SDR_CONTROL_STATUS_SEND_FAILED,
                         now_ms);
        return false;
    }
    client->stats.state = SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED;
    return true;
}

static bool sdr_control_send_capture_request(sdr_control_client_t *client,
                                             uint32_t now_ms)
{
    bool sent;

    if (client == NULL)
    {
        return false;
    }

    /* Publish the request identity before transport. A transient link or ARP
     * failure must not discard the CPU0-owned request/session. */
    client->request_start_ms = now_ms;
    client->stats.request_start_ms = now_ms;
    client->stats.request_id = client->active_request.request_id;
    client->stats.session_id = client->active_request.session_id;
    client->stats.center_index = client->active_request.center_index;
    client->stats.state = SDR_CONTROL_CLIENT_WAIT_ACCEPTED;
    client->last_tx_ms = now_ms;

    sent = sdr_control_send_message(client, &client->active_request, now_ms);
    if (!sent)
    {
        client->last_tx_ms = now_ms;
        client->request_sent = 0U;
        client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
        return true;
    }
    client->request_sent = 1U;
    return true;
}

static bool sdr_control_send_prefetch_request(sdr_control_client_t *client,
                                               uint32_t now_ms)
{
    uint32_t previous_state;
    bool sent;

    previous_state = client->prefetch_state;
    client->prefetch_last_tx_ms = now_ms;
    if ((previous_state != SDR_CONTROL_CLIENT_WAIT_STARTED) &&
        (previous_state != SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT) &&
        (previous_state != SDR_CONTROL_CLIENT_RECEIVING))
    {
        client->prefetch_state = SDR_CONTROL_CLIENT_WAIT_ACCEPTED;
    }
    client->stats.prefetch_state = client->prefetch_state;
    sent = sdr_control_send_message(client,
                                    &client->prefetched_request,
                                    now_ms);
    /* A failed duplicate send cannot revoke an earlier CAPTURE_ACCEPTED proof.
     * Keep both the strongest observed state and the fact that at least one
     * copy reached the local UDP stack. */
    client->prefetch_request_sent =
        sent ? 1U : client->prefetch_request_sent;
    return true;
}

static bool sdr_control_next_prefetch_center(
    const sdr_control_client_t *client,
    uint32_t *center_index)
{
    if ((client == NULL) || (center_index == NULL) ||
        (client->current_center_index >= RA8P1_SDR_CONTROL_CENTER_COUNT))
    {
        return false;
    }
    if (client->scan_all != 0U)
    {
        if (client->current_center_index >=
            (RA8P1_SDR_CONTROL_CENTER_COUNT - 1U))
        {
            if (client->repeat_scan == 0U)
            {
                return false;
            }
            *center_index = 0U;
            return true;
        }
        *center_index = client->current_center_index + 1U;
        return true;
    }
    if (client->repeat_scan != 0U)
    {
        *center_index = client->current_center_index;
        return true;
    }
    return false;
}

static bool sdr_control_start_prefetch(sdr_control_client_t *client,
                                       uint32_t now_ms)
{
    uint32_t center_index;
    if ((client == NULL) ||
        (client->prefetch_valid != 0U) ||
        (client->fallback_pending != 0U) ||
        (client->prefetch_abandoned != 0U) ||
        !sdr_control_next_prefetch_center(client, &center_index))
    {
        return false;
    }
    sdr_control_prepare_capture_request(client,
                                        &client->prefetched_request,
                                        center_index,
                                        0U);
    client->prefetch_valid = 1U;
    client->prefetch_ready = 0U;
    client->prefetch_request_sent = 0U;
    client->prefetch_request_start_ms = now_ms;
    client->stats.prefetched_request_id =
        client->prefetched_request.request_id;
    client->stats.prefetched_session_id =
        client->prefetched_request.session_id;
    client->stats.prefetched_center_index = center_index;
    return sdr_control_send_prefetch_request(client, now_ms);
}

static bool sdr_control_start(sdr_control_client_t *client,
                              uint32_t center_index,
                              bool scan_all,
                              bool repeat_scan,
                              const sdr_control_capture_options_t *options,
                              uint32_t now_ms)
{
    if ((client == NULL) || (center_index >= RA8P1_SDR_CONTROL_CENTER_COUNT) ||
        (options == NULL) || !sdr_control_options_valid(options) ||
        sdr_control_state_active(client->stats.state) ||
        (client->stats.state == SDR_CONTROL_CLIENT_ERROR))
    {
        return false;
    }
    client->options = *options;
    client->scan_all = scan_all ? 1U : 0U;
    client->repeat_scan = repeat_scan ? 1U : 0U;
    client->stats.completed_windows = 0U;
    client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_OK;
    client->stats.actual_payload_mbps_x1000 = 0U;
    client->stats.window_crc32c = 0U;
    client->stats.prefetched_windows = 0U;
    client->stats.prefetch_iqsc_credit_proofs = 0U;
    client->credit_proof_pending = 0U;
    memset(&client->credit_proof_request, 0,
           sizeof(client->credit_proof_request));
    sdr_control_terminal_cancel_clear(client);
    sdr_control_fallback_clear(client);
    sdr_control_prefetch_clear(client);
    sdr_control_make_capture_request(client, center_index);
    return sdr_control_send_capture_request(client, now_ms);
}

static bool sdr_control_retry_request(sdr_control_client_t *client,
                                      uint32_t now_ms)
{
    if (client->active_request.attempt >= client->options.retry_limit)
    {
        const uint32_t status = (client->request_sent == 0U) ?
            RA8P1_SDR_CONTROL_STATUS_SEND_FAILED :
            RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT;
        sdr_control_fail(client, status, now_ms);
        return false;
    }
    client->active_request.attempt++;
    client->active_request.flags |= RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    client->stats.retries++;
    client->agent_complete = 0U;
    client->agent_complete_ms = 0U;
    client->iqsc_started = 0U;
    client->active_result_ready = 0U;
    client->active_data_valid = 0U;
    client->iqsc_end_ms = 0U;
    client->iqsc_end_seen = 0U;
    client->missing_complete_counted = 0U;
    client->fast_complete_probe_sent = 0U;
    client->completion_probe_last_tx_ms = 0U;
    client->completion_probe_retries = 0U;
    return sdr_control_send_capture_request(client, now_ms);
}

/* A missing control response is not evidence that the SDR cached a window.
 * Repeat the identical request/attempt so an existing slot answers
 * idempotently and an actually-lost datagram can be accepted as new work. */
static bool sdr_control_resend_active_request(sdr_control_client_t *client,
                                              uint32_t now_ms)
{
    bool sent;

    if (client == NULL)
    {
        return false;
    }
    if (client->active_control_retries >= client->options.retry_limit)
    {
        const uint32_t status = (client->request_sent == 0U) ?
            RA8P1_SDR_CONTROL_STATUS_SEND_FAILED :
            RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT;
        sdr_control_fail(client, status, now_ms);
        return false;
    }
    client->active_control_retries++;
    client->stats.retries++;
    client->last_tx_ms = now_ms;
    sent = sdr_control_send_message(client, &client->active_request, now_ms);
    client->request_sent = sent ? 1U : client->request_sent;
    if (!sent)
    {
        client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
    }
    return true;
}

static uint32_t sdr_control_completion_probe_interval_ms(
    uint8_t retry_count)
{
    if (retry_count == 0U)
    {
        return SDR_CONTROL_COMPLETION_PROBE_1_MS;
    }
    if (retry_count == 1U)
    {
        return SDR_CONTROL_COMPLETION_PROBE_2_MS;
    }
    return SDR_CONTROL_COMPLETION_PROBE_MAX_MS;
}

/* This is a completion-state probe, not a delivery retry.  The SDR agent
 * treats an identical request/session/attempt as idempotent and returns its
 * cached state.  Keep this retry clock independent from the active request
 * and ACK retry clocks: a lost CAPTURE_COMPLETE must be recovered promptly,
 * without consuming the delivery retry budget or waiting one ACK timeout. */
static bool sdr_control_completion_probe_active_request(
    sdr_control_client_t *client,
    uint32_t now_ms)
{
    bool sent;

    if (client == NULL)
    {
        return false;
    }
    client->fast_complete_probe_sent = 1U;
    client->completion_probe_last_tx_ms = now_ms;
    client->last_tx_ms = now_ms;
    sent = sdr_control_send_message(client, &client->active_request, now_ms);
    client->request_sent = sent ? 1U : client->request_sent;
    if (!sent)
    {
        client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
    }
    return sent;
}

static bool sdr_control_retry_prefetch(sdr_control_client_t *client,
                                       uint32_t now_ms)
{
    if ((client == NULL) || (client->prefetch_valid == 0U))
    {
        return false;
    }
    if (client->prefetch_control_retries >= client->options.retry_limit)
    {
        client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
        client->stats.prefetch_state = client->prefetch_state;
        return false;
    }
    client->prefetch_control_retries++;
    client->stats.retries++;
    return sdr_control_send_prefetch_request(client, now_ms);
}

static bool sdr_control_has_next_center(const sdr_control_client_t *client)
{
    uint32_t center_index;
    return sdr_control_next_prefetch_center(client, &center_index);
}

static bool sdr_control_prefetch_promotable(
    const sdr_control_client_t *client)
{
    if ((client == NULL) || (client->prefetch_valid == 0U) ||
        (client->prefetch_request_sent == 0U) ||
        (client->prefetch_state == SDR_CONTROL_CLIENT_ERROR))
    {
        return false;
    }
    /* CREDIT_ACCEPTED only proves that the active WINDOW_ACK was accepted;
     * it does not prove that this speculative CAPTURE_REQ reached the SDR.
     * Require an explicit response for the prefetched request (or an IQSC
     * START, which moves it to RECEIVING) before transferring ownership.  A
     * completely lost prefetch is cancelled and retried serially below. */
    return (client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_STARTED) ||
           (client->prefetch_state == SDR_CONTROL_CLIENT_RECEIVING) ||
           (client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);
}

static bool sdr_control_start_serial_fallback(sdr_control_client_t *client,
                                              uint32_t now_ms)
{
    uint32_t center_index;

    if ((client == NULL) || (client->fallback_pending == 0U) ||
        (client->fallback_cancel_pending != 0U) ||
        (client->fallback_cancel_confirmed == 0U) ||
        (client->fallback_wait_credit != 0U))
    {
        return false;
    }
    center_index = client->fallback_center_index;
    if (center_index >= RA8P1_SDR_CONTROL_CENTER_COUNT)
    {
        sdr_control_fail(client,
                         RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST,
                         now_ms);
        return false;
    }
    sdr_control_fallback_clear(client);
    sdr_control_make_capture_request(client, center_index);
    return sdr_control_send_capture_request(client, now_ms);
}

static void sdr_control_promote_prefetch(sdr_control_client_t *client,
                                         uint32_t now_ms)
{
    uint32_t promoted_state = client->prefetch_state;
    uint8_t promoted_control_retries = client->prefetch_control_retries;
    uint8_t promoted_agent_complete = client->prefetch_agent_complete;
    uint32_t promoted_agent_complete_ms =
        client->prefetch_agent_complete_ms;
    client->active_request = client->prefetched_request;
    client->current_center_index = client->active_request.center_index;
    client->request_start_ms = client->prefetch_request_start_ms;
    client->last_tx_ms = client->prefetch_last_tx_ms;
    client->agent_complete = promoted_agent_complete;
    client->agent_complete_ms = promoted_agent_complete_ms;
    client->iqsc_started =
        (promoted_state == SDR_CONTROL_CLIENT_RECEIVING) ? 1U : 0U;
    client->request_sent = client->prefetch_request_sent;
    client->active_control_retries = promoted_control_retries;
    client->active_from_prefetch =
        (promoted_state == SDR_CONTROL_CLIENT_RECEIVING) ? 0U : 1U;
    client->pending_retransmit = 0U;
    client->expect_prefetched_iq = 0U;
    client->active_result_ready = 0U;
    client->active_data_valid = 0U;
    client->iqsc_end_ms = 0U;
    client->iqsc_end_seen = 0U;
    client->missing_complete_counted = 0U;
    client->fast_complete_probe_sent = 0U;
    client->completion_probe_last_tx_ms = 0U;
    client->completion_probe_retries = 0U;
    client->stats.request_id = client->active_request.request_id;
    client->stats.session_id = client->active_request.session_id;
    client->stats.center_index = client->active_request.center_index;
    client->stats.request_start_ms = client->request_start_ms;
    client->stats.state =
        (promoted_state == SDR_CONTROL_CLIENT_RECEIVING) ?
        SDR_CONTROL_CLIENT_RECEIVING :
        ((promoted_agent_complete != 0U) ?
         SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT :
         SDR_CONTROL_CLIENT_WAIT_STARTED);
    client->stats.prefetched_windows++;
    sdr_control_prefetch_clear(client);

    if (promoted_state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED)
    {
        /* The speculative request may have been lost entirely.  Re-send the
         * exact attempt before allowing N+2 to consume the SDR credit. */
        (void)sdr_control_resend_active_request(client, now_ms);
    }
    else if (promoted_state == SDR_CONTROL_CLIENT_RECEIVING)
    {
        /* ACK released the old slot, so the SDR can prepare one more window
         * while the promoted window is being sent and analyzed. */
        (void)sdr_control_start_prefetch(client, now_ms);
    }
}

static void sdr_control_credit_accepted(sdr_control_client_t *client,
                                        uint32_t now_ms)
{
    uint32_t next_center;

    if (client->pending_retransmit != 0U)
    {
        client->pending_retransmit = 0U;
        if (!sdr_control_retry_request(client, now_ms))
        {
            client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW;
        }
        return;
    }

    client->stats.completed_windows++;
    if (sdr_control_next_prefetch_center(client, &next_center))
    {
        if (client->fallback_pending != 0U)
        {
            const bool recancel_after_credit =
                client->fallback_wait_credit != 0U;
            /* A speculative slot failed.  If this ACK just installed its
             * credit, the pre-credit CANCEL may have raced it; send a second
             * CANCEL and wait for the explicit release response before issuing
             * a fresh credit=1 request for the same center. */
            if (recancel_after_credit)
            {
                client->fallback_wait_credit = 0U;
                sdr_control_new_fallback_cancel_generation(client);
            }
            client->stats.state = SDR_CONTROL_CLIENT_WAIT_CANCELLED;
            if (recancel_after_credit)
            {
                (void)sdr_control_send_fallback_cancel(client, now_ms);
            }
            else if (client->fallback_cancel_confirmed != 0U)
            {
                (void)sdr_control_start_serial_fallback(client, now_ms);
            }
            else if (client->fallback_cancel_pending == 0U)
            {
                (void)sdr_control_send_fallback_cancel(client, now_ms);
            }
            return;
        }
        /* Promotion is bound to the credit value frozen into the ACK that the
         * SDR just accepted.  A late ACCEPTED/READY for a speculative request
         * must not upgrade an already-sent credit=0 ACK. */
        if ((client->pending_ack.credit != 0U) &&
            sdr_control_prefetch_promotable(client))
        {
            sdr_control_promote_prefetch(client, now_ms);
        }
        else
        {
            if (client->prefetch_valid != 0U)
            {
                /* A local send failure is still an uncertain delivery result.
                 * Cancel its identity before issuing a fresh credit=1 request. */
                sdr_control_abandon_prefetch(client, now_ms);
                client->stats.state = SDR_CONTROL_CLIENT_WAIT_CANCELLED;
            }
            else
            {
                /* No speculative identity exists, so serial fallback can start
                 * immediately without risking an orphan slot. */
                sdr_control_make_capture_request(client, next_center);
                (void)sdr_control_send_capture_request(client, now_ms);
            }
        }
    }
    else
    {
        if (client->repeat_scan != 0U)
        {
            const uint32_t next_center =
                (client->scan_all != 0U) ? 0U :
                client->current_center_index;
            /* The ACK was accepted and carried credit=0, so the SDR has
             * released the current slot before CPU0 creates the next request.
             * Four-center scans wrap to center 0; continuous single-center
             * mode repeats its selected center.  Both receive fresh identity. */
            sdr_control_make_capture_request(client, next_center);
            (void)sdr_control_send_capture_request(client, now_ms);
        }
        else
        {
            client->stats.state = SDR_CONTROL_CLIENT_COMPLETE;
            client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_OK;
        }
    }
}

void sdr_control_capture_options_default(sdr_control_capture_options_t *options)
{
    if (options == NULL)
    {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->sample_rate_hz = RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE;
    options->bandwidth_hz = RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH;
    options->sample_count = RA8P1_SDR_CONTROL_DEFAULT_SAMPLES;
    options->target_payload_mbps_x1000 =
        SDR_CONTROL_DEFAULT_TARGET_MBPS_X1000;
    options->send_batch = SDR_CONTROL_DEFAULT_SEND_BATCH;
    options->retry_limit = SDR_CONTROL_DEFAULT_RETRY_LIMIT;
    options->ack_timeout_ms = SDR_CONTROL_DEFAULT_ACK_TIMEOUT_MS;
    options->request_timeout_ms = SDR_CONTROL_DEFAULT_REQUEST_TIMEOUT_MS;
    options->flags = RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY |
                     RA8P1_SDR_CONTROL_FLAG_WINDOW_CRC32C |
                     RA8P1_SDR_CONTROL_FLAG_FASTLOCK |
                     RA8P1_SDR_CONTROL_FLAG_DOUBLE_BUFFER;
    options->test_fault_flags = 0U;
}

void sdr_control_client_init(sdr_control_client_t *client,
                             const sdr_control_transport_t *transport,
                             uint32_t request_seed,
                             uint32_t session_seed)
{
    uint64_t epoch = ((uint64_t)request_seed << 32U) | session_seed;
    if (epoch == 0ULL)
    {
        epoch = 1ULL;
    }
    sdr_control_client_init_with_epoch(client,
                                       transport,
                                       request_seed,
                                       session_seed,
                                       epoch);
}

void sdr_control_client_init_with_epoch(
    sdr_control_client_t *client,
    const sdr_control_transport_t *transport,
    uint32_t request_seed,
    uint32_t session_seed,
    uint64_t boot_epoch)
{
    if ((client == NULL) || (boot_epoch == 0ULL))
    {
        return;
    }
    memset(client, 0, sizeof(*client));
    if (transport != NULL)
    {
        client->transport = *transport;
    }
    client->next_request_id = request_seed;
    client->next_session_id = session_seed;
    client->boot_epoch = boot_epoch;
    client->stats.boot_epoch = boot_epoch;
    client->stats.state = SDR_CONTROL_CLIENT_IDLE;
    sdr_control_prefetch_clear(client);
    sdr_control_capture_options_default(&client->options);
}

bool sdr_control_client_start_single(sdr_control_client_t *client,
                                     uint32_t center_index,
                                     const sdr_control_capture_options_t *options,
                                     uint32_t now_ms)
{
    return sdr_control_start(client,
                             center_index,
                             false,
                             false,
                             options,
                             now_ms);
}

bool sdr_control_client_start_continuous_single(
    sdr_control_client_t *client,
    uint32_t center_index,
    const sdr_control_capture_options_t *options,
    uint32_t now_ms)
{
    return sdr_control_start(client,
                             center_index,
                             false,
                             true,
                             options,
                             now_ms);
}

bool sdr_control_client_start_scan(sdr_control_client_t *client,
                                   const sdr_control_capture_options_t *options,
                                   uint32_t now_ms)
{
    return sdr_control_start(client, 0U, true, false, options, now_ms);
}

bool sdr_control_client_start_continuous_scan(
    sdr_control_client_t *client,
    const sdr_control_capture_options_t *options,
    uint32_t now_ms)
{
    return sdr_control_start(client, 0U, true, true, options, now_ms);
}

bool sdr_control_client_cancel(sdr_control_client_t *client, uint32_t now_ms)
{
    if (client == NULL)
    {
        return false;
    }
    if (client->stats.state ==
        SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED)
    {
        return true;
    }
    if (!sdr_control_state_active(client->stats.state) &&
        (client->stats.state != SDR_CONTROL_CLIENT_ERROR))
    {
        return false;
    }

    sdr_control_terminal_cancel_clear(client);
    (void)sdr_control_terminal_cancel_append(client,
                                              &client->active_request);
    if (client->prefetch_valid != 0U)
    {
        (void)sdr_control_terminal_cancel_append(
            client, &client->prefetched_request);
    }
    if (client->fallback_pending != 0U)
    {
        (void)sdr_control_terminal_cancel_append(
            client, &client->fallback_cancel_request);
    }
    if (client->terminal_cancel_count == 0U)
    {
        return false;
    }

    client->repeat_scan = 0U;
    client->pending_retransmit = 0U;
    client->expect_prefetched_iq = 0U;
    client->stats.state = SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED;
    client->stats.last_status = RA8P1_SDR_CONTROL_STATUS_OK;
    /* A local UDP send failure is retryable and does not discard identity. */
    (void)sdr_control_terminal_cancel_send_current(client, now_ms);
    return true;
}

void sdr_control_client_poll(sdr_control_client_t *client, uint32_t now_ms)
{
    if (client == NULL)
    {
        return;
    }
    if (client->stats.state ==
        SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED)
    {
        if (sdr_control_elapsed(now_ms,
                                client->terminal_cancel_last_tx_ms,
                                SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS))
        {
            if (client->terminal_cancel_retries >=
                client->options.retry_limit)
            {
                client->stats.timeouts++;
                client->stats.state = SDR_CONTROL_CLIENT_ERROR;
                client->stats.last_status =
                    (client->terminal_cancel_tx_succeeded != 0U) ?
                    RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT :
                    RA8P1_SDR_CONTROL_STATUS_SEND_FAILED;
            }
            else
            {
                client->terminal_cancel_retries++;
                client->stats.retries++;
                (void)sdr_control_terminal_cancel_send_current(client,
                                                                now_ms);
            }
        }
        return;
    }
    if ((client->fallback_pending != 0U) &&
        (client->fallback_cancel_pending != 0U) &&
        sdr_control_elapsed(now_ms,
                            client->fallback_cancel_last_tx_ms,
                            client->options.ack_timeout_ms))
    {
        if (client->fallback_cancel_retries >= client->options.retry_limit)
        {
            client->stats.timeouts++;
            sdr_control_fail(client,
                             RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT,
                             now_ms);
        }
        else
        {
            client->fallback_cancel_retries++;
            client->stats.retries++;
            (void)sdr_control_send_fallback_cancel(client, now_ms);
        }
    }
    if ((client->prefetch_valid != 0U) &&
        (client->prefetch_abandoned == 0U) &&
        sdr_control_elapsed(now_ms,
                            client->prefetch_request_start_ms,
                            client->options.request_timeout_ms))
    {
        /* A speculative capture must never consume the active window's result
         * timeout. Cancel it at one absolute deadline and use serial fallback. */
        client->stats.timeouts++;
        sdr_control_abandon_prefetch(client, now_ms);
    }
    else if ((client->prefetch_valid != 0U) &&
             (client->prefetch_abandoned == 0U) &&
              ((client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
               (client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_STARTED)) &&
             sdr_control_elapsed(now_ms,
                                 client->prefetch_last_tx_ms,
                                 client->options.ack_timeout_ms))
    {
        if (!sdr_control_retry_prefetch(client, now_ms))
        {
            /* Marking the slot failed allows the current window to finish and
             * then use the serial fallback instead of deadlocking a scan. */
            sdr_control_abandon_prefetch(client, now_ms);
        }
    }
    if ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED))
    {
        if (sdr_control_elapsed(now_ms,
                                client->last_tx_ms,
                                client->options.ack_timeout_ms))
        {
            client->stats.timeouts++;
            (void)sdr_control_resend_active_request(client, now_ms);
        }
    }
    else if (((client->stats.state == SDR_CONTROL_CLIENT_RECEIVING) ||
              (client->stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT)) &&
             (client->agent_complete == 0U) &&
             (client->iqsc_end_seen != 0U))
    {
        if (sdr_control_elapsed(now_ms,
                                client->request_start_ms,
                                client->options.request_timeout_ms))
        {
            /* Full IQ is local, but without CAPTURE_COMPLETE CPU0 cannot
             * legally grant credit.  Do not leave the SDR slot stuck forever. */
            client->stats.timeouts++;
            sdr_control_fail(client,
                             RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT,
                             now_ms);
        }
        else if ((client->fast_complete_probe_sent == 0U) &&
                 sdr_control_elapsed(now_ms,
                                     client->iqsc_end_ms,
                                     SDR_CONTROL_COMPLETION_FAST_PROBE_MS))
        {
            /* This runs in CPU0's control worker, not the RMAC receive
             * callback. It can recover CAPTURE_COMPLETE while STFT drains. */
            (void)sdr_control_completion_probe_active_request(client, now_ms);
        }
        else if ((client->fast_complete_probe_sent != 0U) &&
                 sdr_control_elapsed(now_ms,
                                     client->completion_probe_last_tx_ms,
                                     sdr_control_completion_probe_interval_ms(
                                         client->completion_probe_retries)))
        {
            /* The first response can be lost independently of IQSC. Probe at
             * 4 -> 20 -> 40 -> 80 ms; 80 ms is then the stable upper bound
             * until CAPTURE_COMPLETE arrives or the request timeout fires. */
            if (client->completion_probe_retries != UINT8_MAX)
            {
                client->completion_probe_retries++;
            }
            client->stats.retries++;
            if (client->missing_complete_counted == 0U)
            {
                client->stats.missing_capture_complete++;
                client->missing_complete_counted = 1U;
            }
            (void)sdr_control_completion_probe_active_request(client, now_ms);
        }
    }
    else if (((client->stats.state == SDR_CONTROL_CLIENT_RECEIVING) ||
              (client->stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT)) &&
             (client->agent_complete != 0U) &&
             (client->iqsc_started == 0U) &&
             (client->active_result_ready == 0U) &&
             sdr_control_elapsed(now_ms,
                                 client->agent_complete_ms,
                                 SDR_CONTROL_IQSC_START_RETRY_MS))
    {
        /* A valid CAPTURE_COMPLETE makes RETRY_WINDOW legal: the agent has
         * finished sending and retained this exact request/session.  Missing
         * START means the local data plane cannot ever complete this attempt,
         * so recover the cached window on a short bounded timer. */
        client->stats.timeouts++;
        if (!sdr_control_request_cached_retry(client, now_ms))
        {
            sdr_control_fail(client,
                             RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT,
                             now_ms);
        }
    }
    else if (((client->stats.state == SDR_CONTROL_CLIENT_RECEIVING) ||
              (client->stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT)) &&
             (client->active_result_ready == 0U))
    {
        if (sdr_control_elapsed(now_ms,
                                client->request_start_ms,
                                client->options.request_timeout_ms))
        {
            if (client->agent_complete == 0U)
            {
                /* A missing CAPTURE_COMPLETE means the SDR is not yet proven
                 * to be in WAIT_ACK.  Probe the exact request idempotently;
                 * WINDOW_ACK/RETRY_WINDOW would be illegal in that state. */
                if (sdr_control_elapsed(now_ms,
                                        client->last_tx_ms,
                                        client->options.ack_timeout_ms))
                {
                    client->stats.timeouts++;
                    (void)sdr_control_resend_active_request(client, now_ms);
                }
            }
            else
            {
                client->stats.timeouts++;
                if ((client->active_data_valid == 0U) &&
                    (client->active_request.attempt <
                     client->options.retry_limit))
                {
                    (void)sdr_control_request_cached_retry(client, now_ms);
                }
                else
                {
                    sdr_control_fail(client,
                                     RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT,
                                     now_ms);
                }
            }
        }
    }
    else if ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED) &&
             (client->fallback_pending != 0U) &&
             (client->fallback_cancel_confirmed != 0U))
    {
        (void)sdr_control_start_serial_fallback(client, now_ms);
    }
    else if ((client->stats.state ==
              SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED) &&
             sdr_control_elapsed(now_ms,
                                 client->last_tx_ms,
                                 client->options.ack_timeout_ms))
    {
        if (client->pending_ack.attempt >= client->options.retry_limit)
        {
            client->stats.timeouts++;
            sdr_control_fail(client,
                             RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT,
                             now_ms);
        }
        else
        {
            client->pending_ack.attempt++;
            client->stats.retries++;
            client->last_tx_ms = now_ms;
            (void)sdr_control_send_message(client,
                                           &client->pending_ack,
                                           now_ms);
        }
    }
}

static bool sdr_control_response_matches(
    const ra8p1_sdr_control_message_t *response,
    const ra8p1_sdr_control_message_t *request)
{
    return (response != NULL) && (request != NULL) &&
           (response->boot_epoch == request->boot_epoch) &&
           (response->request_id == request->request_id) &&
           (response->session_id == request->session_id) &&
           (response->center_index == request->center_index) &&
           (response->center_frequency_hz == request->center_frequency_hz);
}

static bool sdr_control_active_response_attempt_valid(
    const sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *response)
{
    if ((client == NULL) || (response == NULL))
    {
        return false;
    }
    if (response->command == RA8P1_SDR_CONTROL_CREDIT_ACCEPTED)
    {
        /* The agent caches the first accepted ACK response.  An idempotent ACK
         * retry can therefore receive an older (but never newer) attempt. */
        return response->attempt <= client->pending_ack.attempt;
    }
    if ((response->command == RA8P1_SDR_CONTROL_ERROR) &&
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED))
    {
        return (response->attempt == client->active_request.attempt) ||
               (response->attempt <= client->pending_ack.attempt);
    }
    return response->attempt == client->active_request.attempt;
}

static bool sdr_control_begin_active_fallback(sdr_control_client_t *client,
                                              uint32_t now_ms)
{
    if ((client == NULL) || (client->active_from_prefetch == 0U) ||
        (client->iqsc_started != 0U) ||
        (client->fallback_pending != 0U))
    {
        return false;
    }
    client->fallback_cancel_request = client->active_request;
    client->fallback_center_index = client->active_request.center_index;
    client->prefetch_abandoned = 1U;
    client->fallback_pending = 1U;
    client->fallback_wait_credit = 0U;
    client->fallback_cancel_confirmed = 0U;
    client->fallback_cancel_retries = 0U;
    client->fallback_cancel_generation = 0U;
    client->expect_prefetched_iq = 0U;
    client->active_from_prefetch = 0U;
    client->stats.state = SDR_CONTROL_CLIENT_WAIT_CANCELLED;
    sdr_control_new_fallback_cancel_generation(client);
    (void)sdr_control_send_fallback_cancel(client, now_ms);
    return true;
}

static bool sdr_control_receive_fallback(
    sdr_control_client_t *client,
    const ra8p1_sdr_control_message_t *response,
    uint32_t now_ms)
{
    if ((client == NULL) || (response == NULL) ||
        (client->fallback_pending == 0U) ||
        !sdr_control_response_matches(response,
                                      &client->fallback_cancel_request))
    {
        return false;
    }
    if ((response->command == RA8P1_SDR_CONTROL_ERROR) &&
        (response->status == RA8P1_SDR_CONTROL_STATUS_CANCELLED))
    {
        client->last_rx_ms = now_ms;
        client->stats.last_rx_ms = now_ms;
        client->stats.rx_datagrams++;
        client->stats.last_status = response->status;
        if (response->attempt != client->fallback_cancel_generation)
        {
            /* Valid but stale confirmation for an earlier cancellation
             * generation.  Keep waiting for the post-credit response. */
            return true;
        }
        client->fallback_cancel_pending = 0U;
        client->fallback_cancel_confirmed = 1U;
        /* A pre-credit confirmation is deliberately retained as a tombstone;
         * credit acceptance below forces one more CANCEL to close the race. */
        if ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED) &&
            (client->fallback_wait_credit == 0U))
        {
            (void)sdr_control_start_serial_fallback(client, now_ms);
        }
        return true;
    }
    /* A capture ERROR may arrive after a best-effort CANCEL.  It confirms the
     * speculative failure but does not release a possibly outstanding token. */
    return (response->command == RA8P1_SDR_CONTROL_ERROR);
}

static bool sdr_control_receive_prefetch(sdr_control_client_t *client,
                                         const ra8p1_sdr_control_message_t *response,
                                         uint32_t now_ms)
{
    client->prefetch_last_tx_ms = now_ms;
    switch (response->command)
    {
        case RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED:
            if (response->status != RA8P1_SDR_CONTROL_STATUS_OK)
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
                break;
            }
            if (client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED)
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_WAIT_STARTED;
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_STARTED:
            if (response->status != RA8P1_SDR_CONTROL_STATUS_OK)
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
                break;
            }
            /* credit=0 means this is local capture, not an IQSC send.  Do not
             * let a delayed STARTED response regress a later READY state. */
            if ((client->prefetch_state ==
                 SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
                (client->prefetch_state == SDR_CONTROL_CLIENT_WAIT_STARTED))
            {
                client->prefetch_state =
                    SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_READY:
            if (response->status != RA8P1_SDR_CONTROL_STATUS_OK)
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
                break;
            }
            client->prefetch_ready = 1U;
            if (client->prefetch_state != SDR_CONTROL_CLIENT_RECEIVING)
            {
                client->prefetch_state =
                    SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_COMPLETE:
            /* A prefetched slot may complete immediately after an earlier ACK
             * grants send credit. Keep its state for promotion. */
            if (response->status == RA8P1_SDR_CONTROL_STATUS_OK)
            {
                client->prefetch_agent_complete = 1U;
                client->prefetch_agent_complete_ms = now_ms;
                if (client->prefetch_state != SDR_CONTROL_CLIENT_RECEIVING)
                {
                    client->prefetch_state =
                        SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
                }
            }
            else
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
            }
            break;
        case RA8P1_SDR_CONTROL_ERROR:
            if (response->status == RA8P1_SDR_CONTROL_STATUS_CANCELLED)
            {
                /* The fallback tombstone handler normally consumes this
                 * response after the slot has been detached. */
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
            }
            else
            {
                client->prefetch_state = SDR_CONTROL_CLIENT_ERROR;
            }
            break;
        default:
            client->stats.invalid_datagrams++;
            return false;
    }
    client->stats.prefetch_state = client->prefetch_state;
    if ((client->prefetch_state == SDR_CONTROL_CLIENT_ERROR) &&
        (client->fallback_pending == 0U))
    {
        client->prefetch_ready = 0U;
        /* A rejected speculative slot is not promotable.  Keep a tombstone
         * until the active ACK/credit ordering is known. */
        sdr_control_abandon_prefetch(client, now_ms);
    }
    return true;
}

bool sdr_control_client_receive(sdr_control_client_t *client,
                                const uint8_t *wire,
                                size_t length,
                                uint32_t now_ms)
{
    ra8p1_sdr_control_message_t response;
    if ((client == NULL) ||
        !ra8p1_sdr_control_decode(wire, length, &response))
    {
        if (client != NULL)
        {
            client->stats.invalid_datagrams++;
        }
        return false;
    }
    if (!sdr_control_state_active(client->stats.state) ||
        (response.boot_epoch != client->boot_epoch))
    {
        client->stats.invalid_datagrams++;
        return false;
    }
    if (client->stats.state ==
        SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED)
    {
        return sdr_control_receive_terminal_cancel(client,
                                                   &response,
                                                   now_ms);
    }
    if ((client->credit_proof_pending != 0U) &&
        (response.command == RA8P1_SDR_CONTROL_CREDIT_ACCEPTED) &&
        sdr_control_response_matches(&response, &client->credit_proof_request))
    {
        /* IQSC START already proved the token and promoted the prefetched
         * request.  Consume the delayed control response as a duplicate. */
        client->credit_proof_pending = 0U;
        client->last_response = response;
        client->last_rx_ms = now_ms;
        client->stats.last_rx_ms = now_ms;
        client->stats.rx_datagrams++;
        client->stats.last_message_crc32c = response.message_crc32c;
        return true;
    }
    if ((client->fallback_pending != 0U) &&
        sdr_control_response_matches(&response,
                                     &client->fallback_cancel_request))
    {
        client->last_response = response;
        client->stats.last_message_crc32c = response.message_crc32c;
        return sdr_control_receive_fallback(client, &response, now_ms);
    }
    if ((client->prefetch_valid != 0U) &&
        sdr_control_response_matches(&response,
                                     &client->prefetched_request))
    {
        client->last_response = response;
        client->last_rx_ms = now_ms;
        client->stats.last_rx_ms = now_ms;
        client->stats.rx_datagrams++;
        client->stats.last_message_crc32c = response.message_crc32c;
        return sdr_control_receive_prefetch(client, &response, now_ms);
    }
    if (!sdr_control_response_matches(&response, &client->active_request))
    {
        client->stats.invalid_datagrams++;
        return false;
    }
    if (!sdr_control_active_response_attempt_valid(client, &response))
    {
        /* A retransmitted window keeps request/session identity.  Reject a
         * delayed response from its previous attempt before it can advance the
         * new capture or poison its terminal state. */
        client->stats.invalid_datagrams++;
        return false;
    }
    client->last_response = response;
    client->last_rx_ms = now_ms;
    client->stats.last_rx_ms = now_ms;
    client->stats.rx_datagrams++;
    client->stats.last_status = response.status;
    client->stats.actual_payload_mbps_x1000 =
        response.actual_payload_mbps_x1000;
    client->stats.window_crc32c = response.window_crc32c;
    client->stats.last_message_crc32c = response.message_crc32c;
    client->stats.agent_request_rx_us = response.agent_request_rx_us;
    client->stats.tune_start_us = response.tune_start_us;
    client->stats.tune_complete_us = response.tune_complete_us;
    client->stats.capture_start_us = response.capture_start_us;
    client->stats.capture_complete_us = response.capture_complete_us;

    switch (response.command)
    {
        case RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED:
            if ((response.status == RA8P1_SDR_CONTROL_STATUS_OK) &&
                ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
                 (client->stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED)))
            {
                client->stats.state = SDR_CONTROL_CLIENT_WAIT_STARTED;
                client->last_tx_ms = now_ms;
                (void)sdr_control_start_prefetch(client, now_ms);
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_STARTED:
            if ((response.status == RA8P1_SDR_CONTROL_STATUS_OK) &&
                ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
                 (client->stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED) ||
                 (client->stats.state == SDR_CONTROL_CLIENT_RECEIVING)))
            {
                client->stats.state = SDR_CONTROL_CLIENT_RECEIVING;
                (void)sdr_control_start_prefetch(client, now_ms);
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_COMPLETE:
            if ((response.status == RA8P1_SDR_CONTROL_STATUS_OK) &&
                ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED) ||
                 (client->stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED) ||
                 (client->stats.state == SDR_CONTROL_CLIENT_RECEIVING) ||
                 (client->stats.state ==
                  SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT)))
            {
                client->agent_complete = 1U;
                client->agent_complete_ms = now_ms;
                client->stats.state = SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_READY:
            /* The active request carries send credit and must progress through
             * IQSC START/END, never through the prefetch-only READY state. */
            client->stats.invalid_datagrams++;
            return false;
        case RA8P1_SDR_CONTROL_CREDIT_ACCEPTED:
            if ((response.status == RA8P1_SDR_CONTROL_STATUS_OK) &&
                (client->stats.state ==
                 SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED))
            {
                sdr_control_credit_accepted(client, now_ms);
            }
            break;
        case RA8P1_SDR_CONTROL_ERROR:
            if ((response.status ==
                 RA8P1_SDR_CONTROL_STATUS_WINDOW_NOT_READY) &&
                (client->stats.state ==
                 SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED))
            {
                /* IQSC END may beat the agent's WAIT_ACK transition.  Keep the
                 * exact pending ACK and retry it on the normal bounded timer. */
                client->last_tx_ms = now_ms;
            }
            else if (!sdr_control_begin_active_fallback(client, now_ms))
            {
                sdr_control_fail(client, response.status, now_ms);
            }
            break;
        default:
            client->stats.invalid_datagrams++;
            return false;
    }
    return true;
}

bool sdr_control_client_session_matches(const sdr_control_client_t *client,
                                        uint32_t session_id,
                                        uint32_t center_index)
{
    const ra8p1_sdr_control_message_t *expected =
        sdr_control_client_expected_request(client);
    return (client != NULL) && sdr_control_state_active(client->stats.state) &&
           (expected != NULL) &&
           (session_id != 0U) &&
           (session_id == expected->session_id) &&
           (center_index == expected->center_index);
}

void sdr_control_client_notify_iqsc_start(sdr_control_client_t *client,
                                          uint32_t session_id,
                                          uint32_t center_index,
                                          uint32_t now_ms)
{
    if (!sdr_control_client_session_matches(client,
                                            session_id,
                                            center_index))
    {
        if (client != NULL)
        {
            client->stats.invalid_datagrams++;
        }
        return;
    }
    if ((client->expect_prefetched_iq != 0U) &&
        (client->prefetch_valid != 0U) &&
        (session_id == client->prefetched_request.session_id))
    {
        client->prefetch_state = SDR_CONTROL_CLIENT_RECEIVING;
        client->stats.prefetch_state = client->prefetch_state;
        if ((client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED) &&
            (client->pending_ack.credit != 0U))
        {
            /* IQSC START can arrive before the sparse control response.  It
             * proves that the SDR applied this ACK's credit, so promote now
             * and retain a tombstone for the delayed CREDIT_ACCEPTED packet. */
            client->credit_proof_request = client->pending_ack;
            client->credit_proof_pending = 1U;
            client->stats.prefetch_iqsc_credit_proofs++;
            client->expect_prefetched_iq = 0U;
            sdr_control_credit_accepted(client, now_ms);
        }
    }
    else
    {
        client->iqsc_started = 1U;
        client->active_from_prefetch = 0U;
        client->stats.state = SDR_CONTROL_CLIENT_RECEIVING;
        (void)sdr_control_start_prefetch(client, now_ms);
    }
    client->last_rx_ms = now_ms;
    client->stats.last_rx_ms = now_ms;
}

void sdr_control_client_notify_iqsc_end(sdr_control_client_t *client,
                                        uint32_t session_id,
                                        uint32_t center_index,
                                        uint32_t now_ms)
{
    if ((client == NULL) ||
        !sdr_control_state_active(client->stats.state) ||
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED) ||
        (session_id == 0U) ||
        (session_id != client->active_request.session_id) ||
        (center_index != client->active_request.center_index))
    {
        return;
    }
    if (client->iqsc_end_seen == 0U)
    {
        client->iqsc_end_ms = now_ms;
        client->iqsc_end_seen = 1U;
    }
}

void sdr_control_client_observe_window(
    sdr_control_client_t *client,
    const sdr_control_window_evidence_t *evidence,
    uint32_t now_ms)
{
    bool data_error;
    bool grant_prefetch;
    bool prefetch_ready_at_ack;
    uint32_t status;
    if ((client == NULL) || (evidence == NULL) ||
        !sdr_control_state_active(client->stats.state) ||
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED) ||
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED) ||
        (evidence->session_id != client->active_request.session_id) ||
        !evidence->iqsc_complete ||
        (evidence->ring_free != RA8P1_SDR_CONTROL_RING_SLOTS))
    {
        return;
    }

    data_error = (evidence->sequence_gaps != 0U) ||
                 (evidence->reordered != 0U) ||
                 (evidence->invalid_packets != 0U) ||
                 (evidence->ring_drops != 0U) ||
                 (evidence->ring_full_drops != 0U) ||
                 (evidence->ring_oversize_drops != 0U) ||
                 (evidence->crc_errors != 0U) ||
                 !evidence->payload_complete ||
                 !evidence->crc_present || !evidence->crc_valid;
    client->active_data_valid = data_error ? 0U : 1U;
    if (client->iqsc_end_seen == 0U)
    {
        /* Host replays and legacy callers may not publish an explicit END
         * timestamp.  They still get bounded behavior, with the grace starting
         * at the first complete local observation. */
        client->iqsc_end_ms = now_ms;
        client->iqsc_end_seen = 1U;
    }
    /* IQSC END proves only that the data plane ended. WINDOW_ACK is legal
     * only after the passive agent publishes CAPTURE_COMPLETE and enters
     * WAIT_ACK. The control worker owns all completion-state probing so it
     * can overlap analysis without sending from the RMAC context. */
    if (client->agent_complete == 0U)
    {
        client->stats.state = SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
        return;
    }
    /* The derived frame is durable once CPU0 publishes it into the shared
     * four-slot ring. CPU1 ownership/visibility acknowledgement remains
     * asynchronous telemetry and must not hold the SDR's next-window credit. */
    if (!data_error && !evidence->analysis_complete)
    {
        client->stats.state = SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT;
        return;
    }
    if (!data_error)
    {
        client->active_result_ready = 1U;
    }
    grant_prefetch = !data_error && sdr_control_has_next_center(client) &&
                     sdr_control_prefetch_promotable(client);
    prefetch_ready_at_ack = client->prefetch_ready != 0U;

    if (!evidence->crc_present || !evidence->crc_valid)
    {
        status = RA8P1_SDR_CONTROL_STATUS_IQ_CRC;
    }
    else if ((evidence->sequence_gaps != 0U) ||
             (evidence->reordered != 0U))
    {
        status = RA8P1_SDR_CONTROL_STATUS_IQ_GAP;
    }
    else if (!evidence->payload_complete ||
             (evidence->invalid_packets != 0U) ||
             (evidence->ring_drops != 0U) ||
             (evidence->ring_full_drops != 0U) ||
             (evidence->ring_oversize_drops != 0U))
    {
        status = RA8P1_SDR_CONTROL_STATUS_IQ_DROP;
    }
    else
    {
        status = RA8P1_SDR_CONTROL_STATUS_OK;
    }

    client->pending_ack = client->active_request;
    client->pending_ack.command = RA8P1_SDR_CONTROL_WINDOW_ACK;
    client->pending_ack.flags &=
        (uint16_t)~RA8P1_SDR_CONTROL_FLAG_RETRANSMIT;
    client->pending_ack.credit = grant_prefetch ? 1U : 0U;
    client->pending_ack.ring_free = evidence->ring_free;
    client->pending_ack.status = data_error ?
        RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW : status;
    client->pending_ack.attempt = 0U;
    client->pending_ack.window_crc32c = evidence->crc32c;
    client->pending_ack.actual_payload_mbps_x1000 =
        evidence->actual_payload_mbps_x1000;
    client->pending_ack.sequence_gaps = evidence->sequence_gaps;
    client->pending_ack.reordered = evidence->reordered;
    client->pending_ack.invalid_packets = evidence->invalid_packets;
    client->pending_ack.ring_full_drops = evidence->ring_full_drops;
    client->pending_ack.ring_oversize_drops = evidence->ring_oversize_drops;
    client->pending_ack.crc_errors = evidence->crc_errors;
    client->pending_retransmit = data_error ? 1U : 0U;
    client->stats.last_status = status;
    client->stats.window_crc32c = evidence->crc32c;
    client->stats.actual_payload_mbps_x1000 =
        evidence->actual_payload_mbps_x1000;

    if (data_error &&
        (client->active_request.attempt >= client->options.retry_limit))
    {
        sdr_control_fail(client, status, now_ms);
        return;
    }
    client->expect_prefetched_iq =
        (!data_error && (client->pending_ack.credit != 0U) &&
         (client->prefetch_valid != 0U)) ? 1U : 0U;
    client->stats.state = SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED;
    client->last_tx_ms = now_ms;
    if (sdr_control_send_message(client, &client->pending_ack, now_ms))
    {
        if (grant_prefetch && !prefetch_ready_at_ack)
        {
            client->stats.prefetch_credit_without_ready++;
        }
    }
    else
    {
        if ((client->credit_proof_pending != 0U) &&
            sdr_control_response_matches(&client->credit_proof_request,
                                         &client->pending_ack))
        {
            /* A transport can report failure after the datagram reached the
             * peer.  Reentrant IQSC START is stronger delivery evidence: it
             * proves this exact ACK's credit was applied and promotion already
             * occurred.  Never roll that valid transfer back into ERROR. */
            if (grant_prefetch && !prefetch_ready_at_ack)
            {
                client->stats.prefetch_credit_without_ready++;
            }
            return;
        }
        client->expect_prefetched_iq = 0U;
        sdr_control_fail(client,
                         RA8P1_SDR_CONTROL_STATUS_SEND_FAILED,
                         now_ms);
    }
}

const ra8p1_sdr_control_message_t *sdr_control_client_expected_request(
    const sdr_control_client_t *client)
{
    if ((client == NULL) || !sdr_control_state_active(client->stats.state) ||
        (client->stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED) ||
        (client->stats.state ==
         SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED))
    {
        return NULL;
    }
    if ((client->expect_prefetched_iq != 0U) &&
        (client->prefetch_valid != 0U))
    {
        return &client->prefetched_request;
    }
    return &client->active_request;
}

uint32_t sdr_control_client_expected_session(
    const sdr_control_client_t *client)
{
    const ra8p1_sdr_control_message_t *request =
        sdr_control_client_expected_request(client);
    return (request != NULL) ? request->session_id : 0U;
}

void sdr_control_client_stats_get(const sdr_control_client_t *client,
                                  uint32_t now_ms,
                                  sdr_control_client_stats_t *stats)
{
    if ((client == NULL) || (stats == NULL))
    {
        return;
    }
    *stats = client->stats;
    if (stats->request_id != 0U)
    {
        stats->request_elapsed_ms = now_ms - client->request_start_ms;
    }
}
