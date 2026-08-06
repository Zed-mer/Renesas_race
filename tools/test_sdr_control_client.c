#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../cpu0/src/framework/sdr_control_client.h"

typedef struct st_mock_transport
{
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    uint32_t sends;
    uint32_t calls;
    uint32_t fail_on_call;
    uint32_t failures_remaining;
    sdr_control_client_t *observe_client;
    uint32_t ack_expected_session_during_send;
    uint32_t ack_state_during_send;
    uint32_t ack_observations;
    uint32_t ack_notify_now_ms;
    uint8_t notify_iqsc_start_on_ack;
    uint8_t fail_after_ack_proof;
} mock_transport_t;

static bool mock_send(void *context, const uint8_t *data, size_t length)
{
    mock_transport_t *mock = (mock_transport_t *)context;
    ra8p1_sdr_control_message_t message;
    if ((mock == NULL) || (data == NULL) ||
        (length != sizeof(mock->wire)))
    {
        return false;
    }
    mock->calls++;
    if ((mock->fail_on_call != 0U) &&
        (mock->calls == mock->fail_on_call))
    {
        return false;
    }
    if (mock->failures_remaining != 0U)
    {
        mock->failures_remaining--;
        return false;
    }
    if ((mock->observe_client != NULL) &&
        ra8p1_sdr_control_decode(data, length, &message) &&
        (message.command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
        (message.credit != 0U))
    {
        const ra8p1_sdr_control_message_t *expected =
            sdr_control_client_expected_request(mock->observe_client);
        mock->ack_expected_session_during_send =
            (expected != NULL) ? expected->session_id : 0U;
        mock->ack_state_during_send = mock->observe_client->stats.state;
        mock->ack_observations++;
        if ((mock->notify_iqsc_start_on_ack != 0U) && (expected != NULL))
        {
            sdr_control_client_notify_iqsc_start(
                mock->observe_client, expected->session_id,
                expected->center_index, mock->ack_notify_now_ms);
        }
        if (mock->fail_after_ack_proof != 0U)
        {
            return false;
        }
    }
    memcpy(mock->wire, data, length);
    mock->sends++;
    return true;
}

static void send_response(sdr_control_client_t *client,
                          uint16_t command,
                          uint32_t status,
                          uint32_t now_ms)
{
    ra8p1_sdr_control_message_t response = client->active_request;
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    response.command = command;
    response.status = status;
    assert(ra8p1_sdr_control_encode(&response, wire));
    assert(sdr_control_client_receive(client, wire, sizeof(wire), now_ms));
}

static void send_response_for(sdr_control_client_t *client,
                              const ra8p1_sdr_control_message_t *request,
                              uint16_t command,
                              uint32_t status,
                              uint32_t now_ms)
{
    ra8p1_sdr_control_message_t response = *request;
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    response.command = command;
    response.status = status;
    assert(ra8p1_sdr_control_encode(&response, wire));
    assert(sdr_control_client_receive(client, wire, sizeof(wire), now_ms));
}

static void make_clean_evidence(sdr_control_window_evidence_t *evidence,
                                uint32_t session_id)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->session_id = session_id;
    evidence->ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    evidence->iqsc_complete = true;
    evidence->payload_complete = true;
    evidence->crc_present = true;
    evidence->crc_valid = true;
    evidence->analysis_complete = true;
    evidence->cpu1_visible = true;
}

static void test_single_window_gate(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence = {0};
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 100U, 200U);
    assert(sdr_control_client_start_single(&client, 2U, &options, 10U));
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(mock.sends == 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == 101U);
    assert(outbound.session_id == 201U);
    assert(outbound.center_frequency_hz == 5760000000ULL);

    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid == 0U);
    assert(mock.sends == 1U); /* A one-shot single never speculates. */
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, 201U, 2U, 13U);

    evidence.session_id = 201U;
    evidence.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    evidence.iqsc_complete = true;
    evidence.payload_complete = true;
    evidence.crc_present = true;
    evidence.crc_valid = true;
    evidence.analysis_complete = true;
    evidence.cpu1_visible = true;
    sdr_control_client_observe_window(&client, &evidence, 14U);
    assert(mock.sends == 1U); /* CAPTURE_COMPLETE is still missing. */

    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                  RA8P1_SDR_CONTROL_STATUS_OK, 15U);
    evidence.cpu1_visible = false;
    sdr_control_client_observe_window(&client, &evidence, 16U);
    assert(mock.sends == 1U); /* CPU1 has not acknowledged visibility. */

    evidence.cpu1_visible = true;
    sdr_control_client_observe_window(&client, &evidence, 17U);
    assert(mock.sends == 2U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.credit == 0U); /* A final single window grants no next work. */
    assert(outbound.ring_free == RA8P1_SDR_CONTROL_RING_SLOTS);

    send_response(&client, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 18U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_COMPLETE);
    assert(client.stats.completed_windows == 1U);
}

static void test_request_retry_is_idempotent(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t retry;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 900U, 1900U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 100U));
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &first));
    sdr_control_client_poll(&client, 100U + options.ack_timeout_ms);
    assert(mock.sends == 2U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &retry));
    assert(retry.request_id == first.request_id);
    assert(retry.session_id == first.session_id);
    /* A missing control datagram is retried idempotently.  attempt/RETRANSMIT
     * describe cached-window recovery only, not UDP control delivery. */
    assert(retry.attempt == first.attempt);
    assert((retry.flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) == 0U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 100U +
                  options.ack_timeout_ms + 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED);
}

