"""Stage 03 端到端测试：真实 KVM + bzImage + initramfs，经串口与 BusyBox shell 交互。

需要 Linux、可读写的 /dev/kvm 和一个 x86 bzImage（BZIMAGE_PATH，默认 images/bzImage）。
条件不满足时跳过；若显式设置了 BZIMAGE_PATH 却无法运行，则判为失败。
"""

import hashlib
import os
import pty
import signal
import subprocess
import termios
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VMM = os.path.join(ROOT, "bin", "linux_boot")
INITRAMFS = os.path.join(ROOT, "build", "initramfs.cpio.gz")
KERNEL = os.environ.get("BZIMAGE_PATH") or os.path.join(ROOT, "images", "bzImage")
PROMPT = b"/ # "
BOOT_TIMEOUT = 60


def environment_problem():
    if os.uname().sysname != "Linux":
        return "Linux/KVM required"
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        return "/dev/kvm is unavailable"
    if not os.access(KERNEL, os.R_OK):
        return "bzImage is unreadable: " + KERNEL
    return None


def setUpModule():
    problem = environment_problem()
    if problem:
        if os.environ.get("BZIMAGE_PATH"):
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)
    subprocess.run(["make", "-s", "-C", ROOT, "all", "initramfs"], check=True)


class Guest:
    """启动 VMM，并在后台收集客户机串口输出，提供 expect/send。"""

    def __init__(self, tty=False, env=None):
        full_env = dict(os.environ)
        full_env.update(env or {})
        self.master = None
        if tty:
            self.master, slave = pty.openpty()
            self.saved_termios = termios.tcgetattr(slave)
            self.slave = slave
            stdin = stdout = slave
        else:
            stdin = stdout = subprocess.PIPE
        self.proc = subprocess.Popen(
            [VMM, KERNEL], cwd=ROOT, env=full_env, stdin=stdin, stdout=stdout,
            stderr=subprocess.PIPE)
        self.output = b""
        self.cursor = 0
        self.stderr = b""
        self.lock = threading.Condition()
        out_fd = self.master if tty else self.proc.stdout.fileno()
        self.reader = threading.Thread(target=self._read, args=(out_fd,), daemon=True)
        self.reader.start()
        self.err_reader = threading.Thread(target=self._read_err, daemon=True)
        self.err_reader.start()

    def _read(self, fd):
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if not data:
                break
            with self.lock:
                self.output += data
                self.lock.notify_all()

    def _read_err(self):
        for line in self.proc.stderr:
            with self.lock:
                self.stderr += line

    def expect(self, pattern, timeout=15):
        """等待 pattern 出现在上次匹配之后的输出中，返回其间的输出。"""
        deadline = time.monotonic() + timeout
        with self.lock:
            while True:
                index = self.output.find(pattern, self.cursor)
                if index >= 0:
                    chunk = self.output[self.cursor:index]
                    self.cursor = index + len(pattern)
                    return chunk
                remaining = deadline - time.monotonic()
                if remaining <= 0 or (self.proc.poll() is not None and
                                      not self.reader.is_alive()):
                    raise AssertionError(
                        "timed out waiting for %r; tail of output:\n%s\nstderr:\n%s" % (
                            pattern, self.output[-2000:].decode(errors="replace"),
                            self.stderr[-2000:].decode(errors="replace")))
                self.lock.wait(min(remaining, 0.2))

    def send(self, data):
        if self.master is not None:
            os.write(self.master, data)
        else:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()

    def run(self, command, marker):
        """执行一条命令，返回命令回显之后、marker 之前的输出。"""
        self.send(command + (b"\r" if self.master is not None else b"\n"))
        return self.expect(marker)

    def wait(self, timeout=20):
        try:
            code = self.proc.wait(timeout)
        finally:
            self.close()
        self.err_reader.join(5)
        return code

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.master is not None:
            os.close(self.slave)
            self.slave = None
            self.reader.join(5)
            os.close(self.master)
            self.master = None
        else:
            self.reader.join(5)
            for pipe in (self.proc.stdin, self.proc.stdout):
                if pipe and not pipe.closed:
                    try:
                        pipe.close()
                    except BrokenPipeError:
                        pass
        self.err_reader.join(5)
        if not self.proc.stderr.closed:
            self.proc.stderr.close()

    def metric_reason(self):
        for line in self.stderr.decode(errors="replace").splitlines():
            if "stage=vcpu_exit" in line:
                return line.split("reason=")[1].split()[0]
        return None


