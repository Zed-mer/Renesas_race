#ifndef CPU1_CAMPAIGN_CONTROL_H
#define CPU1_CAMPAIGN_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_stream.h"

#define RA8P1_CPU1_CAMPAIGN_REQUEST_MAGIC       (0x51525043UL) /* "CPRQ" */
#define RA8P1_CPU1_CAMPAIGN_PROOF_MAGIC         (0x46525043UL) /* "CPRF" */
#define RA8P1_CPU1_CAMPAIGN_VERSION             (1U)
#define RA8P1_CPU1_CAMPAIGN_REQUEST_BYTES       (64U)
#define RA8P1_CPU1_CAMPAIGN_PROOF_BYTES         (128U)
#define RA8P1_CPU1_CAMPAIGN_COMPLETE_MAGIC      (0x454E4F44UL) /* "DONE" */
#define RA8P1_CPU1_CAMPAIGN_FAILURE_MAGIC       (0x4C494146UL) /* "FAIL" */
#define RA8P1_CPU1_CAMPAIGN_MAX_ITERATIONS      (100000U)

typedef enum e_ra8p1_cpu1_campaign_mode
{
    RA8P1_CPU1_CAMPAIGN_MODE_NONE = 0U,
    RA8P1_CPU1_CAMPAIGN_MODE_STOP = 1U,
    RA8P1_CPU1_CAMPAIGN_MODE_SINGLE = 2U,
    RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP = 3U,
    RA8P1_CPU1_CAMPAIGN_MODE_FOUR_SERIAL = 4U
} ra8p1_cpu1_campaign_mode_t;

typedef enum e_ra8p1_cpu1_campaign_state
{
    RA8P1_CPU1_CAMPAIGN_STATE_UNINITIALIZED = 0U,
    RA8P1_CPU1_CAMPAIGN_STATE_READY = 1U,
    RA8P1_CPU1_CAMPAIGN_STATE_STOPPING = 2U,
    RA8P1_CPU1_CAMPAIGN_STATE_ARMING = 3U,
    RA8P1_CPU1_CAMPAIGN_STATE_RUNNING = 4U,
    RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT = 5U,
    RA8P1_CPU1_CAMPAIGN_STATE_COMPLETE = 6U,
    RA8P1_CPU1_CAMPAIGN_STATE_STOPPED = 7U,
    RA8P1_CPU1_CAMPAIGN_STATE_ERROR = 8U
} ra8p1_cpu1_campaign_state_t;

typedef enum e_ra8p1_cpu1_campaign_error
{
    RA8P1_CPU1_CAMPAIGN_ERROR_NONE = 0U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_HEADER = 1U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_MODE = 2U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_CENTER = 3U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_ITERATIONS = 4U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_RATE = 5U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_FAULT_FLAGS = 6U,
    RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_FLAGS = 7U,
    RA8P1_CPU1_CAMPAIGN_ERROR_REQUEST_ID_REUSED = 8U,
    RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_SEND = 9U,
    RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_REJECTED = 10U,
    RA8P1_CPU1_CAMPAIGN_ERROR_RESULT_ORDER = 11U
} ra8p1_cpu1_campaign_error_t;

/* Host-owned, fixed ABI. The host writes begin odd, payload, end even, then
 * begin even. CPU1 invalidates this object before every read. */
typedef struct st_ra8p1_cpu1_campaign_request
{
    uint32_t begin_sequence;
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t request_id;
    uint32_t mode;
    uint32_t center_index;
    uint32_t iterations;
    uint32_t target_payload_mbps_x1000;
    uint32_t test_fault_flags;
    uint32_t flags;
    uint32_t reserved[5];
    uint32_t end_sequence;
} ra8p1_cpu1_campaign_request_t;

/* CPU1-owned, fixed ABI. For four-center modes, iterations means complete
 * rounds and windows_expected is iterations * 4. For single mode, iterations
 * and windows_expected are identical. windows_visible counts validated frames
 * copied into the CPU1-owned UI model; GLCDC presentation is measured
 * separately. */
typedef struct st_ra8p1_cpu1_campaign_proof
{
    uint32_t begin_sequence;
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t request_id;
    uint32_t request_begin_sequence;
    uint32_t state;
    uint32_t mode;
    uint32_t configured_center_index;
    uint32_t iterations_requested;
    uint32_t iterations_completed;
    uint32_t windows_expected;
    uint32_t windows_visible;
    uint32_t next_center_index;
    uint32_t active_center_index;
    uint32_t target_payload_mbps_x1000;
    uint32_t test_fault_flags;
    uint32_t campaign_flags;
    uint32_t last_session_id;
    uint32_t last_window_sequence;
    uint32_t last_result_center_index;
    uint32_t last_command_sequence;
    uint32_t last_command_status;
    uint32_t last_command_reason;
    uint32_t last_applied_session_id;
    uint32_t command_send_retries;
    uint32_t busy_retries;
    uint32_t rejected_requests;
    uint32_t duplicate_requests;
    uint32_t unexpected_results;
    uint32_t last_error;
    uint32_t terminal_magic;
    uint32_t end_sequence;
} ra8p1_cpu1_campaign_proof_t;

extern volatile ra8p1_cpu1_campaign_request_t g_cpu1_campaign_control;
extern volatile ra8p1_cpu1_campaign_proof_t g_cpu1_campaign_proof;

void cpu1_campaign_init(void);
void cpu1_campaign_service(uint32_t command_sequence,
                           uint32_t command_status,
                           uint32_t command_reason,
                           uint32_t applied_session_id,
                           bool command_pending);
void cpu1_campaign_result_visible(const ra8p1_display_frame_t *frame);
bool cpu1_campaign_owns_scheduler(void);

typedef char ra8p1_cpu1_campaign_request_size_must_be_64[
    (sizeof(ra8p1_cpu1_campaign_request_t) ==
     RA8P1_CPU1_CAMPAIGN_REQUEST_BYTES) ? 1 : -1];
typedef char ra8p1_cpu1_campaign_request_end_offset_must_be_60[
    (offsetof(ra8p1_cpu1_campaign_request_t, end_sequence) == 60U) ? 1 : -1];
typedef char ra8p1_cpu1_campaign_proof_size_must_be_128[
    (sizeof(ra8p1_cpu1_campaign_proof_t) ==
     RA8P1_CPU1_CAMPAIGN_PROOF_BYTES) ? 1 : -1];
typedef char ra8p1_cpu1_campaign_proof_terminal_offset_must_be_120[
    (offsetof(ra8p1_cpu1_campaign_proof_t, terminal_magic) == 120U) ? 1 : -1];
typedef char ra8p1_cpu1_campaign_proof_end_offset_must_be_124[
    (offsetof(ra8p1_cpu1_campaign_proof_t, end_sequence) == 124U) ? 1 : -1];

#endif