static void test_missing_complete_uses_bounded_completion_probes(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 1200U, 2200U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 10U));
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client,
                                         client.active_request.session_id,
                                         client.active_request.center_index,
                                         13U);
    sdr_control_client_notify_iqsc_end(&client,
                                       client.active_request.session_id,
                                       client.active_request.center_index,
                                       13U);
    make_clean_evidence(&evidence, client.active_request.session_id);
    sdr_control_client_observe_window(&client, &evidence, 14U);
    assert(mock.sends == 1U); /* The short completion probe is still pending. */
    sdr_control_client_poll(
        &client, 13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS);
    assert(mock.sends == 2U);
    assert(client.stats.missing_capture_complete == 0U);
    assert(client.active_control_retries == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == client.active_request.request_id);
    assert(outbound.attempt == client.active_request.attempt);

    sdr_control_client_poll(
        &client, 13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS +
        SDR_CONTROL_COMPLETION_PROBE_1_MS - 1U);
    assert(mock.sends == 2U); /* Backoff prevents a control-service flood. */

    /* A second idempotent probe is due after 20 ms, not one full ACK timeout.
     * It must leave the active request/attempt untouched. */
    sdr_control_client_poll(
        &client, 13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS +
        SDR_CONTROL_COMPLETION_PROBE_1_MS);
    assert(mock.sends == 3U);
    assert(client.stats.missing_capture_complete == 1U);
    assert(client.active_control_retries == 0U);
    assert(client.completion_probe_retries == 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == client.active_request.request_id);
    assert(outbound.attempt == client.active_request.attempt);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);

    /* A control response loss is recovered by the idempotent probe.  No ACK is
     * sent while the SDR worker is still only known to have emitted IQSC END. */
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                  RA8P1_SDR_CONTROL_STATUS_OK,
                  13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS +
                  SDR_CONTROL_COMPLETION_PROBE_1_MS + 1U);
    sdr_control_client_observe_window(
        &client, &evidence,
        13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS +
        SDR_CONTROL_COMPLETION_PROBE_1_MS + 2U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.credit == 0U);
    send_response(&client, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK,
                  13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS +
                  SDR_CONTROL_COMPLETION_PROBE_1_MS + 2U +
                  options.ack_timeout_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_COMPLETE);
}

static void test_iqsc_end_fast_probe_runs_before_analysis_complete(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 0U;
    sdr_control_client_init(&client, &transport, 3200U, 4200U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 10U));
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client,
                                         client.active_request.session_id,
                                         client.active_request.center_index,
                                         13U);
    sdr_control_client_notify_iqsc_end(&client,
                                       client.active_request.session_id,
                                       client.active_request.center_index,
                                       13U);

    /* No completed local evidence is supplied here: the probe must be able
     * to overlap the still-running ring/STFT worker. */
    sdr_control_client_poll(
        &client, 13U + SDR_CONTROL_COMPLETION_FAST_PROBE_MS);
    assert(mock.sends == 2U);
    assert(client.fast_complete_probe_sent != 0U);
    assert(client.active_control_retries == 0U);
    assert(client.stats.missing_capture_complete == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == client.active_request.request_id);
    assert(outbound.session_id == client.active_request.session_id);
    assert(outbound.attempt == client.active_request.attempt);
}

static void test_iqsc_end_delayed_poll_sends_one_completion_probe(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 1U;
    sdr_control_client_init(&client, &transport, 5200U, 6200U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 10U));
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client,
                                         client.active_request.session_id,
                                         client.active_request.center_index,
                                         13U);
    sdr_control_client_notify_iqsc_end(&client,
                                       client.active_request.session_id,
                                       client.active_request.center_index,
                                       13U);

    /* A delayed worker must not emit several probes in one service slice. */
    sdr_control_client_poll(
        &client, 13U + SDR_CONTROL_COMPLETION_PROBE_2_MS);
    assert(mock.sends == 2U);
    assert(client.fast_complete_probe_sent != 0U);
    assert(client.completion_probe_retries == 0U);
    assert(client.stats.missing_capture_complete == 0U);
    assert(client.active_control_retries == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == client.active_request.request_id);
    assert(outbound.session_id == client.active_request.session_id);
    assert(outbound.attempt == client.active_request.attempt);
}

static void test_transient_send_failure_keeps_request(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;

    mock.failures_remaining = 1U;
    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 300U, 400U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 100U));
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.stats.request_id == 301U);
    assert(client.stats.session_id == 401U);
    assert(client.stats.last_status == RA8P1_SDR_CONTROL_STATUS_SEND_FAILED);

    sdr_control_client_poll(&client, 100U + options.ack_timeout_ms);
    assert(mock.sends == 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.request_sent == 1U);
    assert(client.active_request.request_id == 301U);
    assert(client.active_request.session_id == 401U);
}

static void test_scan_prefetch_waits_for_credit(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence = {0};
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init_with_epoch(&client, &transport, 1000U, 2000U,
                                       0x0123456789ABCDEFULL);
    mock.observe_client = &client;
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    assert(first.credit == 1U);
    assert(first.boot_epoch == 0x0123456789ABCDEFULL);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid != 0U);
    second = client.prefetched_request;
    assert(second.center_index == 1U);
    assert(second.credit == 0U);
    assert(second.boot_epoch == first.boot_epoch);
    assert(mock.sends == 2U);

    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 13U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_READY,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    assert(client.prefetch_ready != 0U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    assert(client.prefetch_state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 15U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 16U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);

    evidence.session_id = first.session_id;
    evidence.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    evidence.iqsc_complete = true;
    evidence.payload_complete = true;
    evidence.crc_present = true;
    evidence.crc_valid = true;
    evidence.analysis_complete = true;
    evidence.cpu1_visible = true;
    sdr_control_client_observe_window(&client, &evidence, 18U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(sdr_control_client_expected_session(&client) == second.session_id);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.credit == 1U);
    assert(mock.ack_observations == 1U);
    assert(mock.ack_expected_session_during_send == second.session_id);
    assert(mock.ack_state_during_send ==
           SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 19U);
    assert(client.active_request.request_id == second.request_id);
    assert(client.active_request.session_id == second.session_id);
    assert(client.active_request.center_index == 1U);
    assert(client.prefetch_valid == 0U);
    sdr_control_client_notify_iqsc_start(&client, second.session_id,
                                         second.center_index, 20U);
    assert(client.prefetch_valid != 0U); /* Center 2 is now being prepared. */
    assert(client.prefetched_request.center_index == 2U);
}

static void start_active_window(sdr_control_client_t *client,
                                const ra8p1_sdr_control_message_t *request,
                                uint32_t now_ms)
{
    send_response_for(client, request, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, now_ms);
    sdr_control_client_notify_iqsc_start(client, request->session_id,
                                         request->center_index, now_ms + 1U);
}

