# Source provenance

`src/fenced-run.c` derives from the GPL-2.0 `process_guard.c` in the Athena NSS
controller's 2026-10-17 local delivery snapshot. The source project is
https://github.com/ZyPulse-zy/athena-nss-mainline; the local delivery candidate
identifier was `e5d1b5d85320affd6f71e04fe926b5d3272acbd60b6a925530f0377cc73893a1`.
That identifier describes a delivery artifact, not a public Git commit.

The source file SHA-256 was
`afaad2e96191fa7ab24a37f381c1b02cd34503b2445595bdbecb1d4c3e3da00f`.
This repository removes Athena's service roles, fixed paths,
process-stop/observer APIs and environment names. It adds a
single generic lock-and-run interface, pre-execution pidfd capability probe,
strict standalone build, real process/lock tests and independent CI.
New development uses Yuen Su <zypulsecore@gmail.com>.

The original license is GPL-2.0; this derivative uses GPL-2.0-only (see COPYING).
AI assistance was used for the extraction, implementation, tests and docs.
