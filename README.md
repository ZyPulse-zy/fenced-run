# fenced-run

Run a trusted Linux command with a deadline, bounded output and a shared writer
lock. Designed for controllers that invoke finite helpers such as `tc`, `ip`
or `nft` and need to coordinate command execution and descendant cleanup.

## Build and use

Requires a C11 compiler, libc, Linux pidfd support (normally kernel 5.3+) and
`/proc` mounted for the calling PID namespace.

```sh
make
mkdir -m 700 /tmp/my-controller
./fenced-run run 1500 /tmp/my-controller/writer.lock -- /usr/bin/true
make install PREFIX=/usr/local
```

Syntax: `fenced-run run TIMEOUT_MS /absolute/lock -- /absolute/program [args...]`.
The deadline accepts 1–600000 ms. The command runs with the caller's privileges.

## Behavior

- Acquires a nonblocking exclusive `flock` and passes the lock descriptor to
  the command through `FENCED_RUN_LOCK_FD`.
- Runs the command in a new session, captures stdout/stderr with a combined
  1 MiB limit, and measures elapsed time with a monotonic clock.
- Terminates remaining descendants after command completion or failure,
  including detached children, using a child subreaper and pidfd signals.
- Returns the command status only after confirmed cleanup. Uncertain execution
  or cleanup returns a failure.

Cleanup adds up to 1000 ms of userspace polling; output delivery adds up to
500 ms. Scheduling delays and blocking kernel operations can exceed these
bounds.

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | Command succeeded within the deadline and descendants were reaped |
| 2 | Invalid arguments; command not started |
| 75 | Lock busy; command not started |
| 123 | Output limit, capture or delivery failure |
| 124 | Command deadline expired |
| 125 | Unsupported facility, invalid lock or uncertain execution/cleanup |
| 127 | Command could not be executed |
| Other | Command exit status, or 128 + terminating signal |

A command's own nonzero codes can overlap these values. Check the exit status
before using captured output. A timeout does not undo completed writes;
reconcile partial changes under the same writer lock before retrying.

## Integration requirements

Use a stable lock file in a private directory on a local filesystem, owned by
the calling user. The lock file must be a regular file with one hard link and
no group/world write permission. All writers must use the same path; keep the
file and its parent directories in place while writers may exist.

Helpers must preserve the inherited lock descriptor until their writes finish
and must not unlock it explicitly. Writers that ignore this protocol, commands
that close the descriptor, external services started over IPC, and processes
in other PID namespaces are outside its guarantees. This tool is intended for
trusted, finite helpers; remaining children are terminated even after success.

## Tests and OpenWrt package

```sh
make test
make sanitize
make integration
make openwrt-package
```

Process tests require Python 3. Integration tests additionally require
iproute2, CAKE and user/network namespaces. See [VALIDATION.md](VALIDATION.md)
for coverage and results.

Copy `build/openwrt/fenced-run` into an OpenWrt SDK's `package/` directory, then
run `make package/fenced-run/compile V=s` in that SDK.

## License

GPL-2.0-only; see [COPYING](COPYING).
Source history and attribution are in [PROVENANCE.md](PROVENANCE.md).