static void finish_active_window(sdr_control_client_t *client,
                                 const ra8p1_sdr_control_message_t *request,
                                 uint32_t now_ms)
{
    sdr_control_window_evidence_t evidence;

    send_response_for(client, request, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, now_ms);
    make_clean_evidence(&evidence, request->session_id);
    sdr_control_client_observe_window(client, &evidence, now_ms + 1U);
    assert(client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
}

/* Drive one complete four-center sweep to the final ACK, but deliberately
 * leave the center-3 CREDIT_ACCEPTED response to the caller.  Continuous
 * scans must already own a credit=0 center-0 prefetch at that point. */
static ra8p1_sdr_control_message_t drive_four_center_scan_to_final_ack(
    sdr_control_client_t *client,
    ra8p1_sdr_control_message_t *wrapped_prefetch)
{
    mock_transport_t *mock = (mock_transport_t *)client->transport.context;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t third;
    ra8p1_sdr_control_message_t fourth;
    ra8p1_sdr_control_message_t outbound;

    first = client->active_request;
    assert(first.center_index == 0U);
    send_response_for(client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client->prefetched_request;
    assert(second.center_index == 1U);
    send_response_for(client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    start_active_window(client, &first, 13U);
    finish_active_window(client, &first, 15U);
    assert(client->pending_ack.credit == 1U);
    send_response_for(client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    assert(client->active_request.session_id == second.session_id);

    start_active_window(client, &second, 18U);
    third = client->prefetched_request;
    assert(third.center_index == 2U);
    send_response_for(client, &third, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 20U);
    finish_active_window(client, &second, 21U);
    assert(client->pending_ack.credit == 1U);
    send_response_for(client, &second, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 23U);
    assert(client->active_request.session_id == third.session_id);

    start_active_window(client, &third, 24U);
    fourth = client->prefetched_request;
    assert(fourth.center_index == 3U);
    send_response_for(client, &fourth, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 26U);
    finish_active_window(client, &third, 27U);
    assert(client->pending_ack.credit == 1U);
    send_response_for(client, &third, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 29U);
    assert(client->active_request.session_id == fourth.session_id);

    start_active_window(client, &fourth, 30U);
    if (client->repeat_scan != 0U)
    {
        assert(wrapped_prefetch != NULL);
        assert(client->prefetch_valid != 0U);
        *wrapped_prefetch = client->prefetched_request;
        assert(wrapped_prefetch->center_index == 0U);
        assert(wrapped_prefetch->credit == 0U);
        assert(ra8p1_sdr_control_decode(mock->wire,
                                        sizeof(mock->wire),
                                        &outbound));
        assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
        assert(outbound.request_id == wrapped_prefetch->request_id);
        assert(outbound.session_id == wrapped_prefetch->session_id);
        send_response_for(client, wrapped_prefetch,
                          RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                          RA8P1_SDR_CONTROL_STATUS_OK, 31U);
    }
    else
    {
        assert(wrapped_prefetch == NULL);
        assert(client->prefetch_valid == 0U);
    }
    finish_active_window(client, &fourth, 32U);
    assert(client->pending_ack.credit ==
           ((client->repeat_scan != 0U) ? 1U : 0U));
    return fourth;
}

static void test_finite_scan_completes_after_center_three(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t fourth;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 23000U, 24000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    assert(client.repeat_scan == 0U);
    fourth = drive_four_center_scan_to_final_ack(&client, NULL);
    send_response_for(&client, &fourth, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 34U);
    assert(client.stats.completed_windows == 4U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_COMPLETE);
    assert(client.active_request.session_id == fourth.session_id);
}

static void test_continuous_scan_prefetches_round_boundary(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t fourth;
    ra8p1_sdr_control_message_t wrapped;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 25000U, 26000U);
    assert(sdr_control_client_start_continuous_scan(&client, &options, 10U));
    assert(client.repeat_scan != 0U);
    fourth = drive_four_center_scan_to_final_ack(&client, &wrapped);

    /* The next identity and SDR preparation overlap center 3, while IQSC data
     * still cannot start until the accepted final ACK releases credit. */
    assert(client.active_request.session_id == fourth.session_id);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.prefetch_valid != 0U);
    assert(client.prefetched_request.session_id == wrapped.session_id);
    send_response_for(&client, &fourth, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 34U);

    assert(client.stats.completed_windows == 4U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED);
    assert(client.active_request.request_id == wrapped.request_id);
    assert(client.active_request.session_id == wrapped.session_id);
    assert(wrapped.center_index == 0U);
    assert(wrapped.credit == 0U);
    assert(wrapped.request_id != fourth.request_id);
    assert(wrapped.session_id != fourth.session_id);
    assert(client.prefetch_valid == 0U);
    sdr_control_client_notify_iqsc_start(&client, wrapped.session_id,
                                         wrapped.center_index, 35U);
    assert(client.prefetch_valid != 0U);
    assert(client.prefetched_request.center_index == 1U);
}

static void test_continuous_single_prefetches_same_center(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t third;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 27000U, 28000U);
    assert(sdr_control_client_start_continuous_single(
        &client, 3U, &options, 10U));
    assert(client.scan_all == 0U);
    assert(client.repeat_scan != 0U);
    first = client.active_request;
    assert(first.center_index == 3U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid != 0U);
    second = client.prefetched_request;
    assert(second.center_index == first.center_index);
    assert(second.center_frequency_hz == first.center_frequency_hz);
    assert(second.request_id != first.request_id);
    assert(second.session_id != first.session_id);
    assert(second.credit == 0U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_READY,
                      RA8P1_SDR_CONTROL_STATUS_OK, 13U);
    start_active_window(&client, &first, 14U);
    finish_active_window(&client, &first, 16U);

    /* The prefetched identity remains speculative until the active ACK is
     * accepted, even though both requests select the same center. */
    assert(client.pending_ack.credit == 1U);
    assert(client.active_request.session_id == first.session_id);
    assert(!sdr_control_client_start_continuous_single(
        &client, 1U, &options, 18U));
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 19U);

    assert(client.stats.completed_windows == 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED);
    assert(client.active_request.request_id == second.request_id);
    assert(client.active_request.session_id == second.session_id);
    assert(client.active_request.center_index == 3U);
    assert(client.stats.prefetched_windows == 1U);
    assert(client.prefetch_valid == 0U);

    start_active_window(&client, &second, 20U);
    assert(client.prefetch_valid != 0U);
    third = client.prefetched_request;
    assert(third.center_index == second.center_index);
    assert(third.request_id != second.request_id);
    assert(third.session_id != second.session_id);
    assert(third.credit == 0U);

    /* A focus change is a mode transition, not a re-entrant START. */
    assert(!sdr_control_client_start_continuous_single(
        &client, 1U, &options, 22U));
    assert(sdr_control_client_cancel(&client, 23U));
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(client.terminal_cancel_count == 2U);
    assert(sdr_control_client_expected_request(&client) == NULL);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == second.session_id);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 24U);
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == third.session_id);
    send_response_for(&client, &third, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 25U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
    assert(sdr_control_client_start_continuous_single(
        &client, 1U, &options, 26U));
    assert(client.active_request.center_index == 1U);
}

