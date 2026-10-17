# Validation evidence (2026-10-18, Asia/Shanghai)

Executed in a separate workspace; the original Athena project and router were
not modified. No production services, qdiscs, flows or configurations changed.

- GCC 15.2, strict C11 `-Wall -Wextra -Werror -Wconversion -Wshadow` build.
- 28 actual-process tests passed on Linux/glibc, then the same 28 passed with a
  link-time `open()` wrapper forcing the `/proc` child-discovery fallback.
- Both 28-test variants passed with AddressSanitizer and UndefinedBehaviorSanitizer.
- Real network namespace integration passed: CAKE 2 Mbit partial write remains
  after timeout, then a separate invocation takes the same lock and reconciles
  it to 1 Mbit. This proves timeout is not rollback, with actual `tc` readback.
- AArch64/musl APK built using an independent OpenWrt SDK copy with GCC 14.4.0.
  The extracted ELF reports `fenced-run 0.1.0` under QEMU user mode. This is only
  an ABI/CLI smoke check; it is not target-kernel pidfd, process or hardware proof.

The process tests include detached/double-forked descendants, TERM-resistant
children, partial effects, signals, busy/unsafe locks, supervisor SIGKILL with
inherited exclusion, unrelated sibling preservation, output overflow and a
blocked consumer, late completion observation, closed stdin, ignored SIGCHLD,
and a seccomp-induced unavailable pidfd syscall before command execution.

No real uninterruptible kernel task or malicious command was injected. The
lock inheritance guarantee depends on trusted commands preserving the FD.
A failed/uncertain result must not be interpreted as rollback or absence of
all effects. Filesystem stalls and scheduler pauses are outside hard deadline
guarantees. NSS hardware, router deployment and game performance are untested.

Remote CI status is recorded separately after its actual run is observed.

Remote run 39049527981: Linux/glibc, both sanitizer variants, and the
real-kernel namespace integration passed. Alpine musl initially stopped at
compiling the seccomp test helper because linux/filter.h was missing. The CI
container now explicitly installs linux-headers; this failure was not reported
as a passing musl test.
