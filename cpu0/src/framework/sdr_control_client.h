#ifndef SDR_CONTROL_CLIENT_H
#define SDR_CONTROL_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdr_control_protocol.h"

#define SDR_CONTROL_PEER_IP                    "192.168.31.10"
#define SDR_CONTROL_DEFAULT_TARGET_MBPS_X1000  (500000UL)
/* Batch several MTU-sized IQ datagrams per sendmmsg call.  The previous
 * single-datagram default made the SDR userspace sender spend most of the
 * window in syscall/interrupt overhead (measured around 130 Mbps). */
/* Keep each paced wire burst below one quarter of the 96-descriptor RMAC RX
 * queue.  Sixteen datagrams balance sender syscall overhead against CPU0 ring
 * drain latency while preserving the 500 Mbps average-rate target. */
#define SDR_CONTROL_DEFAULT_SEND_BATCH         (16U)
#define SDR_CONTROL_DEFAULT_ACK_TIMEOUT_MS      (1000U)
#define SDR_CONTROL_DEFAULT_REQUEST_TIMEOUT_MS  (10000U)
#define SDR_CONTROL_DEFAULT_RETRY_LIMIT         (3U)
/* IQSC END is sent before the SDR control worker publishes CAPTURE_COMPLETE.
 * CPU0 must not ACK until that terminal control response has arrived, but a
 * lost response must also not insert the normal 1 s ACK-retry delay into the
 * four-frequency critical path.  Repeating the same CAPTURE_REQ is
 * idempotent at the agent, so use a small bounded probe backoff while waiting
 * for CAPTURE_COMPLETE. */
#define SDR_CONTROL_COMPLETION_FAST_PROBE_MS       (4U)
#define SDR_CONTROL_COMPLETION_PROBE_1_MS         (20U)
#define SDR_CONTROL_COMPLETION_PROBE_2_MS         (40U)
#define SDR_CONTROL_COMPLETION_PROBE_MAX_MS       (80U)
#define SDR_CONTROL_TERMINAL_CANCEL_RETRY_MS       (20U)
#define SDR_CONTROL_TERMINAL_CANCEL_TARGETS         (3U)
/* CAPTURE_COMPLETE proves that the agent has retained the exact IQ window and
 * is in WAIT_ACK.  If its IQSC START never became locally visible, request the
 * cached window promptly instead of waiting the 10 s result deadline. */
#define SDR_CONTROL_IQSC_START_RETRY_MS             (4U)

typedef bool (*sdr_control_send_fn)(void *context,
                                    const uint8_t *data,
                                    size_t length);

typedef struct st_sdr_control_transport
{
    sdr_control_send_fn send;
    void *context;
} sdr_control_transport_t;

typedef struct st_sdr_control_capture_options
{
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t sample_count;
    uint32_t target_payload_mbps_x1000;
    uint16_t send_batch;
    uint16_t retry_limit;
    uint32_t ack_timeout_ms;
    uint32_t request_timeout_ms;
    uint16_t flags;
    uint32_t test_fault_flags;
} sdr_control_capture_options_t;

typedef struct st_sdr_control_window_evidence
{
    uint32_t session_id;
    uint32_t ring_free;
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid_packets;
    uint32_t ring_drops;
    uint32_t ring_full_drops;
    uint32_t ring_oversize_drops;
    uint32_t crc_errors;
    uint32_t crc32c;
    uint32_t actual_payload_mbps_x1000;
    bool iqsc_complete;
    bool payload_complete;
    bool crc_present;
    bool crc_valid;
    bool analysis_complete;
    bool cpu1_visible;
} sdr_control_window_evidence_t;

typedef enum e_sdr_control_client_state
{
    SDR_CONTROL_CLIENT_IDLE = 0,
    SDR_CONTROL_CLIENT_WAIT_ACCEPTED = 1,
    SDR_CONTROL_CLIENT_WAIT_STARTED = 2,
    SDR_CONTROL_CLIENT_RECEIVING = 3,
    SDR_CONTROL_CLIENT_WAIT_LOCAL_RESULT = 4,
    SDR_CONTROL_CLIENT_WAIT_CREDIT_ACCEPTED = 5,
    SDR_CONTROL_CLIENT_COMPLETE = 6,
    SDR_CONTROL_CLIENT_CANCELLED = 7,
    SDR_CONTROL_CLIENT_ERROR = 8,
    /* A speculative slot is being cancelled before serial fallback.  Keep
     * this distinct from WAIT_CREDIT_ACCEPTED so the ACK retry timer cannot
     * resend an already-applied window ACK while the SDR releases its token. */
    SDR_CONTROL_CLIENT_WAIT_CANCELLED = 9,
    /* An operator STOP is not complete until the SDR has acknowledged every
     * retained request identity.  Targets are cancelled in ownership order so
     * a delayed credit-bearing WINDOW_ACK cannot revive a prefetched slot. */
    SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED = 10
} sdr_control_client_state_t;

