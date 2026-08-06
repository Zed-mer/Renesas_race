#include "campaign_control.h"

#include "hal_data.h"
#include <string.h>

#include "display_app.h"
#include "system_protocol.h"

#define CPU1_CAMPAIGN_RETRY_DELAY_STEPS       (20U)
#define CPU1_CAMPAIGN_MAX_COMMAND_RETRIES     (20U)

#if defined(__GNUC__)
#define CPU1_CAMPAIGN_SHARED __attribute__((aligned(32), used))
#else
#define CPU1_CAMPAIGN_SHARED
#endif

volatile ra8p1_cpu1_campaign_request_t g_cpu1_campaign_control
    CPU1_CAMPAIGN_SHARED =
{
    .begin_sequence = 2U,
    .magic = RA8P1_CPU1_CAMPAIGN_REQUEST_MAGIC,
    .version = RA8P1_CPU1_CAMPAIGN_VERSION,
    .size = RA8P1_CPU1_CAMPAIGN_REQUEST_BYTES,
    .target_payload_mbps_x1000 =
        RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000,
    .end_sequence = 2U
};

volatile ra8p1_cpu1_campaign_proof_t g_cpu1_campaign_proof
    CPU1_CAMPAIGN_SHARED =
{
    .begin_sequence = 2U,
    .magic = RA8P1_CPU1_CAMPAIGN_PROOF_MAGIC,
    .version = RA8P1_CPU1_CAMPAIGN_VERSION,
    .size = RA8P1_CPU1_CAMPAIGN_PROOF_BYTES,
    .state = RA8P1_CPU1_CAMPAIGN_STATE_UNINITIALIZED,
    .end_sequence = 2U
};

typedef struct st_cpu1_campaign_context
{
    ra8p1_cpu1_campaign_request_t request;
    ra8p1_cpu1_campaign_request_t last_observed_request;
    uint32_t request_begin_sequence;
    uint32_t last_observed_begin_sequence;
    uint32_t state;
    uint32_t terminal_state_after_stop;
    uint32_t windows_expected;
    uint32_t windows_visible;
    uint32_t iterations_completed;
    uint32_t expected_center_index;
    uint32_t active_center_index;
    uint32_t current_command_sequence;
    uint32_t last_command_sequence;
    uint32_t last_command_status;
    uint32_t last_command_reason;
    uint32_t last_applied_session_id;
    uint32_t last_session_id;
    uint32_t last_window_sequence;
    uint32_t last_result_center_index;
    uint32_t baseline_session_id;
    uint32_t command_send_retries;
    uint32_t busy_retries;
    uint32_t rejected_requests;
    uint32_t duplicate_requests;
    uint32_t unexpected_results;
    uint32_t last_error;
    uint32_t retry_delay_steps;
    bool owns_scheduler;
    bool command_issued;
    bool result_pending;
    bool awaiting_new_session;
} cpu1_campaign_context_t;

static cpu1_campaign_context_t g_campaign;

static void campaign_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static void campaign_request_invalidate(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr(
        (volatile void *)&g_cpu1_campaign_control,
        (int32_t)sizeof(g_cpu1_campaign_control));
#endif
    campaign_barrier();
}

static void campaign_proof_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)&g_cpu1_campaign_proof,
                            (int32_t)sizeof(g_cpu1_campaign_proof));
#endif
    __DSB();
}

static bool campaign_request_read(ra8p1_cpu1_campaign_request_t *request,
                                  uint32_t *sequence)
{
    uint32_t begin;
    uint32_t end;
    uint32_t final_begin;
    if ((request == NULL) || (sequence == NULL))
    {
        return false;
    }
    campaign_request_invalidate();
    begin = g_cpu1_campaign_control.begin_sequence;
    if ((begin == 0U) || ((begin & 1U) != 0U))
    {
        return false;
    }
    memcpy(request, (const void *)&g_cpu1_campaign_control, sizeof(*request));
    campaign_barrier();
    end = g_cpu1_campaign_control.end_sequence;
    final_begin = g_cpu1_campaign_control.begin_sequence;
    if ((begin != end) || (begin != final_begin) ||
        (request->begin_sequence != begin) ||
        (request->end_sequence != end))
    {
        return false;
    }
    *sequence = begin;
    return true;
}