static void test_continuous_single_lost_prefetch_uses_same_center_fallback(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t lost;
    ra8p1_sdr_control_message_t cancel;
    ra8p1_sdr_control_message_t fallback;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 29000U, 30000U);
    assert(sdr_control_client_start_continuous_single(
        &client, 3U, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    lost = client.prefetched_request;
    assert(lost.center_index == 3U);
    assert(client.prefetch_state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);

    start_active_window(&client, &first, 12U);
    finish_active_window(&client, &first, 14U);
    assert(client.pending_ack.credit == 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 16U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(client.fallback_pending != 0U);
    cancel = client.fallback_cancel_request;
    assert(cancel.request_id == lost.request_id);
    assert(cancel.session_id == lost.session_id);
    assert(cancel.center_index == 3U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == lost.session_id);

    send_response_for(&client, &cancel, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 17U);
    fallback = client.active_request;
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(fallback.center_index == 3U);
    assert(fallback.center_frequency_hz == first.center_frequency_hz);
    assert(fallback.request_id != lost.request_id);
    assert(fallback.session_id != lost.session_id);
    assert(fallback.credit == 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.center_index == 3U);
    assert(outbound.credit == 1U);
}

static void test_prefetch_credit_without_ready(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence = {0};
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 3000U, 4000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid != 0U);
    second = client.prefetched_request;
    assert(mock.sends == 2U);

    /* A response for this exact speculative request is the ownership proof.
     * CAPTURE_READY may still be lost; the accepted slot remains promotable. */
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U + 1U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 13U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    evidence.session_id = first.session_id;
    evidence.ring_free = RA8P1_SDR_CONTROL_RING_SLOTS;
    evidence.iqsc_complete = true;
    evidence.payload_complete = true;
    evidence.crc_present = true;
    evidence.crc_valid = true;
    evidence.analysis_complete = true;
    evidence.cpu1_visible = true;
    sdr_control_client_observe_window(&client, &evidence, 15U);
    assert(client.active_result_ready != 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(mock.sends == 3U); /* Credit need not wait for CAPTURE_READY. */
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);

    assert(outbound.credit == 1U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 16U);
    assert(client.active_request.center_index == 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_STARTED);
    assert(client.stats.prefetch_credit_without_ready == 1U);
    assert(client.prefetch_valid == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    /* CAPTURE_ACCEPTED is already a delivery proof.  Promotion must not issue
     * a redundant CAPTURE_REQ or recapture the same window. */
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);

    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
}

static void test_prefetch_without_response_is_not_promoted(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t cancel;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 3500U, 4500U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    assert(client.prefetch_state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 13U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    make_clean_evidence(&evidence, first.session_id);
    sdr_control_client_observe_window(&client, &evidence, 15U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.pending_ack.credit == 0U);

    /* A locally queued UDP send is not proof that the SDR accepted this slot.
     * Withhold credit so cancelling an actually-lost request cannot leave an
     * orphan token bound to an accept order that never existed. */
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 16U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(client.active_request.center_index == first.center_index);
    assert(client.fallback_pending != 0U);
    cancel = client.fallback_cancel_request;
    assert(cancel.request_id == second.request_id);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.credit == 0U);

    send_response_for(&client, &cancel, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 17U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.active_request.center_index == second.center_index);
    assert(client.active_request.credit == 1U);
}

static void test_late_prefetch_response_cannot_upgrade_frozen_credit(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t cancel;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 3600U, 4600U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 13U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    make_clean_evidence(&evidence, first.session_id);
    sdr_control_client_observe_window(&client, &evidence, 15U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.pending_ack.credit == 0U);

    /* The ACK already froze credit=0.  These delayed prefetch responses prove
     * ownership, but cannot retroactively authorize IQSC transmission. */
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 16U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_READY,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    assert(client.prefetch_state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);
    assert(client.pending_ack.credit == 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 18U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(client.active_request.center_index == first.center_index);
    assert(client.fallback_pending != 0U);
    cancel = client.fallback_cancel_request;
    assert(cancel.request_id == second.request_id);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.credit == 0U);

    send_response_for(&client, &cancel, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 19U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.active_request.center_index == second.center_index);
    assert(client.active_request.credit == 1U);
}

static void drive_active_to_credit_wait(sdr_control_client_t *client,
                                        const ra8p1_sdr_control_message_t *first,
                                        uint32_t now_ms)
{
    sdr_control_window_evidence_t evidence;
    send_response_for(client, first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, now_ms);
    sdr_control_client_notify_iqsc_start(client, first->session_id,
                                         first->center_index, now_ms + 1U);
    send_response_for(client, first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, now_ms + 2U);
    make_clean_evidence(&evidence, first->session_id);
    sdr_control_client_observe_window(client, &evidence, now_ms + 3U);
    assert(client->stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
}

static void test_post_credit_cancel_rejects_stale_confirmation(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t cancel_before_credit;
    ra8p1_sdr_control_message_t cancel_after_credit;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 11000U, 12000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U + 1U);
    drive_active_to_credit_wait(&client, &first, 12U);
    assert(client.pending_ack.credit == 1U);

    send_response_for(&client, &second, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED, 16U);
    cancel_before_credit = client.fallback_cancel_request;
    assert(cancel_before_credit.attempt != 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    cancel_after_credit = client.fallback_cancel_request;
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(cancel_after_credit.attempt != cancel_before_credit.attempt);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.attempt == cancel_after_credit.attempt);

    send_response_for(&client, &cancel_before_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 18U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(client.fallback_cancel_pending != 0U);
    assert(client.active_request.center_index == first.center_index);

    send_response_for(&client, &cancel_after_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 19U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.active_request.center_index == second.center_index);
    assert(client.active_request.session_id != second.session_id);
}

static void test_cancel_before_credit_requires_second_generation(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t cancel_before_credit;
    ra8p1_sdr_control_message_t cancel_after_credit;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 13000U, 14000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U + 1U);
    drive_active_to_credit_wait(&client, &first, 12U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED, 16U);
    cancel_before_credit = client.fallback_cancel_request;
    send_response_for(&client, &cancel_before_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 17U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.fallback_cancel_confirmed != 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 18U);
    cancel_after_credit = client.fallback_cancel_request;
    assert(cancel_after_credit.attempt != cancel_before_credit.attempt);
    send_response_for(&client, &cancel_before_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 19U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    send_response_for(&client, &cancel_after_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 20U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.active_request.center_index == second.center_index);
}

static void test_iqsc_start_proves_credit_before_control_response(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    uint32_t invalid_before;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 14500U, 15500U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    drive_active_to_credit_wait(&client, &first, 13U);
    assert(client.pending_ack.credit == 1U);
    assert(client.expect_prefetched_iq != 0U);

    /* The data-plane START proves that the SDR already consumed the ACK's
     * credit even if the sparse CREDIT_ACCEPTED control datagram is delayed. */
    sdr_control_client_notify_iqsc_start(&client, second.session_id,
                                         second.center_index, 17U);
    assert(client.active_request.session_id == second.session_id);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
    assert(client.credit_proof_pending != 0U);
    invalid_before = client.stats.invalid_datagrams;

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 18U);
    assert(client.credit_proof_pending == 0U);
    assert(client.active_request.session_id == second.session_id);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
    assert(client.stats.invalid_datagrams == invalid_before);
}

static void test_reentrant_start_proof_survives_transport_failure(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 14525U, 15525U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    start_active_window(&client, &first, 13U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 15U);

    mock.observe_client = &client;
    mock.notify_iqsc_start_on_ack = 1U;
    mock.fail_after_ack_proof = 1U;
    mock.ack_notify_now_ms = 17U;
    make_clean_evidence(&evidence, first.session_id);
    sdr_control_client_observe_window(&client, &evidence, 16U);

    assert(mock.ack_observations == 1U);
    assert(mock.ack_expected_session_during_send == second.session_id);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
    assert(client.active_request.session_id == second.session_id);
    assert(client.credit_proof_pending != 0U);
    assert(client.stats.completed_windows == 1U);
    assert(client.prefetch_valid != 0U);
    assert(client.prefetched_request.center_index == 2U);

    /* The delayed control response is still consumed idempotently. */
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 18U);
    assert(client.credit_proof_pending == 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
    assert(client.active_request.session_id == second.session_id);
}

static void test_prefetch_complete_before_credit_retries_missing_start(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t outbound;
    const uint32_t prefetch_complete_ms = 20U;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 14550U, 15550U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 13U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_READY,
                      RA8P1_SDR_CONTROL_STATUS_OK, 14U);

    start_active_window(&client, &first, 15U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    make_clean_evidence(&evidence, first.session_id);
    sdr_control_client_observe_window(&client, &evidence, 18U);
    assert(client.pending_ack.credit == 1U);

    /* UDP ports can reorder the prefetched CAPTURE_COMPLETE ahead of the old
     * window's CREDIT_ACCEPTED.  Promotion must retain that proof. */
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK,
                      prefetch_complete_ms);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 21U);
    assert(client.active_request.session_id == second.session_id);
    assert(client.agent_complete != 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);

    sdr_control_client_poll(
        &client, prefetch_complete_ms + SDR_CONTROL_IQSC_START_RETRY_MS - 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);
    sdr_control_client_poll(
        &client, prefetch_complete_ms + SDR_CONTROL_IQSC_START_RETRY_MS);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.status == RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW);
    assert(outbound.request_id == second.request_id);
    assert(outbound.session_id == second.session_id);
    assert(outbound.credit == 0U);
    assert(outbound.sequence_gaps == 1U);
}