class InitramfsShellTest(unittest.TestCase):
    def boot(self, **kwargs):
        guest = Guest(**kwargs)
        self.addCleanup(guest.close)
        guest.expect(b"Run /init as init process", BOOT_TIMEOUT)
        guest.expect(b"hello from mini sandbox", BOOT_TIMEOUT)
        guest.expect(PROMPT, BOOT_TIMEOUT)
        return guest

    def test_shell_runs_commands_and_powers_off(self):
        guest = self.boot()
        log = guest.output.decode(errors="replace")
        self.assertIn("ACPI: PM: (supports S0 S5)", log)
        self.assertRegex(log, r"00:00: ttyS0 at I/O 0x3f8 .* is a 16550A")

        # 命令回显里是 $((6*7))，只有 shell 真正执行后才会出现 MARK 42。
        guest.run(b"echo MARK $((6*7))", b"MARK 42")
        mounts = guest.run(b"awk '{print \"M:\" $3 \":\" $2}' /proc/mounts; echo END$((1))",
                           b"END1")
        for entry in (b"M:proc:/proc", b"M:sysfs:/sys", b"M:devtmpfs:/dev", b"M:tmpfs:/tmp"):
            self.assertIn(entry, mounts)
        guest.run(b"echo T$((1))MP > /tmp/probe; cat /tmp/probe", b"T1MP")
        guest.run(b"test -c /dev/null && test -c /dev/ttyS0 && echo DEV$((2))OK", b"DEV2OK")
        guest.run(b"echo PID1:$(cat /proc/1/comm)", b"PID1:init")

        guest.send(b"poweroff -f\n")
        self.assertEqual(guest.wait(), 0)
        self.assertEqual(guest.metric_reason(), "poweroff")
        self.assertIn(b"Stage 03 completed", guest.stderr)

    def test_large_input_is_not_dropped(self):
        # 13 KiB 远超 4 KiB 的 VMM 接收队列和 16 字节 UART FIFO，校验流控不丢字节。
        guest = self.boot()
        lines = [("%04d" % i + "abcdefghijklmnopqrstuvwxyz0123456789" * 2)[:64]
                 for i in range(200)]
        blob = "".join(line + "\n" for line in lines).encode()
        guest.send(b"cat > /tmp/blob <<'EOF'\n" + blob + b"EOF\n")
        guest.expect(PROMPT, 60)
        digest = hashlib.md5(blob).hexdigest().encode()
        out = guest.run(b"echo SIZE:$(wc -c < /tmp/blob) MD$((5)):$(md5sum < /tmp/blob)",
                        b"MD5:")
        self.assertIn(b"SIZE:%d" % len(blob), out)
        self.assertEqual(guest.expect(b" ").strip(), digest)
        guest.send(b"poweroff -f\n")
        self.assertEqual(guest.wait(), 0)

    def test_shell_exit_resets_guest(self):
        guest = self.boot()
        guest.send(b"exit\n")
        guest.expect(b"mini sandbox shell exited")
        self.assertEqual(guest.wait(), 0)
        self.assertEqual(guest.metric_reason(), "reset")

    def test_terminal_session(self):
        guest = self.boot(tty=True)
        # 有控制终端时 tty 输出串口设备，Ctrl-C 能打断前台命令。
        guest.run(b"tty; echo TTY$((3))", b"TTY3")
        self.assertIn(b"/dev/ttyS0", guest.output)
        guest.send(b"sleep 100; echo NOT$((4))REACHED\r")
        guest.expect(b"sleep 100")
        time.sleep(0.5)
        start = time.monotonic()
        guest.send(b"\x03")
        guest.expect(PROMPT, 5)
        self.assertLess(time.monotonic() - start, 5)
        guest.run(b"echo AFTER$((5))", b"AFTER5")
        self.assertNotIn(b"NOT4REACHED", guest.output)

        # Ctrl-A x 退出 VMM，终端恢复原来的模式。
        slave = guest.slave
        guest.send(b"\x01x")
        self.assertEqual(guest.proc.wait(20), 0)
        self.assertEqual(termios.tcgetattr(slave), guest.saved_termios)
        guest.wait()
        self.assertEqual(guest.metric_reason(), "host_stop")

    def test_sigterm_stops_vmm(self):
        guest = self.boot()
        guest.proc.send_signal(signal.SIGTERM)
        self.assertEqual(guest.wait(), 0)
        self.assertEqual(guest.metric_reason(), "host_stop")

    def test_missing_initramfs_is_rejected(self):
        guest = Guest(env={"INITRAMFS_PATH": "/nonexistent/initramfs.cpio.gz"})
        self.addCleanup(guest.close)
        self.assertNotEqual(guest.wait(), 0)
        self.assertIn(b"stage=failed_initramfs_load", guest.stderr)


if __name__ == "__main__":
    unittest.main()