static bool campaign_reserved_zero(
    const ra8p1_cpu1_campaign_request_t *request)
{
    for (uint32_t index = 0U;
         index < (sizeof(request->reserved) / sizeof(request->reserved[0]));
         ++index)
    {
        if (request->reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

static bool campaign_request_payload_equal(
    const ra8p1_cpu1_campaign_request_t *left,
    const ra8p1_cpu1_campaign_request_t *right)
{
    const size_t payload_offset =
        offsetof(ra8p1_cpu1_campaign_request_t, magic);
    const size_t payload_bytes =
        offsetof(ra8p1_cpu1_campaign_request_t, end_sequence) -
        payload_offset;
    return memcmp(((const uint8_t *)left) + payload_offset,
                  ((const uint8_t *)right) + payload_offset,
                  payload_bytes) == 0;
}

static uint32_t campaign_request_error(
    const ra8p1_cpu1_campaign_request_t *request,
    uint32_t *windows_expected)
{
    uint64_t total;
    if ((request->magic != RA8P1_CPU1_CAMPAIGN_REQUEST_MAGIC) ||
        (request->version != RA8P1_CPU1_CAMPAIGN_VERSION) ||
        (request->size != sizeof(*request)))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_HEADER;
    }
    if ((request->mode < RA8P1_CPU1_CAMPAIGN_MODE_STOP) ||
        (request->mode > RA8P1_CPU1_CAMPAIGN_MODE_FOUR_SERIAL))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_MODE;
    }
    if ((request->target_payload_mbps_x1000 <
         RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000) ||
        (request->target_payload_mbps_x1000 >
         RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000) ||
        ((request->target_payload_mbps_x1000 % 1000U) != 0U))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_RATE;
    }
    if ((request->test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) != 0U)
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_FAULT_FLAGS;
    }
    if ((request->flags != 0U) || !campaign_reserved_zero(request))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_FLAGS;
    }
    if (request->mode == RA8P1_CPU1_CAMPAIGN_MODE_STOP)
    {
        *windows_expected = 0U;
        return RA8P1_CPU1_CAMPAIGN_ERROR_NONE;
    }
    if ((request->iterations == 0U) ||
        (request->iterations > RA8P1_CPU1_CAMPAIGN_MAX_ITERATIONS))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_ITERATIONS;
    }
    if ((request->mode == RA8P1_CPU1_CAMPAIGN_MODE_SINGLE) &&
        (request->center_index >= RA8P1_CENTER_COUNT))
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_CENTER;
    }
    total = request->iterations;
    if (request->mode != RA8P1_CPU1_CAMPAIGN_MODE_SINGLE)
    {
        total *= RA8P1_CENTER_COUNT;
    }
    if (total > UINT32_MAX)
    {
        return RA8P1_CPU1_CAMPAIGN_ERROR_INVALID_ITERATIONS;
    }
    *windows_expected = (uint32_t)total;
    return RA8P1_CPU1_CAMPAIGN_ERROR_NONE;
}

static uint32_t campaign_proof_next_sequence(void)
{
    uint32_t sequence = (g_cpu1_campaign_proof.end_sequence + 2U) & ~1U;
    return (sequence == 0U) ? 2U : sequence;
}

