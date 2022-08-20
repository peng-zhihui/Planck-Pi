Vim for Debian
---------------

1. The current Debian Vim scripts policy can be found in the vim-doc package
   under /usr/share/doc/vim-common and
   <http://pkg-vim.alioth.debian.org/vim-policy.html/>.

2. Before reporting bugs, check if the bug also exists if you run vim
   with "vim -u NONE -U NONE". If not, make sure that the "bug" is not
   a result of a setting in your ~/.vimrc before reporting it.

 -- Stefano Zacchiroli <zack@debian.org>   Mon, 10 Apr 2006 09:59:41 -0400

MzScheme Vim variant
--------------------

As requested by the current MzScheme maintainer (Ari Pollak <ari@debian.org>),
a vim-mzscheme variant is not being built.  The reasons stated are as follows:

  1) MzScheme does not build on many of Debian's supported architectures.

  2) The MzScheme package is not versioned based on the library.

  3) The MzScheme ABI changes with every upstream version.

 -- James Vega <jamessan@debian.org> Mon, 10  Apr  2006  09:48:25  -0400

Modeline support disabled by default
------------------------------------

Modelines have historically been a source of security/resource vulnerabilities
and are therefore disabled by default in $VIMRUNTIME/debian.vim.

You can enable them in ~/.vimrc or /etc/vim/vimrc with "set modeline".

In order to mimic Vim's default setting (modelines disabled when root, enabled
otherwise), you may instead want to use the following snippet:

  if $USER != 'root'
    set modeline
  else
    set nomodeline
  endif

The securemodelines script from vim.org (and in the vim-scripts package) may
also be of interest as it provides a way to whitelist exactly which options
may be set from a modeline.

 -- James Vega <jamessan@debian.org>  Sun, 04 May 2008 03:11:51 -0400