static void test_prefetch_timeout_then_delayed_credit_is_recancelled(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t cancel_before_credit;
    ra8p1_sdr_control_message_t cancel_after_credit;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    options.request_timeout_ms = options.ack_timeout_ms;
    sdr_control_client_init(&client, &transport, 14600U, 15600U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    drive_active_to_credit_wait(&client, &first, 13U);
    assert(client.pending_ack.credit == 1U);

    sdr_control_client_poll(&client,
                            client.prefetch_request_start_ms +
                            options.request_timeout_ms);
    assert(client.fallback_pending != 0U);
    assert(client.fallback_wait_credit != 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    cancel_before_credit = client.fallback_cancel_request;
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK,
                      client.prefetch_request_start_ms +
                      options.request_timeout_ms + 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    cancel_after_credit = client.fallback_cancel_request;
    assert(cancel_after_credit.attempt != cancel_before_credit.attempt);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.attempt == cancel_after_credit.attempt);

    send_response_for(&client, &cancel_before_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED,
                      client.prefetch_request_start_ms +
                      options.request_timeout_ms + 2U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    send_response_for(&client, &cancel_after_credit,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED,
                      client.prefetch_request_start_ms +
                      options.request_timeout_ms + 3U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(client.active_request.center_index == second.center_index);
    assert(client.active_request.session_id != second.session_id);
    assert(client.active_request.credit == 1U);
}

static void test_continuous_single_prefetch_retry_keeps_identity(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t second;
    ra8p1_sdr_control_message_t retry;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 15000U, 16000U);
    assert(sdr_control_client_start_continuous_single(
        &client, 3U, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    second = client.prefetched_request;
    assert(second.center_index == first.center_index);
    assert(second.center_index == 3U);
    assert(second.credit == 0U);
    send_response_for(&client, &second, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 13U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 14U);
    mock.failures_remaining = 1U;
    sdr_control_client_poll(&client, 12U + options.ack_timeout_ms);
    assert(client.prefetch_state == SDR_CONTROL_CLIENT_WAIT_STARTED);
    assert(client.prefetch_request_sent != 0U);
    mock.failures_remaining = 0U;
    sdr_control_client_poll(&client, 12U + 2U * options.ack_timeout_ms);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &retry));
    assert(retry.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(retry.request_id == second.request_id);
    assert(retry.session_id == second.session_id);
    assert(retry.center_index == second.center_index);
    assert(retry.attempt == second.attempt);
    assert((retry.flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) == 0U);
}

static void test_ack_retry_is_idempotent(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t first_ack;
    ra8p1_sdr_control_message_t retry_ack;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 17000U, 18000U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 10U));
    request = client.active_request;
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, request.session_id,
                                         request.center_index, 13U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                  RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    make_clean_evidence(&evidence, request.session_id);
    evidence.crc32c = 0x12345678U;
    sdr_control_client_observe_window(&client, &evidence, 15U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &first_ack));
    sdr_control_client_poll(&client, 15U + options.ack_timeout_ms);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &retry_ack));
    assert(retry_ack.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(retry_ack.request_id == first_ack.request_id);
    assert(retry_ack.session_id == first_ack.session_id);
    assert(retry_ack.status == first_ack.status);
    assert(retry_ack.credit == first_ack.credit);
    assert(retry_ack.window_crc32c == first_ack.window_crc32c);
    assert(retry_ack.attempt == first_ack.attempt + 1U);
    send_response(&client, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK,
                  16U + options.ack_timeout_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_COMPLETE);
}