static void campaign_publish(void)
{
    volatile ra8p1_cpu1_campaign_proof_t *proof = &g_cpu1_campaign_proof;
    const uint32_t sequence = campaign_proof_next_sequence();
    proof->begin_sequence = sequence | 1U;
    campaign_barrier();
    proof->magic = RA8P1_CPU1_CAMPAIGN_PROOF_MAGIC;
    proof->version = RA8P1_CPU1_CAMPAIGN_VERSION;
    proof->size = (uint16_t)sizeof(*proof);
    proof->request_id = g_campaign.request.request_id;
    proof->request_begin_sequence = g_campaign.request_begin_sequence;
    proof->state = g_campaign.state;
    proof->mode = g_campaign.request.mode;
    proof->configured_center_index = g_campaign.request.center_index;
    proof->iterations_requested = g_campaign.request.iterations;
    proof->iterations_completed = g_campaign.iterations_completed;
    proof->windows_expected = g_campaign.windows_expected;
    proof->windows_visible = g_campaign.windows_visible;
    proof->next_center_index = g_campaign.expected_center_index;
    proof->active_center_index = g_campaign.active_center_index;
    proof->target_payload_mbps_x1000 =
        g_campaign.request.target_payload_mbps_x1000;
    proof->test_fault_flags = g_campaign.request.test_fault_flags;
    proof->campaign_flags = g_campaign.request.flags;
    proof->last_session_id = g_campaign.last_session_id;
    proof->last_window_sequence = g_campaign.last_window_sequence;
    proof->last_result_center_index = g_campaign.last_result_center_index;
    proof->last_command_sequence = g_campaign.last_command_sequence;
    proof->last_command_status = g_campaign.last_command_status;
    proof->last_command_reason = g_campaign.last_command_reason;
    proof->last_applied_session_id = g_campaign.last_applied_session_id;
    proof->command_send_retries = g_campaign.command_send_retries;
    proof->busy_retries = g_campaign.busy_retries;
    proof->rejected_requests = g_campaign.rejected_requests;
    proof->duplicate_requests = g_campaign.duplicate_requests;
    proof->unexpected_results = g_campaign.unexpected_results;
    proof->last_error = g_campaign.last_error;
    proof->terminal_magic = 0U;
    if ((g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_COMPLETE) ||
        (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPED))
    {
        proof->terminal_magic = RA8P1_CPU1_CAMPAIGN_COMPLETE_MAGIC;
    }
    else if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_ERROR)
    {
        proof->terminal_magic = RA8P1_CPU1_CAMPAIGN_FAILURE_MAGIC;
    }
    campaign_barrier();
    proof->end_sequence = sequence;
    proof->begin_sequence = sequence;
    campaign_barrier();
    campaign_proof_clean();
}

static void campaign_reset_progress(void)
{
    g_campaign.windows_visible = 0U;
    g_campaign.iterations_completed = 0U;
    g_campaign.expected_center_index =
        (g_campaign.request.mode == RA8P1_CPU1_CAMPAIGN_MODE_SINGLE) ?
        g_campaign.request.center_index : 0U;
    g_campaign.active_center_index = UINT32_MAX;
    g_campaign.current_command_sequence = 0U;
    g_campaign.last_session_id = 0U;
    g_campaign.last_window_sequence = 0U;
    g_campaign.last_result_center_index = UINT32_MAX;
    g_campaign.baseline_session_id = 0U;
    g_campaign.command_send_retries = 0U;
    g_campaign.busy_retries = 0U;
    g_campaign.unexpected_results = 0U;
    g_campaign.last_error = RA8P1_CPU1_CAMPAIGN_ERROR_NONE;
    g_campaign.retry_delay_steps = 0U;
    g_campaign.command_issued = false;
    g_campaign.result_pending = false;
    g_campaign.awaiting_new_session = false;
}

static void campaign_begin_stop(uint32_t terminal_state)
{
    g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_STOPPING;
    g_campaign.terminal_state_after_stop = terminal_state;
    g_campaign.current_command_sequence = 0U;
    g_campaign.command_issued = false;
    g_campaign.result_pending = false;
}

static void campaign_fail(uint32_t error)
{
    g_campaign.last_error = error;
    campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_ERROR);
}

