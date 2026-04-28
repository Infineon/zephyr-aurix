

import contextlib
import fcntl
import logging
import os
import shlex
import socket
import subprocess
import threading
import time
from pathlib import Path

from runners.core import RunnerCaps, ZephyrBinaryRunner

DEFAULT_WINIDEA_HOST = '192.168.1.2'
DEFAULT_REMOTE_DIR = 'D:/Parthiban'
DEFAULT_SSH_USER = 'bharathi'
DEFAULT_WATCH_SECONDS = 5
DEFAULT_LOCK_TIMEOUT = 600.0
DEFAULT_CONSOLE_HOST = '192.168.1.3'
DEFAULT_CONSOLE_SECONDS = 0

OPT_SYMBOL_FILE = '/IDE/System.Debug.Applications[0].SymbolFiles.File'
OPT_PROGRAM_FILE = '/IDE/System.Debug.SoCs[0].DLFs_Program.File'

@contextlib.contextmanager
def _winidea_lock(instance_id: str, timeout: float, log: logging.Logger):
    '''Serialize hardware access keyed by the winIDEA instance id.

    Lets two callers (e.g. ``west flash`` here and the NuttX
    ``tools/winidea/flash.sh`` in the sibling tree) race for the same DAP
    safely while still parallelizing flashes of *different* boards.
    '''
    runtime = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    if not os.path.isdir(runtime):
        runtime = '/tmp'
    path = f'{runtime}/winidea-{instance_id}.lock'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT, 0o600)
    waited = False
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            waited = True
            log.info('waiting for winIDEA lock %s', path)
            deadline = time.monotonic() + timeout
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(
                            f'timed out after {timeout:.0f}s waiting for '
                            f'{path}; another flasher is still holding it')
                    time.sleep(0.5)
        os.ftruncate(fd, 0)
        os.write(fd, f'{os.getpid()} {time.time():.0f}\n'.encode())
        if waited:
            log.info('acquired winIDEA lock %s', path)
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)

BOARD_PROFILES = {
    'kit_a3g_tc4d7_lite/tc4d7xp/cpu0': {
        'instance_id': 'com.tasking.winIDEA.instance.id-TC4D7',
        'remote_name': 'zephyr-tc4d7-cpu0.out',
        'console_port': 9001,
    },
    'kit_a2g_tc397xa_3v3_tft/tc397xp/cpu0': {
        'instance_id': 'com.tasking.winIDEA.instance.id-TC397',
        'remote_name': 'zephyr-tc397-cpu0.out',
        'console_port': 9000,
    },
    'kit_tc4x7_com_trb/tc4d7xp/cpu0': {
        'instance_id': 'com.tasking.winIDEA.instance.id-TC4D7 - Triboard',
        'remote_name': 'zephyr-tc4x7-com-cpu0.out',
        'console_port': 9002,
    },
}

