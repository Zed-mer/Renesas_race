#!/usr/bin/env python3
"""Generate an offline RFC 5322 progress-email draft.

The generator is intentionally an outbox preparer.  It reads an existing
performance summary and writes an ``.eml`` file; it never opens a socket,
contacts an SMTP server, or sends a message.  Structured JSON summaries are
rendered with the source status of every metric (MEASURED, ESTIMATED, or
MISSING). Markdown input is preserved verbatim and is accepted only when it
contains explicit status markers/words.
"""

from __future__ import annotations

import argparse
import datetime as dt
import email.policy
import json
import math
import re
import sys
from email.message import EmailMessage
from email.utils import format_datetime, parseaddr
from pathlib import Path
from typing import Any


STATUSES = {"measured", "estimated", "missing"}
STATUS_RE = re.compile(r"\[(MEASURED|ESTIMATED|MISSING)\]", re.IGNORECASE)
STATUS_WORD_RE = re.compile(r"\b(MEASURED|ESTIMATED|MISSING)\b", re.IGNORECASE)
ADDRESS_RE = re.compile(r"^[^\s@<>]+@[^\s@<>]+$")


class InputError(ValueError):
    """Raised when an input summary is unsafe or not sufficiently explicit."""


def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise InputError(f"cannot decode {path} as UTF-8/UTF-16") from exc


def validate_header_value(value: str, name: str) -> str:
    value = value.strip()
    if not value or "\r" in value or "\n" in value:
        raise InputError(f"{name} must be non-empty and contain no CR/LF")
    return value


def validate_address(value: str, name: str) -> str:
    value = validate_header_value(value, name)
    _display, address = parseaddr(value)
    if not ADDRESS_RE.fullmatch(address):
        raise InputError(f"{name} is not a valid address: {value!r}")
    return value


def status_metric(metric: Any, label: str, unit: str = "ms") -> str:
    if not isinstance(metric, dict):
        raise InputError(f"{label} is not a metric object")
    status = str(metric.get("status", "")).strip().lower()
    if status not in STATUSES:
        raise InputError(f"{label} has invalid status {status!r}")
    value = metric.get("value_ms" if unit == "ms" else "value_hz")
    if status == "missing":
        if value is not None:
            raise InputError(f"{label} is MISSING but has a value")
        value_text = "MISSING"
    else:
        if value is None:
            raise InputError(f"{label} is {status.upper()} but has no value")
        if isinstance(value, bool):
            raise InputError(f"{label} value is boolean")
        try:
            numeric = float(value)
        except (TypeError, ValueError) as exc:
            raise InputError(f"{label} value is not numeric") from exc
        if not math.isfinite(numeric) or numeric < 0:
            raise InputError(f"{label} value is not a finite non-negative number")
        value_text = f"{numeric:g}"
    suffix = f" {unit}" if value is not None else ""
    return f"{value_text}{suffix} [{status.upper()}]"


def _source_text(metric: dict[str, Any]) -> str:
    basis = str(metric.get("basis", "")).strip() or "unspecified basis"
    sources = metric.get("source", [])
    if isinstance(sources, list) and sources:
        source_text = "; ".join(str(item) for item in sources)
        return f"{basis}; source={source_text}"
    return basis