static void test_prefetch_error_falls_back_to_serial(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t rejected_prefetch;
    ra8p1_sdr_control_message_t cancel;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 5000U, 6000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    rejected_prefetch = client.prefetched_request;
    send_response_for(&client, &rejected_prefetch,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED, 12U);
    assert(client.prefetch_valid == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == rejected_prefetch.session_id);
    cancel = client.fallback_cancel_request;

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 13U);
    assert(client.prefetch_valid == 0U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 14U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 15U);
    make_clean_evidence(&evidence, first.session_id);
    sdr_control_client_observe_window(&client, &evidence, 16U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.credit == 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 17U);
    /* The active credit ACK and the prefetch CANCEL can cross on UDP.  A new
     * credit=1 request is legal only after the SDR confirms the old slot is
     * released. */
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CANCELLED);
    assert(client.active_request.center_index == first.center_index);
    send_response_for(&client, &cancel, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 18U);
    assert(client.active_request.center_index == 1U);
    assert(client.active_request.session_id != rejected_prefetch.session_id);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.center_index == 1U);
}

static void test_missing_window_requests_cached_retry(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t outbound;
    const uint32_t complete_ms = 103U;
    const uint32_t first_retry_ms = complete_ms +
        SDR_CONTROL_IQSC_START_RETRY_MS;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 1U;
    sdr_control_client_init(&client, &transport, 7000U, 8000U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 100U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 101U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 102U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 103U);

    sdr_control_client_poll(&client, first_retry_ms - 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT);
    sdr_control_client_poll(&client, first_retry_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.pending_retransmit != 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.status == RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW);
    assert(outbound.credit == 0U);

    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CREDIT_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, first_retry_ms + 1U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_ACCEPTED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == first.request_id);
    assert(outbound.session_id == first.session_id);
    assert(outbound.attempt == 1U);
    assert((outbound.flags & RA8P1_SDR_CONTROL_FLAG_RETRANSMIT) != 0U);

    /* A delayed terminal response from attempt 0 must not complete the
     * retransmitted attempt with the same request/session identity. */
    {
        ra8p1_sdr_control_message_t stale = first;
        uint8_t stale_wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
        stale.command = RA8P1_SDR_CONTROL_CAPTURE_COMPLETE;
        stale.status = RA8P1_SDR_CONTROL_STATUS_OK;
        assert(ra8p1_sdr_control_encode(&stale, stale_wire));
        assert(!sdr_control_client_receive(&client,
                                           stale_wire,
                                           sizeof(stale_wire),
                                           first_retry_ms + 2U));
    }

    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, first_retry_ms + 2U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, first_retry_ms + 3U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                  RA8P1_SDR_CONTROL_STATUS_OK, first_retry_ms + 4U);
    sdr_control_client_poll(&client,
                            first_retry_ms + 4U +
                            SDR_CONTROL_IQSC_START_RETRY_MS);
    assert(client.stats.state == SDR_CONTROL_CLIENT_ERROR);
    assert(client.stats.last_status ==
           RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == first.session_id);
}

/* A debugger-induced ingress gap can put the active window into the normal
 * RETRY_WINDOW handshake just as CPU1 asks CPU0 to stop.  Once terminal
 * CANCEL has been accepted locally, no retry or result timeout may revive the
 * request.  This protects real UI STOP behavior from a host-side SWD halt. */
static void test_cancel_during_retry_suppresses_timeout(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t prefetched;
    ra8p1_sdr_control_message_t outbound;
    ra8p1_sdr_control_message_t delayed_credit;
    uint8_t delayed_wire[RA8P1_SDR_CONTROL_WIRE_BYTES];
    uint32_t sends_after_cancel;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 7200U, 8200U);
    assert(sdr_control_client_start_continuous_scan(&client, &options, 100U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 101U);
    assert(client.prefetch_valid != 0U);
    prefetched = client.prefetched_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 102U);
    sdr_control_client_notify_iqsc_start(&client, first.session_id,
                                         first.center_index, 103U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, 104U);
    make_clean_evidence(&evidence, first.session_id);
    evidence.sequence_gaps = 1U;
    sdr_control_client_observe_window(&client, &evidence, 105U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);
    assert(client.pending_retransmit != 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.status == RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW);
    assert(outbound.credit == 0U);

    assert(sdr_control_client_cancel(&client, 106U));
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(sdr_control_client_expected_request(&client) == NULL);
    assert(client.prefetch_valid != 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == first.session_id);

    delayed_credit = first;
    delayed_credit.command = RA8P1_SDR_CONTROL_CREDIT_ACCEPTED;
    delayed_credit.status = RA8P1_SDR_CONTROL_STATUS_OK;
    assert(ra8p1_sdr_control_encode(&delayed_credit, delayed_wire));
    assert(sdr_control_client_receive(&client, delayed_wire,
                                      sizeof(delayed_wire), 107U));
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 108U);
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == prefetched.session_id);
    send_response_for(&client, &prefetched, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 109U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
    assert(client.stats.last_status == RA8P1_SDR_CONTROL_STATUS_CANCELLED);
    assert(client.prefetch_valid == 0U);
    sends_after_cancel = mock.sends;
    sdr_control_client_poll(&client,
                            106U + (options.request_timeout_ms * 2U));
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
    assert(client.stats.last_status == RA8P1_SDR_CONTROL_STATUS_CANCELLED);
    assert(mock.sends == sends_after_cancel);
}

