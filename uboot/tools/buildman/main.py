#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
#
# Copyright (c) 2012 The Chromium OS Authors.
#

"""See README for more information"""

import doctest
import multiprocessing
import os
import re
import sys
import unittest

# Bring in the patman libraries
our_path = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(1, os.path.join(our_path, '..'))

# Our modules
from buildman import board
from buildman import bsettings
from buildman import builder
from buildman import cmdline
from buildman import control
from buildman import toolchain
from patman import patchstream
from patman import gitutil
from patman import terminal

def RunTests(skip_net_tests):
    import func_test
    import test
    import doctest

    result = unittest.TestResult()
    for module in ['buildman.toolchain', 'patman.gitutil']:
        suite = doctest.DocTestSuite(module)
        suite.run(result)

    sys.argv = [sys.argv[0]]
    if skip_net_tests:
        test.use_network = False
    for module in (test.TestBuild, func_test.TestFunctional):
        suite = unittest.TestLoader().loadTestsFromTestCase(module)
        suite.run(result)

    print(result)
    for test, err in result.errors:
        print(err)
    for test, err in result.failures:
        print(err)


options, args = cmdline.ParseArgs()

# Run our meagre tests
if options.test:
    RunTests(options.skip_net_tests)

# Build selected commits for selected boards
else:
    bsettings.Setup(options.config_file)
    ret_code = control.DoBuildman(options, args)
    sys.exit(ret_code)
