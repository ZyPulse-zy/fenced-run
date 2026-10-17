// SPDX-License-Identifier: GPL-2.0
// Bounded execution with descendant cleanup and inherited exclusion.
// Derived from Athena NSS process_guard.c; see PROVENANCE.md.
#define _GNU_SOURCE
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNKNOWN 125
#define OUTPUT_LIMIT (1024U * 1024U)
#define CLEANUP_MS 1000
#define CHILD_SCAN_MS 100
#define CHILD_SCAN_LIMIT 32768U
static volatile sig_atomic_t interrupted;
static void signal_seen(int sig) { interrupted = sig; }
static long long monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static int number(const char *s, long min, long max, long *value)
{
	char *end; long v;
	if (!s || !*s) return -1;
	for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return -1;
	errno = 0; v = strtol(s, &end, 10);
	if (errno || *end || v < min || v > max) return -1;
	*value = v; return 0;
}
static int pidfd_open_exact(pid_t pid)
{
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open, pid, 0);
#else
	errno = ENOSYS; return -1;
#endif
}
static int pidfd_signal(int fd, int sig)
{
#ifdef SYS_pidfd_send_signal
	return (int)syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
#else
	(void)fd; (void)sig; errno = ENOSYS; return -1;
#endif
}
static int small_read(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC); ssize_t n;
	if (fd < 0) return -1;
	do { n = read(fd, buf, size - 1); } while (n < 0 && errno == EINTR);
	int saved = errno; close(fd); errno = saved;
	if (n < 0) return -1;
	if ((size_t)n == size - 1) { errno = EOVERFLOW; return -1; }
	buf[n] = 0; return 0;
}
// 1 is confirmed exit, 0 is still live, -1 is unknown.
static int pidfd_exited(int fd, int timeout)
{
	struct pollfd p = { fd, POLLIN, 0 }; int rc;
	do { rc = poll(&p, 1, timeout); } while (rc < 0 && errno == EINTR);
	if (rc < 0 || (p.revents & (POLLNVAL | POLLERR))) return -1;
	return rc && (p.revents & POLLIN) ? 1 : 0;
}
struct stream { int fd; unsigned char *bytes; size_t used; };
static int drain(struct stream *s, size_t *total)
{
	unsigned char chunk[8192];
	if (s->fd < 0) return 0;
	for (;;) {
		ssize_t n = read(s->fd, chunk, sizeof(chunk));
		if (n > 0) {
			if ((size_t)n > OUTPUT_LIMIT - *total) return -1;
			memcpy(s->bytes + s->used, chunk, (size_t)n);
			s->used += (size_t)n; *total += (size_t)n;
		} else if (!n) { close(s->fd); s->fd = -1; return 0; }
		else if (errno == EINTR) continue;
		else return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
	}
}
static int emit(int fd, const unsigned char *bytes, size_t length)
{
	int original = fcntl(fd, F_GETFL); size_t at = 0;
	long long deadline = monotonic_ms() + 250;
	if (original < 0 || fcntl(fd, F_SETFL, original | O_NONBLOCK)) return -1;
	while (at < length && monotonic_ms() < deadline) {
		ssize_t n = write(fd, bytes + at, length - at);
		if (n > 0) at += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd p = { fd, POLLOUT, 0 }; (void)poll(&p, 1, 10);
		} else break;
	}
	if (fcntl(fd, F_SETFL, original)) return -1;
	return at == length ? 0 : -1;
}
static int visit_proc_children(int (*visit)(pid_t))
{
	// Some target kernels omit task/children. A subreaper's direct children
	// remain ours until waitpid reaps them; SIGCHLD is explicitly defaulted.
	// Never treat a partial or unreadable /proc scan as an empty child set.
	DIR *directory = opendir("/proc"); struct dirent *entry;
	unsigned scanned = 0; int result = 0; long long began = monotonic_ms();
	if (!directory) return -1;
	if (began < 0) { closedir(directory); return -1; }
	for (;;) {
		errno = 0; entry = readdir(directory);
		if (!entry) { if (errno || monotonic_ms() >= began + CHILD_SCAN_MS) result = -1; break; }
		if (++scanned > CHILD_SCAN_LIMIT || monotonic_ms() >= began + CHILD_SCAN_MS) { result = -1; break; }
		long pid;
		if (number(entry->d_name, 1, INT_MAX, &pid)) continue;
		char path[80], buf[4096], *p, *end;
		snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
		if (small_read(path, buf, sizeof(buf))) {
			if (errno == ENOENT || errno == ESRCH) continue;
			result = -1; break;
		}
		p = strrchr(buf, ')');
		if (!p || p[1] != ' ' || !p[2] || p[3] != ' ') { result = -1; break; }
		errno = 0; long parent = strtol(p + 4, &end, 10);
		if (errno || end == p + 4 || *end != ' ' || parent < 0 || parent > INT_MAX) { result = -1; break; }
		if (parent == (long)getpid() && (result = visit((pid_t)pid))) break;
	}
	if (closedir(directory)) result = -1;
	return result;
}
static int visit_children(int (*visit)(pid_t))
{
	char path[96], buf[32768], *p;
	snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
	if (small_read(path, buf, sizeof(buf))) {
		if (errno == ENOENT || errno == ENOTDIR) return visit_proc_children(visit);
		return -1;
	}
	long long began = monotonic_ms();
	if (began < 0) return -1;
	p = buf;
	while (*p) {
		char *end; long child = strtol(p, &end, 10);
		if (end == p || child < 1 || child > INT_MAX || monotonic_ms() >= began + CHILD_SCAN_MS) return -1;
		int result = visit((pid_t)child);
		if (result) return result;
		p = end; while (*p == ' ') ++p;
	}
	return 0;
}
static int kill_child(pid_t child)
{
	int fd = pidfd_open_exact(child);
	if (fd < 0) return errno == ESRCH ? 0 : -1;
	int rc = pidfd_signal(fd, SIGKILL), saved = errno; close(fd);
	return rc && saved != ESRCH ? -1 : 0;
}
static int child_exited(pid_t child)
{
	int fd = pidfd_open_exact(child);
	if (fd < 0) return errno == ESRCH ? 0 : -1;
	int gone = pidfd_exited(fd, 0); close(fd);
	return gone == 1 ? 0 : gone < 0 ? -1 : 1;
}
static int kill_adopted(void) { return visit_children(kill_child); }
static int children_exited(void) { return visit_children(child_exited) == 0; }
static int run_command(int argc, char **argv)
{
	long timeout; int pipes[2][2], ready[2], ack[2], reason = 0, lead_status = 0;
	int lead_seen = 0, clean = 0, ready_seen = 0; pid_t child;
	struct stream streams[2]; size_t total = 0; siginfo_t status;
	if (argc < 6 || number(argv[2], 1, 600000, &timeout) || strcmp(argv[4], "--") || argv[5][0] != '/') return 2;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)) return UNKNOWN;
	struct sigaction child_sa = {0}; child_sa.sa_handler = SIG_DFL; sigemptyset(&child_sa.sa_mask);
	if (sigaction(SIGCHLD, &child_sa, NULL)) return UNKNOWN;
	struct sigaction sa = {0}; sa.sa_handler = signal_seen; sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) || sigaction(SIGHUP, &sa, NULL)) return UNKNOWN;
	signal(SIGPIPE, SIG_IGN);
	for (int i = 0; i < 2; ++i) {
		if (pipe2(pipes[i], O_CLOEXEC)) return UNKNOWN;
		streams[i].fd = pipes[i][0]; streams[i].used = 0;
		streams[i].bytes = malloc(OUTPUT_LIMIT);
		if (!streams[i].bytes) return UNKNOWN;
		if (fcntl(streams[i].fd, F_SETFL, O_NONBLOCK)) return UNKNOWN;
	}
	if (pipe2(ready, O_CLOEXEC) || pipe2(ack, O_CLOEXEC)) return UNKNOWN;
	long long began = monotonic_ms(), deadline = began + timeout;
	if (began < 0 || (child = fork()) < 0) return UNKNOWN;
	if (!child) {
		close(ready[0]); close(ack[1]);
		signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGPIPE, SIG_DFL);
		for (int i = 0; i < 2; ++i) {
			close(pipes[i][0]); if (dup2(pipes[i][1], i + 1) < 0) _exit(126); close(pipes[i][1]);
		}
		if (setsid() < 0) _exit(126);
		char byte = 'R'; if (write(ready[1], &byte, 1) != 1) _exit(126); close(ready[1]);
		if (read(ack[0], &byte, 1) != 1 || byte != 'A') _exit(126);
		close(ack[0]); execv(argv[5], argv + 5); _exit(127);
	}
	close(ready[1]); close(ack[0]);
	for (int i = 0; i < 2; ++i) close(pipes[i][1]);
	while (!interrupted && monotonic_ms() < deadline) {
		struct pollfd p = { ready[0], POLLIN, 0 }; int rc = poll(&p, 1, 10);
		if (rc < 0 && errno != EINTR) { reason = UNKNOWN; break; }
		if (rc > 0) { char byte; if (read(ready[0], &byte, 1) == 1 && byte == 'R') ready_seen = 1; else reason = UNKNOWN; break; }
	}
	close(ready[0]);
	if (ready_seen && !interrupted && monotonic_ms() < deadline) {
		char byte = 'A'; if (write(ack[1], &byte, 1) != 1) reason = UNKNOWN;
	} else if (!reason) reason = interrupted ? 128 + interrupted : 124;
	close(ack[1]);
	while (!reason) {
		for (int i = 0; i < 2; ++i) if (drain(&streams[i], &total)) reason = 123;
		// A late observation cannot establish that completion was in time.
		// In particular, scheduling delay must not turn an expired command
		// into a successful result merely because waitid now sees a zombie.
		if (!reason && interrupted) reason = 128 + interrupted;
		else if (!reason && monotonic_ms() >= deadline) reason = 124;
		if (reason) break;
		memset(&status, 0, sizeof(status));
		if (waitid(P_PID, (id_t)child, &status, WEXITED | WNOHANG | WNOWAIT)) { reason = UNKNOWN; break; }
		if (status.si_pid == child) break;
		pause_ms(10);
	}
	// Keep the leader unreaped through both group signals: its PGID cannot
	// refer to a new unrelated process group. A pre-exec child is also ours.
	if (ready_seen) {
		if (kill(-child, SIGTERM) && errno != ESRCH) reason = UNKNOWN;
	} else if (kill(child, SIGTERM) && errno != ESRCH) reason = UNKNOWN;
	long long cleanup_start = monotonic_ms();
	while (monotonic_ms() < cleanup_start + 150) {
		for (int i = 0; i < 2; ++i) if (drain(&streams[i], &total)) reason = 123;
		if (children_exited()) break;
		pause_ms(10);
	}
	if (ready_seen) {
		if (kill(-child, SIGKILL) && errno != ESRCH) reason = UNKNOWN;
	} else if (kill(child, SIGKILL) && errno != ESRCH) reason = UNKNOWN;
	while (monotonic_ms() < cleanup_start + CLEANUP_MS) {
		if (kill_adopted()) reason = UNKNOWN;
		for (;;) {
			int st; pid_t got = waitpid(-1, &st, WNOHANG);
			if (got > 0) { if (got == child) { lead_seen = 1; lead_status = st; } continue; }
			if (got < 0 && errno == EINTR) continue;
			if (got < 0 && errno == ECHILD) clean = 1;
			else if (got < 0) reason = UNKNOWN;
			break;
		}
		for (int i = 0; i < 2; ++i) if (drain(&streams[i], &total)) reason = 123;
		if (clean) break;
		pause_ms(10);
	}
	// Any survivor retains the inherited exclusion lock even after we return.
	// No caller may treat UNKNOWN as permission to overlap a new writer.
	if (!clean || !lead_seen) reason = UNKNOWN;
	if (!reason && interrupted) reason = 128 + interrupted;
	for (int i = 0; i < 2; ++i) {
		if (streams[i].fd >= 0) close(streams[i].fd);
		if (emit(i + 1, streams[i].bytes, streams[i].used) && !reason) reason = 123;
		free(streams[i].bytes);
	}
	if (reason) return reason;
	if (WIFEXITED(lead_status)) return WEXITSTATUS(lead_status);
	if (WIFSIGNALED(lead_status)) return 128 + WTERMSIG(lead_status);
	return UNKNOWN;
}
static int acquire_lock(const char *path)
{
    struct stat st, current;
    if (path[0] != '/') return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 0022)) goto invalid;
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int saved = errno;
        close(fd); errno = saved; return -1;
    }
    if (lstat(path, &current) || st.st_dev != current.st_dev ||
        st.st_ino != current.st_ino) goto invalid;
    char value[32]; snprintf(value, sizeof(value), "%d", fd);
    if (setenv("FENCED_RUN_LOCK_FD", value, 1)) goto invalid;
    return fd;
