#ifndef RF_PIPELINE_H
#define RF_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "iq_protocol.h"
#include "sdr_control_client.h"

#define RF_PIPELINE_STACK_PROOF_MAGIC       (0x52535046U) /* RSPF */
#define RF_PIPELINE_STACK_PROOF_VERSION     (1U)
#define RF_PIPELINE_STACK_PROOF_DONE_MAGIC  (0x5253444EU) /* RSDN */

typedef struct st_rf_pipeline_stack_proof
{
    uint32_t magic;
    uint32_t version;
    uint32_t completion_magic;
    uint32_t stack_bytes;
    uint32_t used_high_water_bytes;
    uint32_t free_low_water_bytes;
    uint32_t observations;
    uint32_t windows_completed;
} rf_pipeline_stack_proof_t;

extern volatile sdr_control_client_stats_t g_sdr_control_stats;
extern volatile rf_pipeline_stack_proof_t g_rf_pipeline_stack_proof;

void rf_pipeline_start(void);
bool rf_pipeline_stream_configure(const ra8p1_iq_stream_config_t *config);
bool rf_pipeline_stream_end(const ra8p1_iq_stream_config_t *config);
bool rf_pipeline_ingest(const uint8_t *data,
                        uint32_t length,
                        uint32_t sequence,
                        uint32_t flags,
                        uint32_t session_id,
                        uint64_t sample_index,
                        uint32_t format);

#endif