def render_json_summary(summary: dict[str, Any]) -> str:
    sessions = summary.get("single_frequency")
    if not isinstance(sessions, list):
        raise InputError("summary JSON has no single_frequency array")
    contract = summary.get("contract")
    if not isinstance(contract, dict):
        raise InputError("summary JSON has no contract object")
    lines: list[str] = [
        "RA8P1 SDR inference progress summary",
        "",
        "This is an offline progress draft. The generator did not contact a",
        "mail server and did not infer missing timings.",
        "",
        f"Generated UTC: {summary.get('generated_utc', 'unspecified')}",
        f"Summary tool: {summary.get('tool', 'unspecified')} {summary.get('tool_version', '')}".rstrip(),
        "",
        "Contract",
        f"- Sample rate: {contract.get('sample_rate_hz', 'unspecified')} Hz",
        f"- Window samples: {contract.get('window_samples', 'unspecified')}",
        f"- RF window span: {contract.get('rf_window_span_ms', 'unspecified')} ms (contract value)",
        "",
        "Single-frequency sessions",
    ]
    if not sessions:
        lines.append("- No sessions present [MISSING]")
    for index, session in enumerate(sessions, 1):
        if not isinstance(session, dict):
            raise InputError(f"single_frequency[{index - 1}] is not an object")
        session_id = session.get("session_id", "unspecified")
        center = session.get("center_hz", "unspecified")
        lines.extend(["", f"Session {session_id}; center={center} Hz"])
        stages = session.get("stages")
        if not isinstance(stages, dict):
            raise InputError(f"session {session_id} has no stages object")
        for key, label in (
            ("capture", "SDR capture"),
            ("tune", "Retune"),
            ("send", "IQ transfer"),
            ("stft", "STFT"),
            ("npu", "NPU"),
            ("e2e", "End-to-end to NPU"),
            ("cpu1_visible", "CPU1 visible upper"),
        ):
            lines.append(f"- {label}: {status_metric(stages.get(key), f'session {session_id} {key}')}")
            lines.append(f"  basis: {_source_text(stages[key])}")
        lines.append(
            f"- Inference/frame rate: {status_metric(session.get('steady_state_fps'), f'session {session_id} FPS', 'Hz')}"
        )
        lines.append(f"  basis: {_source_text(session['steady_state_fps'])}")

    total = summary.get("four_frequency_total")
    if not isinstance(total, dict):
        raise InputError("summary JSON has no four_frequency_total object")
    lines.extend(["", "Four-frequency coverage"])
    lines.append(f"- Total cycle: {status_metric(total, 'four-frequency total')}")
    lines.append(f"- Basis: {_source_text(total)}")
    formula = total.get("formula")
    if formula:
        lines.append(f"- Formula: {formula}")
    missing = total.get("missing", [])
    if missing:
        if not isinstance(missing, list):
            raise InputError("four-frequency missing must be an array")
        lines.append("- Missing inputs [MISSING]: " + "; ".join(str(item) for item in missing))

    overall = summary.get("steady_state_fps")
    lines.extend(["", "Overall steady-state rate"])
    lines.append(f"- {status_metric(overall, 'overall FPS', 'Hz')}")
    if isinstance(overall, dict):
        lines.append(f"- Basis: {_source_text(overall)}")

    accuracy = summary.get("model_accuracy")
    if isinstance(accuracy, dict):
        lines.extend(["", "Model boundary"])
        lines.append(f"- Status: {str(accuracy.get('status', 'unspecified')).upper()}")
        if accuracy.get("conclusion"):
            lines.append(f"- {accuracy['conclusion']}")
    warnings = summary.get("warnings")
    if warnings:
        if not isinstance(warnings, list):
            raise InputError("summary warnings must be an array")
        lines.extend(["", "Warnings"])
        lines.extend(f"- {item}" for item in warnings)
    boundary = summary.get("evidence_boundary")
    if boundary:
        lines.extend(["", "Evidence boundary", str(boundary)])
    return "\n".join(lines).rstrip() + "\n"


