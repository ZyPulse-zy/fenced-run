# fenced-run

Run a trusted, finite Linux command with a deadline, bounded output, inherited
writer exclusion and explicit descendant cleanup. Intended for network
controllers which invoke `tc`, `ip`, `nft` or small platform helpers and must not
start a competing writer while an old command can still be running.

```sh
make
mkdir -m 700 /tmp/my-controller
./fenced-run run 1500 /tmp/my-controller/writer.lock -- /usr/bin/true
```

The program does not configure networking itself. It has no NSS, WAN-count,
interface-name, OpenWrt service-manager or device dependency. The caller owns
the policy and recovery logic. See [PROVENANCE.md](PROVENANCE.md) for its Athena
NSS origin and GPL-2.0-only license.

## Execution contract

1. Acquire a nonblocking exclusive `flock` on the supplied stable lock file.
   Reject symlinks, non-regular files, multiple hard links, a different owner or
   group/world-writable lock files. Never unlink the lock file.
2. Probe pidfd support **before executing the command**. Close unrelated
   inherited descriptors and export `FENCED_RUN_LOCK_FD`. That open file
   description survives the command's forks and execs.
3. Become a child subreaper and start the command in a new session, using an
   acknowledgment handshake before exec. Measure the deadline with
   `CLOCK_MONOTONIC`; a late completion observation is a timeout.
4. Capture stdout/stderr separately, with a combined 1 MiB limit. On completion
   or failure, terminate the original process group while its leader is still
   unreaped. Reclaim adopted descendants with pidfd signals, including children
   which used `setsid()` or double-forked. Confirm cleanup with `waitpid`.
5. Return the command status only after confirmed cleanup. Emit buffered output
   with a separate 250 ms deadline per stream. Output is diagnostic data;
   **always check the exit status**.

This combines exclusion, command lifetime and cleanup confirmation in one
protocol. A process-group timeout alone does not reclaim detached descendants;
a lock held only by the supervisor disappears if that supervisor is killed.
Both cases are covered by real-process regressions here.

## Exit status and recovery

| Status | Meaning |
| --- | --- |
| 0 | Command succeeded within deadline and descendants were reaped |
| 2 | Invalid arguments; command not started |
| 75 | Lock busy; command not started |
| 123 | Output limit, capture or delivery failure |
| 124 | Command deadline expired |
| 125 | Unsupported facility, invalid lock or uncertain execution/cleanup |
| 127 | Command could not be executed |
| Other | Command exit status, or 128 + terminating signal |

Statuses may overlap with a command's own nonzero codes. All nonzero results
must be treated as failures. A timeout, killed command or uncertain cleanup is
**not rollback**: a previous `tc`/`nft` write may already have taken effect.
Read back the state and reconcile it under the same exclusion protocol before
starting a new transaction. Do not assume that a successful signal means exit.

The command deadline is 1–600000 ms. Cleanup adds at most 1000 ms of userspace
polling; output adds up to 500 ms. These are observation bounds under normal
scheduling, not hard real-time guarantees. Kernel filesystem calls, memory
allocation, scheduler starvation, suspended processes and uninterruptible
kernel tasks can exceed them. An uncertain survivor which preserves its lock
FD continues to exclude another cooperating writer even after the supervisor
returns or is killed.

## Requirements and trust boundary

- Linux with pidfd_open/pidfd_send_signal (normally 5.3+) and child subreapers;
  `/proc` must describe the calling PID namespace. Requires C11 and libc only.
- No root is needed for process tests. The command needs its usual privileges
  for actual network operations. No daemon, setuid installation or capabilities
  are installed by this project.
- Use a **local filesystem and a stable private directory owned by the same
  user**. All cooperating writers must use the exact same lock path. Do not
  rename/unlink the file or replace parent directories while writers may exist.
- Commands are trusted, finite helpers. They must preserve the inherited lock
  FD until all possible writes finish and must not call `flock(LOCK_UN)` on it.
  Programs which close all extra FDs, malicious commands, foreign services
  started over IPC, other PID namespaces and writers ignoring this protocol
  are outside the exclusion guarantee. This is not a sandbox.
- The fallback for kernels without `/proc/.../children` scans `/proc` with
  bounded size/time and matches direct child parent IDs. An incomplete scan
  means uncertainty, never proof that no child exists.
- Do not wrap long-running services. Children are terminated even if the main
  command exits successfully. Do not nest this program as a lock handoff API.

## Build, tests and OpenWrt

```sh
make test         # actual fork/exec, pidfds, sessions, locks and seccomp
make sanitize     # same suite with ASan + UBSan
make integration  # isolated real tc partial-write timeout and recovery
make openwrt-package
```

`make integration` needs iproute2, CAKE and user/network namespaces. All network
operations occur inside a fresh namespace. It does not touch a production
router, service or host interface. CI tests glibc, sanitizers and Alpine musl.
See [VALIDATION.md](VALIDATION.md) for executed results and limitations.

Copy `build/openwrt/fenced-run` into `package/` in a **separate** OpenWrt SDK and
build `package/fenced-run/compile`. The package has no init script and never
runs on installation. Installation or use on a production router is a separate
operator decision.
