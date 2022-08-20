# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2016 Google, Inc
# Written by Simon Glass <sjg@chromium.org>
#
# Entry-type module for Intel Firmware Image Table
#

import struct

from binman.etype.blob import Entry_blob

class Entry_intel_fit(Entry_blob):
    """Intel Firmware Image Table (FIT)

    This entry contains a dummy FIT as required by recent Intel CPUs. The FIT
    contains information about the firmware and microcode available in the
    image.

    At present binman only supports a basic FIT with no microcode.
    """
    def __init__(self, section, etype, node):
        Entry_blob.__init__(self, section, etype, node)

    def ReadNode(self):
        """Force 16-byte alignment as required by FIT pointer"""
        Entry_blob.ReadNode(self)
        self.align = 16

    def ObtainContents(self):
        data = struct.pack('<8sIHBB', b'_FIT_   ', 1, 0x100, 0x80, 0x7d)
        self.SetContents(data)
        return True
