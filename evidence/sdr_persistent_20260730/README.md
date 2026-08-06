# SDR persistent deployment verification (2026-07-30)

The content-addressed SDR agent and mmap adapter were installed under
`/mnt/jffs2/ra8p1`, with `/mnt/jffs2/autorun.sh` acting as the supervised boot
entry. The previous 434-line supervisor was preserved as
`/mnt/jffs2/autorun.sh.backup.5f0f898d6956`; no historical backup or old binary
was removed.

The SDR subsequently performed a full Linux reboot. At uptime 104 seconds the
temporary upload bundle was absent, while the new persistent agent had already
been started by the autorun path at boot second 7. Its executable and adapter
both resolved to `/mnt/jffs2`, the command line used `--no-trace`, and all four
required tune/transport environment settings were present.

During a 3.375-second post-boot network interval, 18 capture windows completed.
Retry, timeout, sequence gap, reorder, invalid IQ, CRC, ring drop and lwIP input
failure deltas were all zero. Snapshot payload rates were 331.946-389.797 Mbps.

The RA8P1 display counters also advanced after the SDR reboot: presented frames
4086->4100, content frames 4072->4086, waterfall/IPC tiles 10992->11025 and IPC
frames 687->689. Waterfall drops and IPC misses remained zero; NPU rate was
5.17 Hz and presented rate was 30.303 FPS. CPU0 CFSR and HFSR were both zero.

See `verification_summary.json` for the exact hashes, paths and counters.
