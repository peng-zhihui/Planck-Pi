# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2017 Google, Inc
# Written by Simon Glass <sjg@chromium.org>
#
# Test for the elf module

import os
import shutil
import sys
import tempfile
import unittest

from binman import elf
from patman import command
from patman import test_util
from patman import tools
from patman import tout

binman_dir = os.path.dirname(os.path.realpath(sys.argv[0]))


class FakeEntry:
    """A fake Entry object, usedfor testing

    This supports an entry with a given size.
    """
    def __init__(self, contents_size):
        self.contents_size = contents_size
        self.data = tools.GetBytes(ord('a'), contents_size)

    def GetPath(self):
        return 'entry_path'


class FakeSection:
    """A fake Section object, used for testing

    This has the minimum feature set needed to support testing elf functions.
    A LookupSymbol() function is provided which returns a fake value for amu
    symbol requested.
    """
    def __init__(self, sym_value=1):
        self.sym_value = sym_value

    def GetPath(self):
        return 'section_path'

    def LookupSymbol(self, name, weak, msg, base_addr):
        """Fake implementation which returns the same value for all symbols"""
        return self.sym_value


def BuildElfTestFiles(target_dir):
    """Build ELF files used for testing in binman

    This compiles and links the test files into the specified directory. It the
    Makefile and source files in the binman test/ directory.

    Args:
        target_dir: Directory to put the files into
    """
    if not os.path.exists(target_dir):
        os.mkdir(target_dir)
    testdir = os.path.join(binman_dir, 'test')

    # If binman is involved from the main U-Boot Makefile the -r and -R
    # flags are set in MAKEFLAGS. This prevents this Makefile from working
    # correctly. So drop any make flags here.
    if 'MAKEFLAGS' in os.environ:
        del os.environ['MAKEFLAGS']
    tools.Run('make', '-C', target_dir, '-f',
              os.path.join(testdir, 'Makefile'), 'SRC=%s/' % testdir)


class TestElf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._indir = tempfile.mkdtemp(prefix='elf.')
        tools.SetInputDirs(['.'])
        BuildElfTestFiles(cls._indir)

    @classmethod
    def tearDownClass(cls):
        if cls._indir:
            shutil.rmtree(cls._indir)

    @classmethod
    def ElfTestFile(cls, fname):
        return os.path.join(cls._indir, fname)

    def testAllSymbols(self):
        """Test that we can obtain a symbol from the ELF file"""
        fname = self.ElfTestFile('u_boot_ucode_ptr')
        syms = elf.GetSymbols(fname, [])
        self.assertIn('.ucode', syms)

    def testRegexSymbols(self):
        """Test that we can obtain from the ELF file by regular expression"""
        fname = self.ElfTestFile('u_boot_ucode_ptr')
        syms = elf.GetSymbols(fname, ['ucode'])
        self.assertIn('.ucode', syms)
        syms = elf.GetSymbols(fname, ['missing'])
        self.assertNotIn('.ucode', syms)
        syms = elf.GetSymbols(fname, ['missing', 'ucode'])
        self.assertIn('.ucode', syms)

    def testMissingFile(self):
        """Test that a missing file is detected"""
        entry = FakeEntry(10)
        section = FakeSection()
        with self.assertRaises(ValueError) as e:
            syms = elf.LookupAndWriteSymbols('missing-file', entry, section)
        self.assertIn("Filename 'missing-file' not found in input path",
                      str(e.exception))

    def testOutsideFile(self):
        """Test a symbol which extends outside the entry area is detected"""
        entry = FakeEntry(10)
        section = FakeSection()
        elf_fname = self.ElfTestFile('u_boot_binman_syms')
        with self.assertRaises(ValueError) as e:
            syms = elf.LookupAndWriteSymbols(elf_fname, entry, section)
        self.assertIn('entry_path has offset 4 (size 8) but the contents size '
                      'is a', str(e.exception))

    def testMissingImageStart(self):
        """Test that we detect a missing __image_copy_start symbol

        This is needed to mark the start of the image. Without it we cannot
        locate the offset of a binman symbol within the image.
        """
        entry = FakeEntry(10)
        section = FakeSection()
        elf_fname = self.ElfTestFile('u_boot_binman_syms_bad')
        self.assertEqual(elf.LookupAndWriteSymbols(elf_fname, entry, section),
                         None)

    def testBadSymbolSize(self):
        """Test that an attempt to use an 8-bit symbol are detected

        Only 32 and 64 bits are supported, since we need to store an offset
        into the image.
        """
        entry = FakeEntry(10)
        section = FakeSection()
        elf_fname =self.ElfTestFile('u_boot_binman_syms_size')
        with self.assertRaises(ValueError) as e:
            syms = elf.LookupAndWriteSymbols(elf_fname, entry, section)
        self.assertIn('has size 1: only 4 and 8 are supported',
                      str(e.exception))

    def testNoValue(self):
        """Test the case where we have no value for the symbol

        This should produce -1 values for all thress symbols, taking up the
        first 16 bytes of the image.
        """
        entry = FakeEntry(24)
        section = FakeSection(sym_value=None)
        elf_fname = self.ElfTestFile('u_boot_binman_syms')
        syms = elf.LookupAndWriteSymbols(elf_fname, entry, section)
        self.assertEqual(tools.GetBytes(255, 20) + tools.GetBytes(ord('a'), 4),
                                                                  entry.data)

    def testDebug(self):
        """Check that enabling debug in the elf module produced debug output"""
        try:
            tout.Init(tout.DEBUG)
            entry = FakeEntry(20)
            section = FakeSection()
            elf_fname = self.ElfTestFile('u_boot_binman_syms')
            with test_util.capture_sys_output() as (stdout, stderr):
                syms = elf.LookupAndWriteSymbols(elf_fname, entry, section)
            self.assertTrue(len(stdout.getvalue()) > 0)
        finally:
            tout.Init(tout.WARNING)

    def testMakeElf(self):
        """Test for the MakeElf function"""
        outdir = tempfile.mkdtemp(prefix='elf.')
        expected_text = b'1234'
        expected_data = b'wxyz'
        elf_fname = os.path.join(outdir, 'elf')
        bin_fname = os.path.join(outdir, 'bin')

        # Make an Elf file and then convert it to a fkat binary file. This
        # should produce the original data.
        elf.MakeElf(elf_fname, expected_text, expected_data)
        stdout = command.Output('objcopy', '-O', 'binary', elf_fname, bin_fname)
        with open(bin_fname, 'rb') as fd:
            data = fd.read()
        self.assertEqual(expected_text + expected_data, data)
        shutil.rmtree(outdir)

    def testDecodeElf(self):
        """Test for the MakeElf function"""
        if not elf.ELF_TOOLS:
            self.skipTest('Python elftools not available')
        outdir = tempfile.mkdtemp(prefix='elf.')
        expected_text = b'1234'
        expected_data = b'wxyz'
        elf_fname = os.path.join(outdir, 'elf')
        elf.MakeElf(elf_fname, expected_text, expected_data)
        data = tools.ReadFile(elf_fname)

        load = 0xfef20000
        entry = load + 2
        expected = expected_text + expected_data
        self.assertEqual(elf.ElfInfo(expected, load, entry, len(expected)),
                         elf.DecodeElf(data, 0))
        self.assertEqual(elf.ElfInfo(b'\0\0' + expected[2:],
                                     load, entry, len(expected)),
                         elf.DecodeElf(data, load + 2))
        shutil.rmtree(outdir)


if __name__ == '__main__':
    unittest.main()
