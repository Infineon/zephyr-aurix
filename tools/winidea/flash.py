#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import fcntl
import logging
import os
import shlex
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

OPT_SYMBOL_FILE = '/IDE/System.Debug.Applications[0].SymbolFiles.File'
OPT_PROGRAM_FILE = '/IDE/System.Debug.SoCs[0].DLFs_Program.File'
DEFAULT_CONSOLE_HOST = '192.168.1.3'

log = logging.getLogger('winidea.flash')

@contextlib.contextmanager
def winidea_lock(instance_id: str, timeout: float = 600.0):
    """Serialize hardware access keyed by the winIDEA instance id.

    Lets two callers (e.g. west flash from one tree and flash.sh from
    another) safely race for the same DAP without stepping on each other,
    while still parallelizing flashes of *different* boards.
    """
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
                        raise SystemExit(
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

def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('elf', help='Path to the local ELF (or .out) to flash')
    p.add_argument('--host', default=os.environ.get('WINIDEA_HOST', '192.168.1.2'),
                   help='IP of the Windows host running winIDEA '
                        '(default: %(default)s; env WINIDEA_HOST)')
    p.add_argument('--ssh-user', default=os.environ.get('WINIDEA_SSH_USER', 'bharathi'),
                   help='SSH user on the Windows host (default: %(default)s; '
                        'env WINIDEA_SSH_USER). Password from SSHPASS env var.')
    p.add_argument('--remote-dir', default=os.environ.get('WINIDEA_REMOTE_DIR',
                                                          'D:/Parthiban'),
                   help='Remote directory to drop the ELF into '
                        '(default: %(default)s; env WINIDEA_REMOTE_DIR)')
    p.add_argument('--remote-name', default=None,
                   help='Filename to give the ELF on the remote side '
                        '(default: same as local basename)')
    p.add_argument('--instance-id', required=True,
                   help='winIDEA instance id, e.g. '
                        'com.tasking.winIDEA.instance.id-TC4D7')
    p.add_argument('--no-scp', action='store_true',
                   help='Skip the scp step (assume the ELF is already at '
                        '<remote-dir>/<remote-name>)')
    p.add_argument('--no-run', action='store_true',
                   help='Download but do not resetAndRun afterwards')
    p.add_argument('--watch-seconds', type=int, default=5,
                   help='Poll the CPU for unexpected stops for N seconds after '
                        'resetAndRun and dump regs/stack on a trap '
                        '(default: %(default)s, set 0 to disable)')
    p.add_argument('--no-capture-console', dest='capture_console',
                   action='store_false', default=True,
                   help='Skip auto-capture of the serial console during the '
                        'lock window')
    p.add_argument('--console-host', default=os.environ.get(
                       'WINIDEA_CONSOLE_HOST', DEFAULT_CONSOLE_HOST),
                   help='Lab host exposing the serial-over-TCP consoles '
                        '(default: %(default)s; env WINIDEA_CONSOLE_HOST)')
    p.add_argument('--console-port', type=int, default=None,
                   help='TCP port on --console-host for this board\'s serial '
                        'console (no default; required if capture is on)')
    p.add_argument('--console-seconds', type=float, default=0.0,
                   help='Capture the console for this many seconds '
                        '(0 = use --watch-seconds)')
    p.add_argument('--console-log', default=None,
                   help='Path to write the captured console to '
                        '(default: ./console-<instance-id>.log next to cwd)')
    p.add_argument('-v', '--verbose', action='store_true')
    return p.parse_args(argv)

def scp(local: Path, host: str, user: str, remote: str) -> None:
    if 'SSHPASS' not in os.environ:
        raise SystemExit('SSHPASS env var must hold the Windows password '
                         'for sshpass+scp')
    cmd = ['sshpass', '-e', 'scp',
           '-o', 'StrictHostKeyChecking=no',
           str(local),
           f'{user}@{host}:/{remote}']
    log.info('scp %s -> %s:%s', local.name, host, remote)
    log.debug('  %s', ' '.join(shlex.quote(c) for c in cmd))
    subprocess.run(cmd, check=True)

def set_path(mgr, opt_path: str, new_path: str) -> None:
    import isystem.connect as ic
    opt = ic.COptionController(mgr, opt_path)
    if opt.size() == 0:
        opt.add()
    entry = opt.at(0)
    cur = entry.get('Path')
    if cur != new_path:
        entry.set('Path', new_path)
        log.info('  %s[0].Path: %s -> %s', opt_path, cur, new_path)

def watch_for_trap(mgr, watch_seconds: int) -> bool:
    import isystem.connect as ic
    exec_ctrl = ic.CExecutionController(mgr)
    deadline = time.time() + watch_seconds
    while time.time() < deadline:
        time.sleep(0.5)
        if exec_ctrl.getCPUStatus(False).isStopped():
            break
    else:
        log.info('CPU still running after %ds (no trap observed)', watch_seconds)
        return True

    log.error('!! CPU halted unexpectedly within %ds of run', watch_seconds)
    data = ic.CDataController(mgr)
    for reg in ('PC', 'PSW', 'PCXI', 'A10', 'A11'):
        try:
            v = data.readRegister(ic.IConnectDebug.fRealTime, reg)
            log.error('  %-4s = 0x%08x', reg, v.getInt() & 0xFFFFFFFF)
        except Exception as e:
            log.error('  %-4s read failed: %s', reg, e)
    try:
        frames = ic.StackFrameVector()
        data.getStackFrames(False, True, frames)
        log.error('Call stack (%d frames):', len(frames))
        for i, frame in enumerate(frames):
            fn = frame.getFunction().getName() or '?'
            log.error('  #%d  0x%08x  %s  %s:%d', i,
                      frame.getAddress(), fn,
                      frame.getFileName() or '?',
                      frame.getLineNumber())
    except Exception as e:
        log.error('  stack frame read failed: %s', e)
    return False

@contextlib.contextmanager
def console_recorder(host, port, log_path, capture):
    """Open a TCP-as-serial reader and copy bytes into log_path while held.

    Held inside the per-board flock so two parallel testers do not race
    on the same /dev/ttyUSB. If the connection cannot be opened, capture
    is downgraded to a warning and the with-block continues without it.
    """
    if not capture or not port or not log_path:
        yield None
        return
    try:
        sock = socket.create_connection((host, port), timeout=5)
    except OSError as e:
        log.warning('console capture disabled: %s', e)
        yield None
        return
    sock.settimeout(0.5)
    Path(log_path).parent.mkdir(parents=True, exist_ok=True)
    fp = open(log_path, 'wb', buffering=0)
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
            fp.write(chunk)

    t = threading.Thread(target=_pump, daemon=True)
    t.start()
    log.info('capturing console %s:%d -> %s', host, port, log_path)
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
        fp.close()
        try:
            tail = Path(log_path).read_bytes().decode('utf-8',
                                                      errors='replace')
            lines = tail.splitlines()
            shown = lines[-20:]
            log.info('console.log tail (%d/%d lines):',
                     len(shown), len(lines))
            for line in shown:
                log.info('| %s', line)
        except OSError:
            pass

def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format='%(name)s: %(message)s',
    )

    elf = Path(args.elf)
    if not args.no_scp and not elf.is_file():
        raise SystemExit(f'ELF not found: {elf}')

    remote_name = args.remote_name or elf.name
    remote_path = f'{args.remote_dir}/{remote_name}'

    try:
        import isystem.connect as ic
    except ImportError:
        raise SystemExit("isystem.connect not importable; "
                         "pip install --user isystem.connect")

    with winidea_lock(args.instance_id):
        if not args.no_scp:
            scp(elf, args.host, args.ssh_user, remote_path)

        mgr = ic.ConnectionMgr()
        cfg = (ic.CConnectionConfig()
               .host(args.host)
               .instanceId(args.instance_id))
        cfg.start_existing()
        mgr.connect(cfg)
        if not mgr.isConnected():
            raise SystemExit(f'could not attach to winIDEA instance '
                             f'{args.instance_id!r} on {args.host}')
        log.info('connected to winIDEA on %s (id=%s)',
                 args.host, args.instance_id)
        try:
            exec_ctrl = ic.CExecutionController(mgr)
            if exec_ctrl.getCPUStatus(False).isRunning():
                log.info('CPU running -> stop()')
                try:
                    exec_ctrl.stop()
                except Exception as e:
                    log.warning('stop() failed (%s); falling back to reset()', e)
                    exec_ctrl.reset()
                time.sleep(0.2)

            set_path(mgr, OPT_SYMBOL_FILE, remote_path)
            set_path(mgr, OPT_PROGRAM_FILE, remote_path)

            log.info('downloading %s', remote_path)
            t0 = time.time()
            ic.CDebugFacade(mgr).download()
            log.info('download ok in %.1fs', time.time() - t0)

            if args.no_run:
                log.info('--no-run: leaving CPU stopped')
                return 0

            console_log = args.console_log or (
                f'./console-{args.instance_id}.log')
            with console_recorder(args.console_host, args.console_port,
                                  console_log, args.capture_console) as rec:
                log.info('resetAndRun')
                exec_ctrl.resetAndRun()
                time.sleep(0.4)
                log.info('CPU running: %s',
                         exec_ctrl.getCPUStatus(False).isRunning())

                rc = 0
                if args.watch_seconds > 0:
                    if not watch_for_trap(mgr, args.watch_seconds):
                        rc = 2

                if rec is not None:
                    seconds = (args.console_seconds
                               or args.watch_seconds or 5)
                    elapsed = time.time() - rec.started_at
                    remaining = seconds - elapsed
                    if remaining > 0:
                        time.sleep(remaining)
                return rc
        finally:
            mgr.disconnect_keep()

if __name__ == '__main__':
    sys.exit(main())