typedef struct st_sdr_control_client_stats
{
    uint32_t state;
    uint32_t request_id;
    uint32_t session_id;
    uint32_t center_index;
    uint32_t completed_windows;
    uint32_t tx_datagrams;
    uint32_t rx_datagrams;
    uint32_t invalid_datagrams;
    uint32_t retries;
    uint32_t timeouts;
    uint32_t last_status;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;
    uint32_t request_start_ms;
    uint32_t request_elapsed_ms;
    uint32_t actual_payload_mbps_x1000;
    uint32_t window_crc32c;
    uint32_t last_message_crc32c;
    uint64_t agent_request_rx_us;
    uint64_t tune_start_us;
    uint64_t tune_complete_us;
    uint64_t capture_start_us;
    uint64_t capture_complete_us;
    uint64_t boot_epoch;
    uint32_t prefetched_request_id;
    uint32_t prefetched_session_id;
    uint32_t prefetched_center_index;
    uint32_t prefetch_state;
    uint32_t prefetched_windows;
    uint32_t missing_capture_complete;
    uint32_t prefetch_credit_without_ready;
    uint32_t prefetch_iqsc_credit_proofs;
} sdr_control_client_stats_t;

/* SWD tools consume this proof object by fixed offsets.  Fail the firmware
 * build if an otherwise innocent field edit silently breaks that ABI. */
typedef char sdr_control_client_stats_must_be_152_bytes[
    (sizeof(sdr_control_client_stats_t) == 152U) ? 1 : -1];
typedef char sdr_control_client_stats_boot_epoch_must_be_at_112[
    (offsetof(sdr_control_client_stats_t, boot_epoch) == 112U) ? 1 : -1];
typedef char sdr_control_client_stats_tail_must_be_at_148[
    (offsetof(sdr_control_client_stats_t,
              prefetch_iqsc_credit_proofs) == 148U) ? 1 : -1];

typedef struct st_sdr_control_client
{
    sdr_control_transport_t transport;
    sdr_control_capture_options_t options;
    ra8p1_sdr_control_message_t active_request;
    ra8p1_sdr_control_message_t prefetched_request;
    ra8p1_sdr_control_message_t pending_ack;
    ra8p1_sdr_control_message_t last_response;
    sdr_control_client_stats_t stats;
    uint32_t next_request_id;
    uint32_t next_session_id;
    uint32_t last_tx_ms;
    uint32_t last_rx_ms;
    uint32_t request_start_ms;
    uint32_t iqsc_end_ms;
    uint32_t completion_probe_last_tx_ms;
    uint32_t prefetch_last_tx_ms;
    uint32_t prefetch_request_start_ms;
    uint32_t current_center_index;
    uint32_t prefetch_state;
    uint32_t agent_complete_ms;
    uint32_t prefetch_agent_complete_ms;
    uint64_t boot_epoch;
    uint8_t scan_all;
    /* Continuous mode advances only after the current WINDOW_ACK has been
     * accepted by the SDR.  It either wraps a four-center sweep or repeats the
     * selected center, according to scan_all. */
    uint8_t repeat_scan;
    uint8_t pending_retransmit;
    uint8_t agent_complete;
    uint8_t prefetch_agent_complete;
    uint8_t iqsc_started;
    uint8_t request_sent;
    uint8_t prefetch_valid;
    uint8_t prefetch_ready;
    uint8_t prefetch_request_sent;
    uint8_t expect_prefetched_iq;
    uint8_t active_result_ready;
    /* Set only after a complete, CRC-valid IQSC window was observed. */
    uint8_t active_data_valid;
    uint8_t iqsc_end_seen;
    uint8_t missing_complete_counted;
    uint8_t fast_complete_probe_sent;
    uint8_t completion_probe_retries;
    /* Internal control-plane retry/ownership bookkeeping.  These fields are
     * intentionally outside the wire message and do not change the SDRC ABI. */
    uint8_t active_control_retries;
    uint8_t prefetch_control_retries;
    uint8_t fallback_cancel_retries;
    uint8_t prefetch_abandoned;
    uint8_t fallback_pending;
    uint8_t fallback_cancel_pending;
    uint8_t fallback_cancel_confirmed;
    uint8_t fallback_wait_credit;
    uint8_t active_from_prefetch;
    uint32_t fallback_center_index;
    uint32_t fallback_cancel_last_tx_ms;
    uint32_t fallback_cancel_generation;
    ra8p1_sdr_control_message_t fallback_cancel_request;
    ra8p1_sdr_control_message_t credit_proof_request;
    uint8_t credit_proof_pending;
    /* Terminal STOP snapshots at most active + prefetch + fallback.  Identity
     * plus attempt is the local key because one retained SDR slot can have a
     * newer fallback cancellation generation.  Neither the SDRC wire ABI nor
     * the exported stats ABI is changed. */
    ra8p1_sdr_control_message_t
        terminal_cancel_requests[SDR_CONTROL_TERMINAL_CANCEL_TARGETS];
    uint32_t terminal_cancel_last_tx_ms;
    uint8_t terminal_cancel_count;
    uint8_t terminal_cancel_index;
    uint8_t terminal_cancel_retries;
    uint8_t terminal_cancel_tx_succeeded;
} sdr_control_client_t;

