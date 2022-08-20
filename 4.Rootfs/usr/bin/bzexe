#!/bin/sh
# gzexe: compressor for Unix executables.
# Use this only for binaries that you do not use frequently.
#
# The compressed version is a shell script which decompresses itself after
# skipping $skip lines of shell commands.  We try invoking the compressed
# executable with the original name (for programs looking at their name).
# We also try to retain the original file permissions on the compressed file.
# For safety reasons, gzexe will not create setuid or setgid shell scripts.

# WARNING: the first line of this file must be either : or #!/bin/sh
# The : is required for some old versions of csh.
# On Ultrix, /bin/sh is too buggy, change the first line to: #!/bin/sh5


# Copyright (C) 1998, 2002 Free Software Foundation
# Copyright (C) 1993 Jean-loup Gailly

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
# 02111-1307, USA.


PATH="/usr/bin:$PATH"
x=`basename $0`
if test $# = 0; then
  echo compress executables. original file foo is renamed to foo~
  echo usage: ${x} [-d] files...
  echo   "   -d  decompress the executables"
  exit 1
fi

set -C
tmp=gz$$
trap "rm -f $tmp; exit 1" HUP INT QUIT TRAP USR1 PIPE TERM
: > $tmp || exit 1

decomp=0
res=0
test "$x" = "ungzexe" && decomp=1
if test "x$1" = "x-d"; then
  decomp=1
  shift
fi

echo hi > zfoo1$$ || exit 1
echo hi > zfoo2$$ || exit 1
if test -z "`(${CPMOD-cpmod} zfoo1$$ zfoo2$$) 2>&1`"; then
  cpmod=${CPMOD-cpmod}
fi
rm -f zfoo[12]$$

tail=""
IFS="${IFS= 	}"; saveifs="$IFS"; IFS="${IFS}:"
for dir in $PATH; do
  test -z "$dir" && dir=.
  if test -f $dir/tail; then
    tail="$dir/tail"
    break
  fi
done
IFS="$saveifs"
if test -z "$tail"; then
  echo cannot find tail
  exit 1
fi
case `echo foo | $tail -n +1 2>/dev/null` in
foo) tail="$tail -n";;
esac

for i do
  if test ! -f "$i" ; then
    echo ${x}: $i not a file
    res=1
    continue
  fi
  if test $decomp -eq 0; then
    if sed -e 1d -e 2q "$i" | grep "^skip=[0-9]*$" >/dev/null; then
      echo "${x}: $i is already gzexe'd"
      continue
    fi
  fi
  if ls -l "$i" | grep '^...[sS]' > /dev/null; then
    echo "${x}: $i has setuid permission, unchanged"
    continue
  fi
  if ls -l "$i" | grep '^......[sS]' > /dev/null; then
    echo "${x}: $i has setgid permission, unchanged"
    continue
  fi
  case "`basename $i`" in
  bzip2 | tail | sed | chmod | ln | sleep | rm)
	echo "${x}: $i would depend on itself"; continue ;;
  esac
  if test -z "$cpmod"; then
    cp -p "$i" $tmp 2>/dev/null || cp "$i" $tmp
    if test -w $tmp 2>/dev/null; then
      writable=1
    else
      writable=0
      chmod u+w $tmp 2>/dev/null
    fi
    : >| $tmp  # truncate the file, ignoring set -C
  fi
  if test $decomp -eq 0; then
    sed 1q $0 >> $tmp
    sed "s|^if tail|if $tail|" >> $tmp <<'EOF'
skip=23
set -C
umask=`umask`
umask 77
tmpfile=`tempfile -p gztmp -d /tmp` || exit 1
if tail +$skip "$0" | /bin/bzip2 -cd >> $tmpfile; then
  umask $umask
  /bin/chmod 700 $tmpfile
  prog="`echo $0 | /bin/sed 's|^.*/||'`"
  if /bin/ln -T $tmpfile "/tmp/$prog" 2>/dev/null; then
    trap '/bin/rm -f $tmpfile "/tmp/$prog"; exit $res' 0
    (/bin/sleep 5; /bin/rm -f $tmpfile "/tmp/$prog") 2>/dev/null &
    /tmp/"$prog" ${1+"$@"}; res=$?
  else
    trap '/bin/rm -f $tmpfile; exit $res' 0
    (/bin/sleep 5; /bin/rm -f $tmpfile) 2>/dev/null &
    $tmpfile ${1+"$@"}; res=$?
  fi
else
  echo Cannot decompress $0; exit 1
fi; exit $res
EOF
    bzip2 -cv9 "$i" >> $tmp || {
      /bin/rm -f $tmp
      echo ${x}: compression not possible for $i, file unchanged.
      res=1
      continue
    }

  else
    # decompression
    skip=23
    if sed -e 1d -e 2q "$i" | grep "^skip=[0-9]*$" >/dev/null; then
      eval `sed -e 1d -e 2q "$i"`
    fi
    if tail +$skip "$i" | bzip2 -cd > $tmp; then
      :
    else
      echo ${x}: $i probably not in gzexe format, file unchanged.
      res=1
      continue
    fi
  fi
  rm -f "$i~"
  mv "$i" "$i~" || {
    echo ${x}: cannot backup $i as $i~
    rm -f $tmp
    res=1
    continue
  }
  mv $tmp "$i" || cp -p $tmp "$i" 2>/dev/null || cp $tmp "$i" || {
    echo ${x}: cannot create $i
    rm -f $tmp
    res=1
    continue
  }
  rm -f $tmp
  if test -n "$cpmod"; then
    $cpmod "$i~" "$i" 2>/dev/null
  elif test $writable -eq 0; then
    chmod u-w $i 2>/dev/null
  fi
done
exit $res