static void test_missing_complete_never_sends_window_ack(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t first;
    ra8p1_sdr_control_message_t outbound;
    const uint32_t first_timeout = 100U +
        SDR_CONTROL_DEFAULT_REQUEST_TIMEOUT_MS;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 2U;
    sdr_control_client_init(&client, &transport, 7100U, 8100U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 100U));
    first = client.active_request;
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 101U);
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 102U);

    /* The data/control terminal messages are both absent.  CPU0 may only
     * repeat CAPTURE_REQ to learn the agent state; it cannot assume WAIT_ACK. */
    sdr_control_client_poll(&client, first_timeout);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CAPTURE_REQ);
    assert(outbound.request_id == first.request_id);
    assert(outbound.session_id == first.session_id);
    assert(outbound.attempt == first.attempt);
    assert(client.pending_retransmit == 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_RECEIVING);
    assert(client.stats.timeouts == 1U);

    /* Polling before the bounded control retry interval must not flood the
     * agent or exhaust retries in one scheduler burst. */
    sdr_control_client_poll(&client, first_timeout + 1U);
    assert(mock.sends == 2U);
    assert(client.stats.timeouts == 1U);

    /* Once CAPTURE_COMPLETE proves WAIT_ACK, cached-window retry is legal. */
    send_response_for(&client, &first, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                      RA8P1_SDR_CONTROL_STATUS_OK, first_timeout + 2U);
    sdr_control_client_poll(&client, first_timeout + 2U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_WINDOW_ACK);
    assert(outbound.status == RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW);
    assert(outbound.credit == 0U);
    assert(client.pending_retransmit != 0U);
}

static void test_ack_timeout_sends_cancel(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    sdr_control_window_evidence_t evidence;
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 0U;
    sdr_control_client_init(&client, &transport, 9000U, 10000U);
    assert(sdr_control_client_start_single(&client, 3U, &options, 10U));
    request = client.active_request;
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_STARTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 12U);
    sdr_control_client_notify_iqsc_start(&client, request.session_id,
                                         request.center_index, 13U);
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_COMPLETE,
                  RA8P1_SDR_CONTROL_STATUS_OK, 14U);
    make_clean_evidence(&evidence, request.session_id);
    sdr_control_client_observe_window(&client, &evidence, 15U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED);

    sdr_control_client_poll(&client, 15U + options.ack_timeout_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_ERROR);
    assert(client.stats.last_status == RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.session_id == request.session_id);
}

static void test_partial_terminal_cancel_retains_prefetch_identity(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t active;
    ra8p1_sdr_control_message_t prefetched;
    ra8p1_sdr_control_message_t outbound;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 11000U, 12000U);
    assert(sdr_control_client_start_continuous_single(
        &client, 3U, &options, 10U));
    active = client.active_request;
    send_response(&client, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                  RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid != 0U);
    prefetched = client.prefetched_request;
    assert(active.center_index == 3U);
    assert(prefetched.center_index == active.center_index);
    assert(prefetched.request_id != active.request_id);
    assert(prefetched.session_id != active.session_id);

    /* Active CANCEL succeeds and the ordered prefetched CANCEL fails.  The
     * exact target remains retained and is retried at the 20 ms deadline. */
    mock.fail_on_call = mock.calls + 2U;
    assert(sdr_control_client_cancel(&client, 12U));
    assert(client.prefetch_valid != 0U);
    assert(client.prefetched_request.request_id == prefetched.request_id);
    assert(client.prefetched_request.session_id == prefetched.session_id);
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);

    send_response_for(&client, &active, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 13U);
    assert(client.terminal_cancel_index == 1U);
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(client.prefetch_valid != 0U);

    mock.fail_on_call = 0U;
    sdr_control_client_poll(&client,
                            13U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS - 1U);
    assert(client.terminal_cancel_retries == 0U);
    sdr_control_client_poll(&client,
                            13U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS);
    assert(client.terminal_cancel_retries == 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(outbound.request_id == prefetched.request_id);
    assert(outbound.session_id == prefetched.session_id);
    assert(outbound.attempt == prefetched.attempt);
    send_response_for(&client, &prefetched, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED,
                      14U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS);
    assert(client.prefetch_valid == 0U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
}

static void test_terminal_cancel_tracks_exact_attempts_in_order(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t active;
    ra8p1_sdr_control_message_t prefetched;
    ra8p1_sdr_control_message_t fallback;
    ra8p1_sdr_control_message_t outbound;
    uint32_t invalid_before;

    sdr_control_capture_options_default(&options);
    sdr_control_client_init(&client, &transport, 13000U, 14000U);
    assert(sdr_control_client_start_scan(&client, &options, 10U));
    active = client.active_request;
    send_response_for(&client, &active, RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED,
                      RA8P1_SDR_CONTROL_STATUS_OK, 11U);
    assert(client.prefetch_valid != 0U);
    prefetched = client.prefetched_request;

    /* Model the retained post-credit tombstone: it has the same request and
     * session identity as the prefetch, but a newer cancel generation. */
    fallback = prefetched;
    fallback.attempt++;
    if (fallback.attempt == 0U)
    {
        fallback.attempt = 1U;
    }
    client.fallback_pending = 1U;
    client.fallback_cancel_pending = 1U;
    client.fallback_cancel_generation = fallback.attempt;
    client.fallback_cancel_request = fallback;

    assert(sdr_control_client_cancel(&client, 12U));
    assert(client.terminal_cancel_count == 3U);
    assert(client.terminal_cancel_index == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.request_id == active.request_id);
    assert(outbound.attempt == active.attempt);

    /* A later target cannot complete early, even if its exact CANCELLED
     * datagram arrives before the active target is acknowledged. */
    send_response_for(&client, &prefetched, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 13U);
    assert(client.terminal_cancel_index == 0U);
    send_response_for(&client, &active, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 14U);
    assert(client.terminal_cancel_index == 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.request_id == prefetched.request_id);
    assert(outbound.attempt == prefetched.attempt);

    /* The fallback shares identity with the current prefetch target.  Its
     * newer attempt must be consumed as stale here, not advance the queue. */
    invalid_before = client.stats.invalid_datagrams;
    send_response_for(&client, &fallback, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 15U);
    assert(client.terminal_cancel_index == 1U);
    assert(client.stats.invalid_datagrams == invalid_before + 1U);
    send_response_for(&client, &prefetched, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 16U);
    assert(client.terminal_cancel_index == 2U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire), &outbound));
    assert(outbound.request_id == fallback.request_id);
    assert(outbound.attempt == fallback.attempt);

    invalid_before = client.stats.invalid_datagrams;
    send_response_for(&client, &prefetched, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 17U);
    assert(client.terminal_cancel_index == 2U);
    assert(client.stats.invalid_datagrams == invalid_before + 1U);
    send_response_for(&client, &fallback, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED, 18U);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
}