invalid:
    close(fd); errno = EINVAL; return -1;
}
static int close_inherited(void)
{
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int scan = dirfd(directory); struct dirent *entry; int result = 0;
    for (;;) {
        errno = 0; entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        long fd;
        if (!number(entry->d_name, 3, INT_MAX, &fd) && fd != scan &&
            close((int)fd) && errno != EBADF) { result = -1; break; }
    }
    if (closedir(directory)) result = -1;
    return result;
}
static int ensure_standard_fds(void)
{
    for (int fd = 0; fd < 3; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) continue;
        if (errno != EBADF) return -1;
        int opened = open("/dev/null", fd ? O_WRONLY : O_RDONLY);
        if (opened != fd) { if (opened >= 0) close(opened); return -1; }
    }
    return 0;
}
int main(int argc, char **argv)
{
    long timeout;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("fenced-run 0.1.0"); return 0;
    }
    if (argc < 6 || strcmp(argv[1], "run") ||
        number(argv[2], 1, 600000, &timeout) || argv[3][0] != '/' ||
        strcmp(argv[4], "--") || argv[5][0] != '/') {
        fputs("usage: fenced-run run TIMEOUT_MS /absolute/lock -- /absolute/program [args...]\n", stderr);
        return 2;
    }
    if (ensure_standard_fds() || close_inherited()) return UNKNOWN;
    int lock = acquire_lock(argv[3]);
    if (lock < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return 75;
        perror("fenced-run lock"); return UNKNOWN;
    }
    // Probe before executing a possibly mutating command. Older kernels or
    // seccomp policies without pidfds must fail before effects are possible.
    int probe = pidfd_open_exact(getpid());
    if (probe < 0) { close(lock); return UNKNOWN; }
    int supported = pidfd_signal(probe, 0); close(probe);
    if (supported) { close(lock); return UNKNOWN; }
    int result = run_command(argc, argv);
    close(lock);
    return result;
}