static void campaign_accept_request(
    const ra8p1_cpu1_campaign_request_t *request,
    uint32_t request_sequence,
    uint32_t windows_expected)
{
    g_campaign.request = *request;
    g_campaign.request_begin_sequence = request_sequence;
    g_campaign.windows_expected = windows_expected;
    g_campaign.owns_scheduler = true;
    campaign_reset_progress();
    display_app_campaign_takeover();
    campaign_begin_stop(
        (request->mode == RA8P1_CPU1_CAMPAIGN_MODE_STOP) ?
        RA8P1_CPU1_CAMPAIGN_STATE_STOPPED :
        RA8P1_CPU1_CAMPAIGN_STATE_ARMING);
}

static bool campaign_owns_active_scheduler(void)
{
    if (!g_campaign.owns_scheduler)
    {
        return false;
    }
    return (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPING) ||
           (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_ARMING) ||
           (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_RUNNING) ||
           (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT);
}

static void campaign_observe_request(void)
{
    ra8p1_cpu1_campaign_request_t request;
    uint32_t request_sequence;
    uint32_t windows_expected = 0U;
    uint32_t error;
    if (!campaign_request_read(&request, &request_sequence) ||
        (request.request_id == 0U) ||
        (request.mode == RA8P1_CPU1_CAMPAIGN_MODE_NONE))
    {
        return;
    }
    if (request.request_id == g_campaign.last_observed_request.request_id)
    {
        if (request_sequence == g_campaign.last_observed_begin_sequence)
        {
            return;
        }
        if (campaign_request_payload_equal(
                &request, &g_campaign.last_observed_request))
        {
            g_campaign.duplicate_requests++;
            g_campaign.last_observed_begin_sequence = request_sequence;
            campaign_publish();
            return;
        }
        g_campaign.rejected_requests++;
        if (!campaign_owns_active_scheduler())
        {
            g_campaign.last_error =
                RA8P1_CPU1_CAMPAIGN_ERROR_REQUEST_ID_REUSED;
        }
        g_campaign.last_observed_begin_sequence = request_sequence;
        campaign_publish();
        return;
    }
    error = campaign_request_error(&request, &windows_expected);
    g_campaign.last_observed_request = request;
    g_campaign.last_observed_begin_sequence = request_sequence;
    /* A J-Link/host campaign write is a separate control plane from the
     * normal CPU1->CPU0 command path.  It must not replace an in-flight
     * scan because doing so can strand a CPU0 prefetch session and make one
     * proof object describe a mixture of two host requests.  STOP remains
     * the only valid way to take ownership while a campaign is active. */
    if (campaign_owns_active_scheduler() &&
        ((error != RA8P1_CPU1_CAMPAIGN_ERROR_NONE) ||
         (request.mode != RA8P1_CPU1_CAMPAIGN_MODE_STOP)))
    {
        g_campaign.rejected_requests++;
        campaign_publish();
        return;
    }
    if (error != RA8P1_CPU1_CAMPAIGN_ERROR_NONE)
    {
        g_campaign.rejected_requests++;
        g_campaign.request = request;
        g_campaign.request_begin_sequence = request_sequence;
        g_campaign.windows_expected = 0U;
        g_campaign.owns_scheduler = true;
        campaign_reset_progress();
        g_campaign.last_error = error;
        display_app_campaign_takeover();
        campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_ERROR);
        campaign_publish();
        return;
    }
    campaign_accept_request(&request, request_sequence, windows_expected);
    campaign_publish();
}

static bool campaign_transient_rejection(uint32_t reason)
{
    return (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_BUSY) ||
           (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED) ||
           (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_TIMEOUT);
}

static void campaign_schedule_retry(void)
{
    g_campaign.command_send_retries++;
    g_campaign.busy_retries++;
    g_campaign.retry_delay_steps = CPU1_CAMPAIGN_RETRY_DELAY_STEPS;
    campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT);
}

static void campaign_issue_stop(bool command_pending)
{
    if (g_campaign.command_issued || command_pending)
    {
        return;
    }
    if (!display_app_campaign_command_stop())
    {
        g_campaign.command_send_retries++;
        if (g_campaign.command_send_retries >=
            CPU1_CAMPAIGN_MAX_COMMAND_RETRIES)
        {
            g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_ERROR;
            g_campaign.last_error = RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_SEND;
        }
        campaign_publish();
        return;
    }
    g_campaign.current_command_sequence =
        display_app_last_issued_command_sequence();
    g_campaign.command_issued = true;
    campaign_publish();
}