static void test_terminal_cancel_retry_is_20ms_and_cancel_is_idempotent(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    ra8p1_sdr_control_message_t request;
    ra8p1_sdr_control_message_t first_cancel;
    ra8p1_sdr_control_message_t retry_cancel;
    uint32_t sends_after_cancel;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 2U;
    sdr_control_client_init(&client, &transport, 15000U, 16000U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 100U));
    request = client.active_request;
    assert(sdr_control_client_cancel(&client, 101U));
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    assert(sdr_control_client_expected_request(&client) == NULL);
    assert(sdr_control_client_expected_session(&client) == 0U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire),
                                    &first_cancel));
    assert(first_cancel.command == RA8P1_SDR_CONTROL_CANCEL);
    sends_after_cancel = mock.sends;

    assert(sdr_control_client_cancel(&client, 102U));
    assert(mock.sends == sends_after_cancel);
    assert(client.terminal_cancel_index == 0U);
    sdr_control_client_poll(
        &client, 101U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS - 1U);
    assert(mock.sends == sends_after_cancel);
    sdr_control_client_poll(
        &client, 101U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS);
    assert(mock.sends == sends_after_cancel + 1U);
    assert(ra8p1_sdr_control_decode(mock.wire, sizeof(mock.wire),
                                    &retry_cancel));
    assert(retry_cancel.command == RA8P1_SDR_CONTROL_CANCEL);
    assert(retry_cancel.request_id == first_cancel.request_id);
    assert(retry_cancel.session_id == first_cancel.session_id);
    assert(retry_cancel.attempt == first_cancel.attempt);

    send_response_for(&client, &request, RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED,
                      102U + SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
}

static void test_error_requires_cancel_before_restart(void)
{
    mock_transport_t mock = {0};
    sdr_control_transport_t transport = {mock_send, &mock};
    sdr_control_capture_options_t options;
    sdr_control_client_t client;
    uint32_t failed_request_id;

    sdr_control_capture_options_default(&options);
    options.retry_limit = 0U;
    sdr_control_client_init(&client, &transport, 21000U, 22000U);
    assert(sdr_control_client_start_single(&client, 0U, &options, 10U));
    failed_request_id = client.active_request.request_id;

    /* The timeout cleanup CANCEL also fails.  ERROR must remain quarantined:
     * issuing a new capture could otherwise race the unresolved SDR slot. */
    mock.failures_remaining = 1U;
    sdr_control_client_poll(&client, 10U + options.ack_timeout_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_ERROR);
    assert(!sdr_control_client_start_single(&client, 1U, &options,
                                            11U + options.ack_timeout_ms));
    assert(client.active_request.request_id == failed_request_id);

    mock.failures_remaining = 0U;
    assert(sdr_control_client_cancel(&client,
                                     12U + options.ack_timeout_ms));
    assert(client.stats.state ==
           SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED);
    send_response_for(&client, &client.active_request,
                      RA8P1_SDR_CONTROL_ERROR,
                      RA8P1_SDR_CONTROL_STATUS_CANCELLED,
                      13U + options.ack_timeout_ms);
    assert(client.stats.state == SDR_CONTROL_CLIENT_CANCELLED);
    assert(sdr_control_client_start_single(&client, 1U, &options,
                                            14U + options.ack_timeout_ms));
    assert(client.active_request.request_id != failed_request_id);
}

int main(void)
{
    test_single_window_gate();
    test_request_retry_is_idempotent();
    test_missing_complete_uses_bounded_completion_probes();
    test_iqsc_end_fast_probe_runs_before_analysis_complete();
    test_iqsc_end_delayed_poll_sends_one_completion_probe();
    test_transient_send_failure_keeps_request();
    test_scan_prefetch_waits_for_credit();
    test_finite_scan_completes_after_center_three();
    test_continuous_scan_prefetches_round_boundary();
    test_continuous_single_prefetches_same_center();
    test_continuous_single_lost_prefetch_uses_same_center_fallback();
    test_prefetch_credit_without_ready();
    test_prefetch_without_response_is_not_promoted();
    test_late_prefetch_response_cannot_upgrade_frozen_credit();
    test_post_credit_cancel_rejects_stale_confirmation();
    test_cancel_before_credit_requires_second_generation();
    test_iqsc_start_proves_credit_before_control_response();
    test_reentrant_start_proof_survives_transport_failure();
    test_prefetch_complete_before_credit_retries_missing_start();
    test_prefetch_timeout_then_delayed_credit_is_recancelled();
    test_continuous_single_prefetch_retry_keeps_identity();
    test_ack_retry_is_idempotent();
    test_prefetch_error_falls_back_to_serial();
    test_missing_window_requests_cached_retry();
    test_cancel_during_retry_suppresses_timeout();
    test_missing_complete_never_sends_window_ack();
    test_ack_timeout_sends_cancel();
    test_partial_terminal_cancel_retains_prefetch_identity();
    test_terminal_cancel_tracks_exact_attempts_in_order();
    test_terminal_cancel_retry_is_20ms_and_cancel_is_idempotent();
    test_error_requires_cancel_before_restart();
    puts("SDRC client host tests passed");
    return 0;
}
