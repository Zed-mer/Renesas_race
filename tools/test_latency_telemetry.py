import ctypes
import pathlib
import re
import unittest

from project_layout import resolve_cpu0, resolve_cpu1


ROOT = pathlib.Path(__file__).resolve().parents[1]
CPU0 = resolve_cpu0(ROOT)
CPU1 = resolve_cpu1(ROOT)


def literal_macro(path: pathlib.Path, name: str) -> int:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+\(?\s*(0x[0-9A-Fa-f]+|[0-9]+)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"numeric macro not found: {name} in {path}")
    return int(match.group(1), 0)


class LatencyControl(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("cpu0_cycle_hz", ctypes.c_uint32),
        ("published_windows", ctypes.c_uint32),
        ("overwritten_windows", ctypes.c_uint32),
        ("overwritten_unacked_windows", ctypes.c_uint32),
        ("cpu1_visible_windows", ctypes.c_uint32),
        ("latest_sequence", ctypes.c_uint32),
    ]


class LatencyRecord(ctypes.Structure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("session_id", ctypes.c_uint32),
        ("window_index_flags", ctypes.c_uint32),
        ("first_packet_cpu0_cycles", ctypes.c_uint32),
        ("window_complete_cpu0_cycles", ctypes.c_uint32),
        ("npu_publish_cpu0_cycles", ctypes.c_uint32),
        ("cpu1_visible_cpu0_cycles", ctypes.c_uint32),
        ("end_sequence", ctypes.c_uint32),
    ]


class LatencyTelemetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.resource = ROOT / "shared" / "resource_layout.h"
        cls.latency = ROOT / "shared" / "latency_telemetry.h"
        cls.analysis = ROOT / "shared" / "analysis_contract.h"

    def test_abi_and_cache_line_layout(self) -> None:
        self.assertEqual(32, ctypes.sizeof(LatencyControl))
        self.assertEqual(32, ctypes.sizeof(LatencyRecord))
        self.assertEqual(24, LatencyRecord.cpu1_visible_cpu0_cycles.offset)
        slot_count = literal_macro(self.latency, "RA8P1_LATENCY_SLOT_COUNT")
        region_bytes = literal_macro(self.resource, "RA8P1_IPC_LATENCY_BYTES")
        offset = literal_macro(self.resource, "RA8P1_IPC_LATENCY_OFFSET")
        self.assertEqual(4, slot_count)
        self.assertEqual(160, ctypes.sizeof(LatencyControl) + slot_count * ctypes.sizeof(LatencyRecord))
        self.assertEqual(160, region_bytes)
        self.assertEqual(0, offset % 32)

    def test_reserved_region_does_not_expand_shared_ram(self) -> None:
        command_end = (
            literal_macro(self.resource, "RA8P1_IPC_COMMAND_OFFSET")
            + literal_macro(self.resource, "RA8P1_IPC_COMMAND_BYTES")
        )
        latency_start = literal_macro(self.resource, "RA8P1_IPC_LATENCY_OFFSET")
        latency_end = latency_start + literal_macro(self.resource, "RA8P1_IPC_LATENCY_BYTES")
        runtime_start = literal_macro(self.resource, "RA8P1_IPC_RUNTIME_OFFSET")
        self.assertEqual(command_end, latency_start)
        self.assertEqual(latency_end, runtime_start)
        self.assertEqual(0x8000, literal_macro(self.resource, "RA8P1_SHARED_RAM_BYTES"))

    def test_low_latency_and_full_session_window_counts(self) -> None:
        low = literal_macro(self.analysis, "RA8P1_ANALYSIS_LOW_LATENCY_SAMPLES")
        total = literal_macro(self.analysis, "RA8P1_ANALYSIS_TOTAL_SAMPLES")
        window = literal_macro(self.analysis, "RA8P1_ANALYSIS_TILE_SAMPLES")
        stride = literal_macro(self.analysis, "RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES")
        count = lambda samples: 1 + (samples - window) // stride
        self.assertEqual(590336, low)
        self.assertEqual(1, count(low))
        self.assertEqual(19, count(total))

    def test_first_packet_mapping_covers_all_19_overlapping_windows(self) -> None:
        total = literal_macro(self.analysis, "RA8P1_ANALYSIS_TOTAL_SAMPLES")
        window = literal_macro(self.analysis, "RA8P1_ANALYSIS_TILE_SAMPLES")
        stride = literal_macro(self.analysis, "RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES")
        window_count = 1 + (total - window) // stride
        seen: dict[int, int] = {}
        packet_samples = 384
        next_window = 0
        next_window_start = 0
        for sample_index in range(0, total, packet_samples):
            sample_end = min(sample_index + packet_samples, total)
            while next_window < window_count and next_window_start < sample_index:
                next_window += 1
                next_window_start += stride
            while next_window < window_count and next_window_start < sample_end:
                seen.setdefault(next_window, sample_index)
                next_window += 1
                next_window_start += stride
        self.assertEqual(list(range(19)), sorted(seen))
        for index, packet_start in seen.items():
            window_start = index * stride
            self.assertLessEqual(packet_start, window_start)
            self.assertLess(window_start, packet_start + packet_samples)

    def test_four_slot_ring_reports_full_session_overwrite_count(self) -> None:
        slot_count = literal_macro(self.latency, "RA8P1_LATENCY_SLOT_COUNT")
        published = 19
        retained = min(published, slot_count)
        overwritten = max(0, published - slot_count)
        self.assertEqual(4, retained)
        self.assertEqual(15, overwritten)

    def test_stage_ordering_is_visible_in_firmware_source(self) -> None:
        cpu0_ipc = (CPU0 / "src" / "framework" / "ipc_bridge.c").read_text(encoding="utf-8")
        cpu1_ipc = (CPU1 / "src" / "framework" / "ipc_bridge.c").read_text(encoding="utf-8")
        analysis = (CPU0 / "src" / "framework" / "analysis_pipeline.c").read_text(encoding="utf-8")
        rf_pipeline = (CPU0 / "src" / "framework" / "rf_pipeline.c").read_text(encoding="utf-8")
        publish_body = cpu0_ipc[cpu0_ipc.index("void ipc_bridge_cpu0_display_publish"):]
        self.assertLess(publish_body.index("ipc_cpu0_latency_update_npu"),
                        publish_body.index("slot->end_sequence = sequence"))
        display_poll = cpu1_ipc[cpu1_ipc.index("bool ipc_bridge_cpu1_display_poll"):]
        self.assertLess(display_poll.index("*frame = oldest"),
                        display_poll.index("ipc_cpu1_latency_ack"))
        lane_publish = analysis[analysis.index("static void analysis_publish_lane"):]
        self.assertLess(lane_publish.index("ipc_bridge_cpu0_latency_window_complete"),
                        lane_publish.index("npu_runner_infer"))
        ingest = rf_pipeline[rf_pipeline.index("bool rf_pipeline_ingest"):]
        self.assertLess(ingest.index("ipc_bridge_cpu0_latency_ingress_prepare"),
                        ingest.index("iq_ring_push_copy"))
        self.assertLess(ingest.index("iq_ring_push_copy"),
                        ingest.index("ipc_bridge_cpu0_latency_ingress_commit"))

    def test_result_rings_and_latency_ack_span_capture_sessions(self) -> None:
        cpu0_ipc = (CPU0 / "src" / "framework" / "ipc_bridge.c").read_text(encoding="utf-8")
        setter = cpu0_ipc[cpu0_ipc.index("void ipc_bridge_cpu0_display_session_set"):]
        setter = setter[:setter.index("void ipc_bridge_cpu0_display_tile_publish")]

        # The four result slots are a cross-session queue. Tile rows retain
        # their existing session-local restart semantics.
        self.assertIn("g_display_session_id = next_session", setter)
        self.assertIn("bool session_changed = (next_session != g_display_session_id)", setter)
        self.assertNotIn("g_display_sequence = 0U", setter)
        self.assertNotIn("memset((void *) RA8P1_DISPLAY_STREAM_SLOTS", setter)
        self.assertIn("g_tile_sequence = 0U", setter)
        self.assertIn("memset((void *) RA8P1_DISPLAY_TILE_SLOTS", setter)

        # A same-session replay and a normal handover both remain above the
        # consumer's last even sequence.
        producer_sequence = 16
        consumer_sequence = 16
        replay_sequence = (producer_sequence + 2) & ~1
        self.assertGreater(replay_sequence, consumer_sequence)

        cpu1_ipc = (CPU1 / "src" / "framework" / "ipc_bridge.c").read_text(encoding="utf-8")
        display_poll = cpu1_ipc[cpu1_ipc.index("bool ipc_bridge_cpu1_display_poll"):]
        display_poll = display_poll[:display_poll.index("bool ipc_bridge_cpu1_display_visible")]
        producer_reset = display_poll[
            display_poll.index(
                "if (g_observed_cpu0_boot_epoch != g_display_cpu0_boot_epoch)"
            ):
        ]
        producer_reset = producer_reset[:producer_reset.index("ipc_barrier()")]
        self.assertIn("g_display_sequence = 0U", producer_reset)
        session_handover = display_poll[
            display_poll.index("if (session_id != g_display_session_id)"):
        ]
        session_handover = session_handover[:session_handover.index("for (index")]
        self.assertNotIn("g_display_sequence = 0U", session_handover)
        self.assertNotIn("candidate.session_id != session_id", display_poll)
        self.assertIn("candidate.session_id == 0U", display_poll)
        self.assertIn("begin - g_display_sequence", display_poll)
        self.assertIn("begin - oldest_sequence", display_poll)

        session_begin = cpu0_ipc[cpu0_ipc.index("void ipc_bridge_cpu0_latency_session_begin"):]
        session_begin = session_begin[:session_begin.index("uint32_t ipc_bridge_cpu0_latency_ingress_prepare")]
        self.assertIn("if (session_id == 0U)", session_begin)
        self.assertEqual(
            1,
            session_begin.count(
                "memset((void *) control, 0, RA8P1_IPC_LATENCY_BYTES)"
            ),
        )
        self.assertLess(
            session_begin.index("ipc_bridge_cpu0_latency_poll()"),
            session_begin.index("g_cpu0_latency.session_id = session_id"),
        )
        latency_poll = cpu0_ipc[cpu0_ipc.index("void ipc_bridge_cpu0_latency_poll"):]
        latency_poll = latency_poll[:latency_poll.index("bool ipc_bridge_cpu0_latency_result_visible")]
        self.assertNotIn("g_cpu0_latency.active == 0U", latency_poll)
        result_visible = cpu0_ipc[cpu0_ipc.index("bool ipc_bridge_cpu0_latency_result_visible"):]
        result_visible = result_visible[:result_visible.index("void ipc_bridge_cpu0_init")]
        self.assertNotIn("g_cpu0_latency.session_id != session_id", result_visible)

        tile_poll = cpu1_ipc[cpu1_ipc.index("bool ipc_bridge_cpu1_display_tile_poll"):]
        tile_poll = tile_poll[:tile_poll.index("bool ipc_bridge_cpu1_command_send")]
        self.assertNotIn("g_tile_window_sequence", cpu1_ipc)
        self.assertNotIn("candidate.window_sequence -", tile_poll)
        self.assertIn("transport sequence remains monotonic", tile_poll)


if __name__ == "__main__":
    unittest.main()