static void campaign_issue_capture(bool command_pending)
{
    bool scan_all;
    if (command_pending)
    {
        return;
    }
    scan_all =
        g_campaign.request.mode == RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP;
    g_campaign.active_center_index = g_campaign.expected_center_index;
    g_campaign.baseline_session_id =
        display_app_last_visible_session_id();
    g_campaign.awaiting_new_session =
        g_campaign.baseline_session_id != 0U;
    if (!display_app_campaign_command_start(
            g_campaign.active_center_index,
            scan_all,
            scan_all,
            g_campaign.request.target_payload_mbps_x1000,
            g_campaign.request.test_fault_flags))
    {
        g_campaign.command_send_retries++;
        if (g_campaign.command_send_retries >=
            CPU1_CAMPAIGN_MAX_COMMAND_RETRIES)
        {
            campaign_fail(RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_SEND);
        }
        campaign_publish();
        return;
    }
    g_campaign.current_command_sequence =
        display_app_last_issued_command_sequence();
    g_campaign.command_issued = true;
    g_campaign.result_pending = true;
    g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_RUNNING;
    campaign_publish();
}

void cpu1_campaign_init(void)
{
    memset(&g_campaign, 0, sizeof(g_campaign));
    g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_READY;
    g_campaign.active_center_index = UINT32_MAX;
    g_campaign.expected_center_index = UINT32_MAX;
    g_campaign.last_result_center_index = UINT32_MAX;
    g_campaign.request.target_payload_mbps_x1000 =
        RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000;
    campaign_proof_clean();
    campaign_publish();
}

void cpu1_campaign_service(uint32_t command_sequence,
                           uint32_t command_status,
                           uint32_t command_reason,
                           uint32_t applied_session_id,
                           bool command_pending)
{
    bool telemetry_changed = false;
    campaign_observe_request();
    if ((g_campaign.last_command_sequence != command_sequence) ||
        (g_campaign.last_command_status != command_status) ||
        (g_campaign.last_command_reason != command_reason) ||
        (g_campaign.last_applied_session_id != applied_session_id))
    {
        g_campaign.last_command_sequence = command_sequence;
        g_campaign.last_command_status = command_status;
        g_campaign.last_command_reason = command_reason;
        g_campaign.last_applied_session_id = applied_session_id;
        telemetry_changed = true;
    }
    if (!g_campaign.owns_scheduler)
    {
        if (telemetry_changed)
        {
            campaign_publish();
        }
        return;
    }
    if (g_campaign.command_issued &&
        (command_sequence == g_campaign.current_command_sequence) &&
        (command_status == RA8P1_COMMAND_REJECTED))
    {
        if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPING)
        {
            if (campaign_transient_rejection(command_reason) &&
                (g_campaign.command_send_retries <
                 CPU1_CAMPAIGN_MAX_COMMAND_RETRIES))
            {
                g_campaign.command_send_retries++;
                g_campaign.busy_retries++;
                g_campaign.command_issued = false;
                g_campaign.current_command_sequence = 0U;
            }
            else
            {
                g_campaign.command_issued = false;
                g_campaign.current_command_sequence = 0U;
                g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_ERROR;
                g_campaign.last_error =
                    RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_REJECTED;
            }
        }
        else if (campaign_transient_rejection(command_reason) &&
                 (g_campaign.command_send_retries <
                  CPU1_CAMPAIGN_MAX_COMMAND_RETRIES))
        {
            campaign_schedule_retry();
        }
        else
        {
            campaign_fail(RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_REJECTED);
        }
        campaign_publish();
        return;
    }
    if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPING)
    {
        if (g_campaign.command_issued &&
            (command_sequence == g_campaign.current_command_sequence) &&
            (command_status == RA8P1_COMMAND_APPLIED) &&
            (command_reason == RA8P1_COMMAND_REASON_STOPPED))
        {
            g_campaign.command_issued = false;
            g_campaign.current_command_sequence = 0U;
            g_campaign.state = g_campaign.terminal_state_after_stop;
            if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_ARMING)
            {
                g_campaign.last_error = RA8P1_CPU1_CAMPAIGN_ERROR_NONE;
            }
            campaign_publish();
            return;
        }
        campaign_issue_stop(command_pending);
        return;
    }
    if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT)
    {
        if (g_campaign.retry_delay_steps != 0U)
        {
            g_campaign.retry_delay_steps--;
            return;
        }
        g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_ARMING;
        campaign_publish();
    }
    if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_ARMING)
    {
        campaign_issue_capture(command_pending);
        return;
    }
    if (telemetry_changed)
    {
        campaign_publish();
    }
}

