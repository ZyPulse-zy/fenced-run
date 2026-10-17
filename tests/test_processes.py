#!/usr/bin/env python3
"""Real process/lock tests. No router, service, qdisc or root access needed."""
import ctypes
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else str(Path('fenced-run').resolve())


class Processes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Keep the intentional supervisor-SIGKILL test's orphan ours to reap.
        if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):
            raise OSError(ctypes.get_errno(), 'PR_SET_CHILD_SUBREAPER')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.lock = self.root / 'writer.lock'
        self.children = []

    def tearDown(self):
        for p in self.children:
            if p.poll() is None:
                p.kill()
                p.wait()
        self.temporary.cleanup()

    def command(self, code, ms=1500):
        return [BINARY, 'run', str(ms), str(self.lock), '--', sys.executable, '-c', code]

    def run_code(self, code, ms=1500):
        started = time.monotonic()
        p = subprocess.run(self.command(code, ms), capture_output=True, timeout=5)
        self.assertLess(time.monotonic() - started, ms / 1000 + 2.5)
        return p

    def wait_file(self, p, child):
        end = time.monotonic() + 3
        while not p.exists():
            self.assertIsNone(child.poll(), child.stderr.read() if child.poll() is not None else '')
            self.assertLess(time.monotonic(), end)
            time.sleep(.005)
        return p.read_text()

    def assert_dead(self, pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        self.fail(f'owned descendant {pid} still exists')

    def test_exit_and_streams(self):
        p = self.run_code("import os;os.write(1,b'ACK\\x00tail');os.write(2,b'err')")
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b'ACK\x00tail', b'err'))

    def test_nonzero(self):
        self.assertEqual(self.run_code('raise SystemExit(9)').returncode, 9)

    def test_exec_failure(self):
        p = subprocess.run([BINARY, 'run', '1000', str(self.lock), '--', '/no/such/program'], capture_output=True)
        self.assertEqual(p.returncode, 127)

    def test_child_signal(self):
        p = self.run_code('import os,signal;os.kill(os.getpid(),signal.SIGUSR1)')
        self.assertEqual(p.returncode, 128 + signal.SIGUSR1)

    def test_timeout_partial_effect(self):
        marker = self.root / 'effect'
        p = self.run_code(f"import pathlib,time;pathlib.Path({str(marker)!r}).write_text('applied');time.sleep(30)", 200)
        self.assertEqual(p.returncode, 124)
        self.assertEqual(marker.read_text(), 'applied')
        self.assertEqual(self.run_code('pass').returncode, 0)

    def test_term_ignored(self):
        p = self.run_code('import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(30)', 200)
        self.assertEqual(p.returncode, 124)

    def test_detached_descendant(self):
        marker = self.root / 'escaped'
        # A new session cannot be reclaimed by signaling the original PGID.
        code = f'''import os,pathlib,time
p=os.fork()
if p==0:
 os.setsid();pathlib.Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(30)
else:
 while not pathlib.Path({str(marker)!r}).exists():time.sleep(.005)
'''
        p = self.run_code(code)
        self.assertEqual(p.returncode, 0)
        self.assert_dead(int(marker.read_text()))

    def test_detached_grandchild_timeout(self):
        marker = self.root / 'escaped'
        code = f'''import os,pathlib,time
p=os.fork()
if p==0:
 os.setsid();p=os.fork()
 if p==0:pathlib.Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(30)
 else:os._exit(0)
else:time.sleep(30)
'''
        p = self.run_code(code, 200)
        self.assertEqual(p.returncode, 124)
        self.assert_dead(int(marker.read_text()))

    def test_background_child(self):
        marker = self.root / 'child'
        p = self.run_code(f'''import os,pathlib,time
if os.fork()==0:
 pathlib.Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(30)
else:
 while not pathlib.Path({str(marker)!r}).exists():time.sleep(.005)
''')
        self.assertEqual(p.returncode, 0)
        self.assert_dead(int(marker.read_text()))

    def test_busy_lock_does_not_run(self):
        with self.lock.open('w') as f:
            os.chmod(self.lock, 0o600)
            fcntl.flock(f, fcntl.LOCK_EX)
            marker = self.root / 'unexpected'
            p = self.run_code(f'import pathlib;pathlib.Path({str(marker)!r}).touch()')
            self.assertEqual(p.returncode, 75)
            self.assertFalse(marker.exists())

    def test_lock_inode_retained(self):
        self.assertEqual(self.run_code('pass').returncode, 0)
        identity = self.lock.stat().st_ino
        self.assertEqual(self.run_code('pass').returncode, 0)
        self.assertEqual(self.lock.stat().st_ino, identity)
        self.assertEqual(self.lock.stat().st_mode & 0o777, 0o600)

    def test_lock_inherited_through_exec(self):
        p = self.run_code("import os,fcntl;fd=int(os.environ['FENCED_RUN_LOCK_FD']);assert not(fcntl.fcntl(fd,fcntl.F_GETFD)&fcntl.FD_CLOEXEC);assert os.fstat(fd).st_nlink==1")
        self.assertEqual(p.returncode, 0)

    def test_symlink_rejected(self):
        target = self.root / 'target'; target.touch(); self.lock.symlink_to(target)
        self.assertEqual(self.run_code('pass').returncode, 125)

    def test_hardlink_rejected(self):
        self.lock.touch(mode=0o600); os.link(self.lock, self.root / 'second')
        self.assertEqual(self.run_code('pass').returncode, 125)

    def test_writable_lock_rejected(self):
        self.lock.touch(mode=0o666); self.lock.chmod(0o666)
        self.assertEqual(self.run_code('pass').returncode, 125)

    def test_fifo_rejected_without_blocking(self):
        os.mkfifo(self.lock)
        self.assertEqual(self.run_code('pass').returncode, 125)

    def test_output_limit(self):
        p = self.run_code("import os;os.write(1,b'x'*1048577)")
        self.assertEqual(p.returncode, 123)
        self.assertLessEqual(len(p.stdout) + len(p.stderr), 1048576)

    def test_combined_output_limit(self):
        p = self.run_code("import os;os.write(1,b'x'*700000);os.write(2,b'y'*700000)")
        self.assertEqual(p.returncode, 123)
        self.assertLessEqual(len(p.stdout) + len(p.stderr), 1048576)

    def test_exact_output_limit(self):
        p = self.run_code("import os;os.write(1,b'x'*1048576)")
        self.assertEqual(p.returncode, 0)
        self.assertEqual(len(p.stdout), 1048576)

    def test_interrupt_cleanup(self):
        marker = self.root / 'leader'
        p = subprocess.Popen(self.command(f"import os,pathlib,time;pathlib.Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(30)"), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(p)
        leader = int(self.wait_file(marker, p))
        p.send_signal(signal.SIGTERM)
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 128 + signal.SIGTERM)
        self.assert_dead(leader)

    def test_external_supervisor_kill_keeps_lock_in_child(self):
        marker = self.root / 'leader'
        p = subprocess.Popen(self.command(f"import os,pathlib,time;pathlib.Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(30)"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(p)
        leader = int(self.wait_file(marker, p))
        try:
            p.kill(); p.wait(timeout=2)
            self.assertEqual(self.run_code('pass').returncode, 75)
        finally:
            os.kill(leader, signal.SIGKILL)
            os.waitpid(leader, 0)
        end = time.monotonic() + 2
        while self.run_code('pass').returncode == 75:
            self.assertLess(time.monotonic(), end)
            time.sleep(.01)

    def test_blocked_output_consumer(self):
        p = subprocess.Popen(self.command("import os;os.write(1,b'x'*60000)"), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(p)
        fcntl.fcntl(p.stdout, fcntl.F_SETPIPE_SZ, 4096)
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 123)
        p.communicate()

    def test_sibling_untouched(self):
        sibling = subprocess.Popen(['/bin/sleep', '30']); self.children.append(sibling)
        self.assertEqual(self.run_code('import time;time.sleep(30)', 100).returncode, 124)
        self.assertIsNone(sibling.poll())

    def test_closed_stdin_keeps_lock_above_stdio(self):
        code = "import os;assert int(os.environ['FENCED_RUN_LOCK_FD'])>=3"
        p = subprocess.run(self.command(code), preexec_fn=lambda: os.close(0), capture_output=True, timeout=3)
        self.assertEqual(p.returncode, 0)

    def test_inherited_sigchld_ignore(self):
        p = subprocess.run(self.command('pass'), preexec_fn=lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN), capture_output=True, timeout=3)
        self.assertEqual(p.returncode, 0)

    def test_late_observation_is_timeout(self):
        marker = self.root / 'ready'
        p = subprocess.Popen(self.command(f"import pathlib,time;pathlib.Path({str(marker)!r}).touch();time.sleep(.08)", 200), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(p)
        self.wait_file(marker, p)
        p.send_signal(signal.SIGSTOP)
        try:
            time.sleep(.3)
        finally:
            p.send_signal(signal.SIGCONT)
        p.communicate(timeout=3)
        self.assertEqual(p.returncode, 124)

    def test_missing_pidfd_fails_before_effect(self):
        marker = self.root / 'unexpected'
        helper = str(Path(BINARY).parent / 'tests' / 'no-pidfd')
        p = subprocess.run([helper] + self.command(f"import pathlib;pathlib.Path({str(marker)!r}).touch()"), capture_output=True, timeout=3)
        self.assertEqual(p.returncode, 125)
        self.assertFalse(marker.exists())

    def test_bad_arguments(self):
        for args in [[], ['run','0',str(self.lock),'--','/bin/true'], ['run','600001',str(self.lock),'--','/bin/true'], ['run','100','relative','--','/bin/true'], ['run','100',str(self.lock),'--','true']]:
            with self.subTest(args=args):
                self.assertEqual(subprocess.run([BINARY] + args, capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
