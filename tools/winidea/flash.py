#!/usr/bin/env python3

from __future__ import annotations

import argparse
import logging
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

OPT_SYMBOL_FILE = '/IDE/System.Debug.Applications[0].SymbolFiles.File'
OPT_PROGRAM_FILE = '/IDE/System.Debug.SoCs[0].DLFs_Program.File'

log = logging.getLogger('winidea.flash')

def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('elf', help='Path to the local ELF (or .out) to flash')
    p.add_argument('--host', default=os.environ.get('WINIDEA_HOST', '10.11.176.11'),
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

def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format='%(name)s: %(message)s',
    )

    elf = Path(args.elf)
    if not elf.is_file():
        raise SystemExit(f'ELF not found: {elf}')

    remote_name = args.remote_name or elf.name
    remote_path = f'{args.remote_dir}/{remote_name}'

    if not args.no_scp:
        scp(elf, args.host, args.ssh_user, remote_path)

    try:
        import isystem.connect as ic
    except ImportError:
        raise SystemExit("isystem.connect not importable; "
                         "pip install --user isystem.connect")

    mgr = ic.ConnectionMgr()
    cfg = (ic.CConnectionConfig()
           .host(args.host)
           .instanceId(args.instance_id))
    cfg.start_existing()
    mgr.connect(cfg)
    if not mgr.isConnected():
        raise SystemExit(f'could not attach to winIDEA instance '
                         f'{args.instance_id!r} on {args.host}')
    log.info('connected to winIDEA on %s (id=%s)', args.host, args.instance_id)
    try:
        exec_ctrl = ic.CExecutionController(mgr)
        if exec_ctrl.getCPUStatus(False).isRunning():
            log.info('CPU running -> stop()')
            exec_ctrl.stop()
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

        log.info('resetAndRun')
        exec_ctrl.resetAndRun()
        time.sleep(0.4)
        log.info('CPU running: %s', exec_ctrl.getCPUStatus(False).isRunning())

        if args.watch_seconds > 0:
            if not watch_for_trap(mgr, args.watch_seconds):
                return 2
        return 0
    finally:
        mgr.disconnect_keep()

if __name__ == '__main__':
    sys.exit(main())
