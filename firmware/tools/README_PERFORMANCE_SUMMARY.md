# Offline performance summary

`ra8p1_performance_summary.py` joins sender stdout with one or more
`ra8p1-runtime-sampler.ps1 -Json` files. It is read-only: it does not contact
the SDR, RA8P1, J-Link, or network, and it cannot change SDR firmware or IP.

```powershell
python .\tools\ra8p1_performance_summary.py `
  --sender-log .\build\evidence\sender.log `
  --runtime .\build\evidence\session-2420-runtime.json `
  --runtime .\build\evidence\session-2464-runtime.json `
  --runtime .\build\evidence\session-5760-runtime.json `
  --runtime .\build\evidence\session-5816-runtime.json `
  --capture-ms 10 `
  --tune-ms 0.5 `
  --output-json .\build\evidence\performance-summary.json `
  --output-md .\build\evidence\performance-summary.md
```

`--capture-ms` and `--tune-ms` are optional fallbacks. They are always labeled
`estimated`; a `captured ... capture_ms=N` or `tuned ... tune_ms=N` sender line
is labeled `measured`. The tool reports:

- capture, tune, full-session send, STFT, NPU, E2E, and CPU1-visible duration;
- per-window send time and steady inference FPS;
- fixed-order 2420/2464/5760/5816 MHz sequential coverage time;
- source path, scope, basis, and `measured` / `estimated` / `missing` status for
  every value.

The sender's `payload_mbps_x1000` is a measured rate, but converting it into a
wire time is a derived estimate. STFT/NPU/E2E and CPU1-visible fields are
hardware measurements from the matching runtime session. Runtime evidence is
never reused merely because another session used the same center frequency.

Without explicit cycle start/end timestamps, a four-frequency value is a
serial estimate:

```text
sum(center capture + tune + full-session send
    + window_count * (E2E + CPU1-visible))
```

If any required stage is absent, the total is `missing`; the JSON and Markdown
list each missing component. The tool does not silently replace missing stages
with zero or add unrelated file timestamps.

The checked-in NPU network is a placeholder. Timing and pipeline evidence
cannot support a drone-detection accuracy conclusion.

Run the offline tests with:

```powershell
python -m unittest .\tools\test_ra8p1_performance_summary.py -v
```