void cpu1_campaign_result_visible(const ra8p1_display_frame_t *frame)
{
    uint32_t center_index;
    if ((frame == NULL) ||
        (g_campaign.state != RA8P1_CPU1_CAMPAIGN_STATE_RUNNING) ||
        !g_campaign.result_pending)
    {
        return;
    }
    if ((frame->session_id == g_campaign.last_session_id) &&
        (frame->analysis.window_sequence == g_campaign.last_window_sequence))
    {
        return;
    }
    if (g_campaign.awaiting_new_session)
    {
        if (frame->session_id == g_campaign.baseline_session_id)
        {
            return;
        }
        g_campaign.awaiting_new_session = false;
    }
    center_index = frame->analysis.center_index;
    if (center_index != g_campaign.expected_center_index)
    {
        g_campaign.unexpected_results++;
        campaign_fail(RA8P1_CPU1_CAMPAIGN_ERROR_RESULT_ORDER);
        campaign_publish();
        return;
    }
    g_campaign.windows_visible++;
    g_campaign.last_session_id = frame->session_id;
    g_campaign.last_window_sequence = frame->analysis.window_sequence;
    g_campaign.last_result_center_index = center_index;
    if (g_campaign.request.mode == RA8P1_CPU1_CAMPAIGN_MODE_SINGLE)
    {
        g_campaign.iterations_completed = g_campaign.windows_visible;
    }
    else if (center_index == (RA8P1_CENTER_COUNT - 1U))
    {
        g_campaign.iterations_completed++;
    }
    if (g_campaign.windows_visible >= g_campaign.windows_expected)
    {
        /* Replace the retained START with STOP before publishing a terminal
         * result.  Otherwise an IPC boot-epoch replay can relaunch a campaign
         * that the host already observed as complete. */
        campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_COMPLETE);
        campaign_publish();
        return;
    }
    if (g_campaign.request.mode ==
        RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP)
    {
        /* One CPU0-owned continuous scan spans every requested round, so the
         * center 3 -> 0 boundary uses the same prefetch/ACK/CREDIT pipeline as
         * the three within-round boundaries. CPU1 only advances proof state. */
        g_campaign.expected_center_index = (center_index + 1U) %
                                           RA8P1_CENTER_COUNT;
        g_campaign.active_center_index = g_campaign.expected_center_index;
        campaign_publish();
        return;
    }
    if (g_campaign.request.mode == RA8P1_CPU1_CAMPAIGN_MODE_SINGLE)
    {
        g_campaign.expected_center_index = g_campaign.request.center_index;
    }
    else
    {
        g_campaign.expected_center_index = (center_index + 1U) %
                                           RA8P1_CENTER_COUNT;
    }
    g_campaign.command_issued = false;
    g_campaign.result_pending = false;
    g_campaign.current_command_sequence = 0U;
    g_campaign.retry_delay_steps = CPU1_CAMPAIGN_RETRY_DELAY_STEPS;
    g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT;
    campaign_publish();
}

bool cpu1_campaign_owns_scheduler(void)
{
    return g_campaign.owns_scheduler;
}
