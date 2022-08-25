# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2015 Stephen Warren
# Copyright (c) 2015-2016, NVIDIA CORPORATION. All rights reserved.

# Logic to interact with the sandbox port of U-Boot, running as a sub-process.

import time
from u_boot_spawn import Spawn
from u_boot_console_base import ConsoleBase

class ConsoleSandbox(ConsoleBase):
    """Represents a connection to a sandbox U-Boot console, executed as a sub-
    process."""

    def __init__(self, log, config):
        """Initialize a U-Boot console connection.

        Args:
            log: A multiplexed_log.Logfile instance.
            config: A "configuration" object as defined in conftest.py.

        Returns:
            Nothing.
        """

        super(ConsoleSandbox, self).__init__(log, config, max_fifo_fill=1024)
        self.sandbox_flags = []

    def get_spawn(self):
        """Connect to a fresh U-Boot instance.

        A new sandbox process is created, so that U-Boot begins running from
        scratch.

        Args:
            None.

        Returns:
            A u_boot_spawn.Spawn object that is attached to U-Boot.
        """

        bcfg = self.config.buildconfig
        config_spl = bcfg.get('config_spl', 'n') == 'y'
        fname = '/spl/u-boot-spl' if config_spl else '/u-boot'
        print(fname)
        cmd = []
        if self.config.gdbserver:
            cmd += ['gdbserver', self.config.gdbserver]
        cmd += [
            self.config.build_dir + fname,
            '-v',
            '-d',
            self.config.dtb
        ]
        cmd += self.sandbox_flags
        return Spawn(cmd, cwd=self.config.source_dir)

    def restart_uboot_with_flags(self, flags):
        """Run U-Boot with the given command-line flags

        Args:
            flags: List of flags to pass, each a string

        Returns:
            A u_boot_spawn.Spawn object that is attached to U-Boot.
        """

        try:
            self.sandbox_flags = flags
            return self.restart_uboot()
        finally:
            self.sandbox_flags = []

    def kill(self, sig):
        """Send a specific Unix signal to the sandbox process.

        Args:
            sig: The Unix signal to send to the process.

        Returns:
            Nothing.
        """

        self.log.action('kill %d' % sig)
        self.p.kill(sig)

    def validate_exited(self):
        """Determine whether the sandbox process has exited.

        If required, this function waits a reasonable time for the process to
        exit.

        Args:
            None.

        Returns:
            Boolean indicating whether the process has exited.
        """

        p = self.p
        self.p = None
        for i in range(100):
            ret = not p.isalive()
            if ret:
                break
            time.sleep(0.1)
        p.close()
        return ret
