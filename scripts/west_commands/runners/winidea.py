

import os
import shlex
import subprocess
import time
from pathlib import Path

from runners.core import RunnerCaps, ZephyrBinaryRunner

DEFAULT_WINIDEA_HOST = '10.11.176.11'
DEFAULT_REMOTE_DIR = 'D:/Parthiban'
DEFAULT_SSH_USER = 'bharathi'
DEFAULT_WATCH_SECONDS = 5

OPT_SYMBOL_FILE = '/IDE/System.Debug.Applications[0].SymbolFiles.File'
OPT_PROGRAM_FILE = '/IDE/System.Debug.SoCs[0].DLFs_Program.File'

BOARD_PROFILES = {
    'kit_a3g_tc4d7_lite/tc4d7xp/cpu0': {
        'instance_id': 'com.tasking.winIDEA.instance.id-TC4D7',
        'remote_name': 'zephyr-tc4d7-cpu0.out',
    },
    'kit_a2g_tc397xa_3v3_tft/tc397xp/cpu0': {
        'instance_id': 'com.tasking.winIDEA.instance.id-TC397',
        'remote_name': 'zephyr-tc397-cpu0.out',
    },
}

class WinIDEABinaryRunner(ZephyrBinaryRunner):
    '''Flash AURIX targets via remote winIDEA + isystem.connect.'''

    def __init__(self, cfg, *, host, ssh_user, remote_dir, instance_id,
                 remote_name, watch, watch_seconds):
        super().__init__(cfg)
        self.host = host
        self.ssh_user = ssh_user
        self.remote_dir = remote_dir
        self.instance_id = instance_id
        self.remote_name = remote_name
        self.watch = watch
        self.watch_seconds = watch_seconds

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
        return cls(cfg,
                   host=args.winidea_host,
                   ssh_user=args.ssh_user,
                   remote_dir=args.remote_dir,
                   instance_id=instance_id,
                   remote_name=remote_name,
                   watch=args.watch,
                   watch_seconds=args.watch_seconds)

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
                exec_ctrl.stop()
                time.sleep(0.2)
            self._set_path(mgr, OPT_SYMBOL_FILE, remote_elf)
            self._set_path(mgr, OPT_PROGRAM_FILE, remote_elf)

            self.logger.info('downloading %s', remote_elf)
            t0 = time.time()
            ic.CDebugFacade(mgr).download()
            self.logger.info('download ok in %.1fs', time.time() - t0)

            self.logger.info('resetAndRun')
            exec_ctrl.resetAndRun()
            time.sleep(0.4)
            running = exec_ctrl.getCPUStatus(False).isRunning()
            self.logger.info('CPU running: %s', running)

            if self.watch and self.watch_seconds > 0:
                self._watch_for_trap(mgr, exec_ctrl, ic)
        finally:
            mgr.disconnect_keep()

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