class WinIDEABinaryRunner(ZephyrBinaryRunner):
    '''Flash AURIX targets via remote winIDEA + isystem.connect.'''

    def __init__(self, cfg, *, host, ssh_user, remote_dir, instance_id,
                 remote_name, watch, watch_seconds, lock_timeout,
                 capture_console, console_host, console_port,
                 console_seconds, console_log):
        super().__init__(cfg)
        self.host = host
        self.ssh_user = ssh_user
        self.remote_dir = remote_dir
        self.instance_id = instance_id
        self.remote_name = remote_name
        self.watch = watch
        self.watch_seconds = watch_seconds
        self.lock_timeout = lock_timeout
        self.capture_console = capture_console
        self.console_host = console_host
        self.console_port = console_port
        self.console_seconds = console_seconds
        self.console_log = console_log

    @classmethod
    def name(cls):
        return 'winidea'

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'flash'})

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument('--winidea-host', default=DEFAULT_WINIDEA_HOST,
                            help='IP of the Windows host running winIDEA '
                                 '(default: %(default)s)')
        parser.add_argument('--ssh-user', default=DEFAULT_SSH_USER,
                            help='SSH username on the Windows host '
                                 '(default: %(default)s); password from '
                                 'SSHPASS env var')
        parser.add_argument('--remote-dir', default=DEFAULT_REMOTE_DIR,
                            help='Remote directory to drop the ELF into '
                                 '(default: %(default)s)')
        parser.add_argument('--instance-id', default=None,
                            help='winIDEA instance id; defaults to the one '
                                 'baked in for the active board')
        parser.add_argument('--remote-name', default=None,
                            help='Filename to give the ELF on the remote '
                                 'side; defaults to the per-board value')
        parser.add_argument('--no-watch', dest='watch', action='store_false',
                            default=True,
                            help='Skip the post-run CPU-state poll that '
                                 'dumps registers/stack on a trap')
        parser.add_argument('--watch-seconds', type=int,
                            default=DEFAULT_WATCH_SECONDS,
                            help='How long to poll for an unexpected stop '
                                 'after resetAndRun (default: %(default)s)')

        parser.add_argument('--lock-timeout', type=float,
                            default=DEFAULT_LOCK_TIMEOUT,
                            help='Seconds to wait for the per-board winIDEA '
                                 'lock if another flasher (e.g. the NuttX '
                                 'flash.sh) holds it (default: %(default)s)')

        parser.add_argument('--no-capture-console', dest='capture_console',
                            action='store_false', default=True,
                            help='Skip auto-capture of the serial console to '
                                 '<build_dir>/console.log during the lock '
                                 'window')
        parser.add_argument('--console-host', default=DEFAULT_CONSOLE_HOST,
                            help='Lab host exposing the serial-over-TCP '
                                 'consoles (default: %(default)s)')
        parser.add_argument('--console-port', type=int, default=None,
                            help='Override the per-board TCP port for the '
                                 'serial console')
        parser.add_argument('--console-seconds', type=float,
                            default=DEFAULT_CONSOLE_SECONDS,
                            help='Capture the console for this many seconds '
                                 'after resetAndRun (0 = use --watch-seconds)')
        parser.add_argument('--console-log', default=None,
                            help='Path to write the captured console to '
                                 '(default: <build_dir>/console.log)')

    @staticmethod
    def _board_target_from_build(build_dir):
        if not build_dir:
            return None
        cache = Path(build_dir) / 'CMakeCache.txt'
        if not cache.is_file():
            return None
        for line in cache.read_text().splitlines():
            if line.startswith('CACHED_BOARD:') or line.startswith('BOARD:'):
                return line.split('=', 1)[1].strip()
        return None

    @classmethod
    def do_create(cls, cfg, args):
        board_target = (os.environ.get('BOARD_TARGET')
                        or cls._board_target_from_build(cfg.build_dir)
                        or (cfg.board_dir and Path(cfg.board_dir).name))
        profile = BOARD_PROFILES.get(board_target, {})
        instance_id = args.instance_id or profile.get('instance_id')
        remote_name = args.remote_name or profile.get('remote_name')
        if not instance_id:
            raise RuntimeError(
                f'no winIDEA instance id known for board {board_target!r}; '
                f'pass --instance-id or extend BOARD_PROFILES')
        if not remote_name:
            elf = cfg.elf_file and Path(cfg.elf_file).name
            remote_name = elf or 'zephyr.out'
        console_port = args.console_port or profile.get('console_port')
        console_log = args.console_log
        if console_log is None and cfg.build_dir:
            console_log = str(Path(cfg.build_dir) / 'console.log')
        return cls(cfg,
                   host=args.winidea_host,
                   ssh_user=args.ssh_user,
                   remote_dir=args.remote_dir,
                   instance_id=instance_id,
                   remote_name=remote_name,
                   watch=args.watch,
                   watch_seconds=args.watch_seconds,
                   lock_timeout=args.lock_timeout,
                   capture_console=args.capture_console,
                   console_host=args.console_host,
                   console_port=console_port,
                   console_seconds=args.console_seconds,
                   console_log=console_log)

    def do_run(self, command, **kwargs):
        if command != 'flash':
            raise RuntimeError(f'winidea runner does not support {command!r}')
        try:
            import isystem.connect as ic
        except ImportError as exc:
            raise RuntimeError(
                'isystem.connect is not importable; install with '
                '`pip install isystem.connect`') from exc

        if not self.cfg.elf_file:
            raise RuntimeError('no ELF available for flashing; check the '
                               'build directory')
        elf = Path(self.cfg.elf_file)
        if not elf.is_file():
            raise RuntimeError(f'ELF not found: {elf}')

        remote_path = f'{self.remote_dir}/{self.remote_name}'
        remote_path_for_winidea = remote_path

        with _winidea_lock(self.instance_id, self.lock_timeout, self.logger):
            self._scp(elf, remote_path)
            self._winidea_flash(remote_path_for_winidea)

    def _scp(self, src: Path, dst_remote: str) -> None:
        if 'SSHPASS' not in os.environ:
            raise RuntimeError('SSHPASS env var must hold the Windows '
                               'password for sshpass+scp')
        cmd = [
            'sshpass', '-e', 'scp',
            '-o', 'StrictHostKeyChecking=no',
            str(src),
            f'{self.ssh_user}@{self.host}:/{dst_remote}',
        ]
        self.logger.info('scp %s -> %s:%s', src.name, self.host, dst_remote)
        self.logger.debug('  %s', ' '.join(shlex.quote(c) for c in cmd))
        subprocess.run(cmd, check=True)

    def _winidea_flash(self, remote_elf: str) -> None:
        import isystem.connect as ic
        mgr = ic.ConnectionMgr()
        cfg = (ic.CConnectionConfig()
               .host(self.host)
               .instanceId(self.instance_id))
        cfg.start_existing()
        mgr.connect(cfg)
        if not mgr.isConnected():
            raise RuntimeError(
                f'could not attach to winIDEA instance {self.instance_id!r} '
                f'on {self.host}')
        self.logger.info('connected to winIDEA %s on %s (id=%s)',
                         mgr.getWinIDEAVersion(), self.host, self.instance_id)
        exec_ctrl = ic.CExecutionController(mgr)
        try:
            if exec_ctrl.getCPUStatus(False).isRunning():
                self.logger.info('CPU running -> stop()')
                try:
                    exec_ctrl.stop()
                except Exception as e:
                    self.logger.warning('stop() failed (%s); falling back to reset()', e)
                    exec_ctrl.reset()
                time.sleep(0.2)
            self._set_path(mgr, OPT_SYMBOL_FILE, remote_elf)
            self._set_path(mgr, OPT_PROGRAM_FILE, remote_elf)

            self.logger.info('downloading %s', remote_elf)
            t0 = time.time()
            ic.CDebugFacade(mgr).download()
            self.logger.info('download ok in %.1fs', time.time() - t0)

            with self._console_recorder() as console:
                if console is not None:
                    self.logger.info('capturing console %s:%d -> %s',
                                     self.console_host, self.console_port,
                                     self.console_log)
                self.logger.info('resetAndRun')
                exec_ctrl.resetAndRun()
                time.sleep(0.4)
                running = exec_ctrl.getCPUStatus(False).isRunning()
                self.logger.info('CPU running: %s', running)

                if self.watch and self.watch_seconds > 0:
                    self._watch_for_trap(mgr, exec_ctrl, ic)

                if console is not None:
                    seconds = self.console_seconds or self.watch_seconds or 5
                    elapsed = time.time() - console.started_at
                    remaining = seconds - elapsed
                    if remaining > 0:
                        time.sleep(remaining)
        finally:
            mgr.disconnect_keep()

    @contextlib.contextmanager
    def _console_recorder(self):
        if not self.capture_console or not self.console_port \
                or not self.console_log:
            yield None
            return
        try:
            sock = socket.create_connection(
                (self.console_host, self.console_port), timeout=5)
        except OSError as e:
            self.logger.warning('console capture disabled: %s', e)
            yield None
            return
        sock.settimeout(0.5)
        Path(self.console_log).parent.mkdir(parents=True, exist_ok=True)
        log_fp = open(self.console_log, 'wb', buffering=0)
        stop = threading.Event()

        class _Recorder:
            started_at = time.time()

        def _pump():
            while not stop.is_set():
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                log_fp.write(chunk)

        t = threading.Thread(target=_pump, daemon=True)
        t.start()
        try:
            yield _Recorder
        finally:
            stop.set()
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
            t.join(timeout=2)
            log_fp.close()
            self._tail_console(self.console_log)

    def _tail_console(self, path: str, lines: int = 20) -> None:
        try:
            data = Path(path).read_bytes()
        except OSError:
            return
        text = data.decode('utf-8', errors='replace').splitlines()
        if not text:
            return
        self.logger.info('console.log tail (%d/%d lines):',
                         min(lines, len(text)), len(text))
        for line in text[-lines:]:
            self.logger.info('| %s', line)

    def _set_path(self, mgr, opt_path: str, new_path: str) -> None:
        import isystem.connect as ic
        opt = ic.COptionController(mgr, opt_path)
        if opt.size() == 0:
            opt.add()
        entry = opt.at(0)
        cur = entry.get('Path')
        if cur != new_path:
            entry.set('Path', new_path)
            self.logger.info('  %s[0].Path: %s -> %s', opt_path, cur, new_path)

    def _watch_for_trap(self, mgr, exec_ctrl, ic) -> None:
        deadline = time.time() + self.watch_seconds
        stopped = False
        while time.time() < deadline:
            time.sleep(0.5)
            if exec_ctrl.getCPUStatus(False).isStopped():
                stopped = True
                break
        if not stopped:
            self.logger.info('CPU still running after %ds (no trap observed)',
                             self.watch_seconds)
            return

        self.logger.error('!! CPU halted unexpectedly within %ds of run',
                          self.watch_seconds)
        data = ic.CDataController(mgr)
        for reg in ('PC', 'PSW', 'PCXI', 'A10', 'A11'):
            try:
                v = data.readRegister(ic.IConnectDebug.fRealTime, reg)
                self.logger.error('  %-4s = 0x%08x',
                                  reg, v.getInt() & 0xFFFFFFFF)
            except Exception as e:
                self.logger.error('  %-4s read failed: %s', reg, e)

        try:
            frames = ic.StackFrameVector()
            data.getStackFrames(False, True, frames)
            self.logger.error('Call stack (%d frames):', len(frames))
            for i, frame in enumerate(frames):
                fn = frame.getFunction().getName() or '?'
                self.logger.error('  #%d  0x%08x  %s  %s:%d', i,
                                  frame.getAddress(), fn,
                                  frame.getFileName() or '?',
                                  frame.getLineNumber())
        except Exception as e:
            self.logger.error('  stack frame read failed: %s', e)
