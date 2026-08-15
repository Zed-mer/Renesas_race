# Versioned Firmware Bundles

Each `T<number>` directory contains the paired CPU0 and CPU1 ELFs for that
source release. These files are committed so a known release can be flashed
without rebuilding it.

Release requirements:

1. Build both cores from the exact tagged source tree.
2. Run `verify-solution.ps1 -SkipBuild`.
3. Store both ELFs and a `MANIFEST.sha256` file under the matching version.
4. Flash CPU0 and CPU1 together through the Solution `Debug_Multicore` launch.

Never mix ELFs from different version directories.