void sdr_control_capture_options_default(sdr_control_capture_options_t *options);
void sdr_control_client_init(sdr_control_client_t *client,
                             const sdr_control_transport_t *transport,
                             uint32_t request_seed,
                             uint32_t session_seed);
void sdr_control_client_init_with_epoch(
    sdr_control_client_t *client,
    const sdr_control_transport_t *transport,
    uint32_t request_seed,
    uint32_t session_seed,
    uint64_t boot_epoch);
bool sdr_control_client_start_single(sdr_control_client_t *client,
                                     uint32_t center_index,
                                     const sdr_control_capture_options_t *options,
                                     uint32_t now_ms);
/* Repeat one selected center.  Prepare the next fresh request/session with
 * credit=0 while the current window is active, then promote it only after the
 * preceding WINDOW_ACK/CREDIT_ACCEPTED gate completes. */
bool sdr_control_client_start_continuous_single(
    sdr_control_client_t *client,
    uint32_t center_index,
    const sdr_control_capture_options_t *options,
    uint32_t now_ms);
bool sdr_control_client_start_scan(sdr_control_client_t *client,
                                   const sdr_control_capture_options_t *options,
                                   uint32_t now_ms);
/* Start a CPU0-owned repeating 0 -> 1 -> 2 -> 3 scan.  Center 0 of the next
 * round is prepared with credit=0 while center 3 is active, then promoted
 * only after the final WINDOW_ACK/CREDIT_ACCEPTED pair. */
bool sdr_control_client_start_continuous_scan(
    sdr_control_client_t *client,
    const sdr_control_capture_options_t *options,
    uint32_t now_ms);
bool sdr_control_client_cancel(sdr_control_client_t *client, uint32_t now_ms);
void sdr_control_client_poll(sdr_control_client_t *client, uint32_t now_ms);
bool sdr_control_client_receive(sdr_control_client_t *client,
                                const uint8_t *wire,
                                size_t length,
                                uint32_t now_ms);
bool sdr_control_client_session_matches(const sdr_control_client_t *client,
                                        uint32_t session_id,
                                        uint32_t center_index);
void sdr_control_client_notify_iqsc_start(sdr_control_client_t *client,
                                          uint32_t session_id,
                                          uint32_t center_index,
                                          uint32_t now_ms);
void sdr_control_client_notify_iqsc_end(sdr_control_client_t *client,
                                        uint32_t session_id,
                                        uint32_t center_index,
                                        uint32_t now_ms);
void sdr_control_client_observe_window(
    sdr_control_client_t *client,
    const sdr_control_window_evidence_t *evidence,
    uint32_t now_ms);
uint32_t sdr_control_client_expected_session(
    const sdr_control_client_t *client);
const ra8p1_sdr_control_message_t *sdr_control_client_expected_request(
    const sdr_control_client_t *client);
void sdr_control_client_stats_get(const sdr_control_client_t *client,
                                  uint32_t now_ms,
                                  sdr_control_client_stats_t *stats);

#endif
