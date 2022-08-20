# SPDX-License-Identifier: GPL-2.0+
# Copyright 2019 Google LLC
# Written by Simon Glass <sjg@chromium.org>
#
# Entry-type module for Intel Firmware Support Package binary blob (T section)
#

from binman.entry import Entry
from binman.etype.blob import Entry_blob

class Entry_intel_fsp_t(Entry_blob):
    """Entry containing Intel Firmware Support Package (FSP) temp ram init

    Properties / Entry arguments:
        - filename: Filename of file to read into entry

    This file contains a binary blob which is used on some devices to set up
    temporary memory (Cache-as-RAM or CAR). U-Boot executes this code in TPL so
    that it has access to memory for its stack and initial storage.

    An example filename is 'fsp_t.bin'

    See README.x86 for information about x86 binary blobs.
    """
    def __init__(self, section, etype, node):
        Entry_blob.__init__(self, section, etype, node)