def render_markdown_summary(markdown: str, source: Path) -> str:
    """Wrap an existing Markdown report without changing its claims.

    Markdown is a presentation artifact, so the generator cannot reconstruct
    structured provenance. Requiring an explicit status marker/word prevents
    accidentally mailing an unlabelled ad-hoc timing note.
    """
    markers = STATUS_RE.findall(markdown)
    if not markers and not STATUS_WORD_RE.search(markdown):
        raise InputError(
            f"Markdown summary {source} has no [MEASURED]/[ESTIMATED]/[MISSING] markers"
        )
    if markers:
        audit = "explicit bracket markers preserved"
    else:
        # Older project reports use a status column with plain words such as
        # `measured`/`estimated`. Keep those reports usable, but make the
        # weaker provenance visible in the outgoing draft.
        words = [item.upper() for item in STATUS_WORD_RE.findall(markdown)]
        counts = ", ".join(f"{name}={words.count(name)}" for name in ("MEASURED", "ESTIMATED", "MISSING"))
        audit = f"plain status words found ({counts}); no per-token reinterpretation"
    return (
        "RA8P1 SDR inference progress summary (source Markdown)\n\n"
        "The source report below is preserved verbatim. Status labels are "
        "taken from the source; this generator does not reinterpret numbers.\n"
        f"Status audit: {audit}.\n\n"
        f"Source: {source}\n\n"
        f"{markdown.rstrip()}\n"
    )


def load_summary(*, summary_json: Path | None, summary_md: Path | None) -> tuple[str, str]:
    if (summary_json is None) == (summary_md is None):
        raise InputError("provide exactly one of --summary-json or --summary-md")
    if summary_json is not None:
        try:
            payload = json.loads(read_text(summary_json))
        except json.JSONDecodeError as exc:
            raise InputError(f"cannot parse summary JSON {summary_json}: {exc}") from exc
        if not isinstance(payload, dict):
            raise InputError("summary JSON root must be an object")
        return render_json_summary(payload), "json"
    assert summary_md is not None
    return render_markdown_summary(read_text(summary_md), summary_md), "markdown"


def build_message(
    body: str,
    *,
    to: str,
    from_address: str,
    subject: str,
    source_kind: str,
) -> EmailMessage:
    message = EmailMessage(policy=email.policy.SMTP)
    message["From"] = from_address
    message["To"] = to
    message["Date"] = format_datetime(dt.datetime.now(dt.timezone.utc))
    message["Subject"] = subject
    message["X-Progress-Report"] = "RA8P1-SDR"
    message["X-Delivery-Status"] = "DRAFT-NOT-SENT"
    message["X-Source-Summary"] = source_kind
    message.set_content(body, charset="utf-8")
    return message


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    source = result.add_mutually_exclusive_group(required=True)
    source.add_argument("--summary-json", type=Path, help="ra8p1_performance_summary JSON output")
    source.add_argument("--summary-md", type=Path, help="performance summary Markdown output")
    result.add_argument("--to", required=True, help="recipient address; no message is sent")
    result.add_argument(
        "--from", dest="from_address", default="RA8P1 performance report <no-reply@example.invalid>",
        help="From header (default is a non-deliverable draft address)",
    )
    result.add_argument("--subject", help="Subject header")
    result.add_argument("--output-eml", required=True, type=Path, help="RFC 5322 draft output path")
    result.add_argument("--output-txt", type=Path, help="optional plain-text body output path")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        to = validate_address(args.to, "--to")
        from_address = validate_address(args.from_address, "--from")
        subject = validate_header_value(
            args.subject or "RA8P1 SDR inference progress (draft)", "--subject"
        )
        body, source_kind = load_summary(summary_json=args.summary_json, summary_md=args.summary_md)
        message = build_message(
            body,
            to=to,
            from_address=from_address,
            subject=subject,
            source_kind=source_kind,
        )
        args.output_eml.parent.mkdir(parents=True, exist_ok=True)
        args.output_eml.write_bytes(message.as_bytes(policy=email.policy.SMTP))
        if args.output_txt:
            args.output_txt.parent.mkdir(parents=True, exist_ok=True)
            args.output_txt.write_text(body, encoding="utf-8", newline="\n")
    except (OSError, InputError) as exc:
        print(f"progress email input/output error: {exc}", file=sys.stderr)
        return 2
    print(f"wrote draft (not sent): {args.output_eml}")
    if args.output_txt:
        print(f"wrote body: {args.output_txt}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
