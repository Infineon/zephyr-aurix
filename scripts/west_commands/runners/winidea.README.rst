winidea runner
==============

A west ``flash`` runner for Infineon AURIX targets that drives an iSYSTEM
winIDEA process on a remote (typically Windows) host through the
``isystem.connect`` Python SDK.

Architecture
------------

::

   Linux (host running west)                     Windows (host running winIDEA)
   -------------------------                     ------------------------------
        west build  -->  zephyr.elf
                              |
        sshpass -e scp -------+---- ssh -------->  D:/Parthiban/zephyr-<board>.out
        isystem.connect (TCP) ------------------>  winIDEA instance (TCP 5315/5316)
                                                        |
                                                        v
                                                   miniWiggler (USB) --> AURIX kit

   Serial console:
        Linux <-- TCP socat <-- /dev/ttyUSBx on the lab "kural" host

One-time host setup
-------------------

On the **Linux** host running west:

.. code-block:: console

   pip install --user isystem.connect
   sudo apt install sshpass openssh-client
   export SSHPASS='<windows password>'

On **Windows**: run two ``winIDEA`` instances, one per board. The
runner identifies them by the *instance id* in their window titles
(e.g. ``com.tasking.winIDEA.instance.id-TC4D7``). Each instance must
have its hardware filter (Infineon DAP serial substring) and target
device set so that the corresponding miniWiggler attaches.

For per-board persistence, give each instance its own ``.xjrf``
workspace (e.g. ``ADS-TC4D7.xjrf``, ``ADS-TC397.xjrf``). The runner
itself doesn't save state on disconnect (it uses ``disconnect_keep``),
so a shared workspace also works, but a manual UI ``Save`` would
overwrite settings made for the other board.

On the **lab "kural" host** (where the boards' serial cables plug in),
keep two persistent ``socat`` servers running:

.. code-block:: console

   stty -F /dev/ttyUSB0 raw -echo speed 115200
   socat FILE:/dev/ttyUSB0,raw,echo=0,b115200 TCP-LISTEN:9000,reuseaddr,fork &
   stty -F /dev/ttyUSB1 raw -echo speed 115200
   socat FILE:/dev/ttyUSB1,raw,echo=0,b115200 TCP-LISTEN:9001,reuseaddr,fork &

Then any number of viewers can ``nc kural 9000`` (TC397) or
``nc kural 9001`` (TC4D7).

Daily use
---------

.. code-block:: console

   west build -b kit_a3g_tc4d7_lite/tc4d7xp/cpu0 samples/hello_world
   SSHPASS=... west flash

The default options come from ``BOARD_PROFILES`` in
``runners/winidea.py``. Override per invocation with
``--winidea-host``, ``--ssh-user``, ``--remote-dir``, ``--instance-id``,
``--remote-name``.

Crash / trap capture
--------------------

After ``resetAndRun`` the runner polls the CPU state for
``--watch-seconds`` (default 5). If the CPU halts unexpectedly during
that window (i.e. a trap fires before the test settles) it dumps
PC, PSW, PCXI, A10 (SP) and A11 (RA), plus the call stack, to the west
log. This is the same information the winIDEA UI shows in its Core
Registers / Callstack panes.

Pass ``--no-watch`` if you don't want post-flash polling (e.g. for a
``hello_world`` that's expected to run forever).

Adding a new board
------------------

Two steps:

1. Add an entry to ``BOARD_PROFILES`` in ``runners/winidea.py`` keyed
   by the board target triple (``<board>/<soc>/<cpu>``).

2. Drop a ``board.cmake`` in the board folder containing::

       board_set_flasher_ifnset(winidea)
       board_finalize_runner_args(winidea)

Pristine-build the application; ``runners.yaml`` will then list
``winidea`` as the default flash runner.
