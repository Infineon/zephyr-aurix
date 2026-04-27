
import importlib
import logging

from runners.core import MissingProgram, ZephyrBinaryRunner

_logger = logging.getLogger('runners')

def _import_runner_module(runner_name):
    try:
        importlib.import_module(f'runners.{runner_name}')
    except ImportError as ie:
        _logger.warning(f'The module for runner "{runner_name}" '
                        f'could not be imported ({ie}). This most likely '
                        'means it is not handling its dependencies properly. '
                        'Please report this to the zephyr developers.')

_names = [
    'amebaflash',
    'bflb_mcu_tool',
    'blackmagicprobe',
    'bossac',
    'canopen_program',
    'dediprog',
    'dfu',
    'ecpprog',
    'esp32',
    'ezflashcli',
    'gd32isp',
    'hifive1',
    'intel_adsp',
    'intel_cyclonev',
    'jlink',
    'linkserver',
    'lldbac',
    'mdb',
    'minichlink',
    'misc',
    'mpcli',
    'native',
    'nrfjprog',
    'nrfutil',
    'nsim',
    'nxp_s32dbg',
    'openocd',
    'probe_rs',
    'pyocd',
    'qemu',
    'renode',
    'renode-robot',
    'rfp',
    'rtsflash',
    'sftool',
    'silabs_commander',
    'spi_burn',
    'spsdk',
    'stlink_gdbserver',
    'stm32cubeprogrammer',
    'stm32flash',
    'sy1xx',
    'teensy',
    'trace32',
    'uf2',
    'wchisp',
    'winidea',
    'wlink',
    'xsdb',
    'xtensa',
]

for _name in _names:
    _import_runner_module(_name)

def get_runner_cls(runner):
    '''Get a runner's class object, given its name.'''
    for cls in ZephyrBinaryRunner.get_runners():
        if cls.name() == runner:
            return cls
    raise ValueError(f'unknown runner "{runner}"')

__all__ = ['ZephyrBinaryRunner', 'MissingProgram', 'get_runner_cls']
