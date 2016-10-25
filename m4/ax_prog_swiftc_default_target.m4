# Serval Project Swift language support
#
# SYNOPSIS
#
#   AX_PROG_SWIFTC_DEFAULT_TARGET
#
# DESCRIPTION
#
#   Sets the SWIFTC_DEFAULT_TARGET shell variable to the default target of the
#   $SWIFTC Swift compiler, as reported by its "-v" option, or "unknown" if
#   that option reports no target.
#
#   Also sets the following shell variables to the various parts of the target
#   name matching ARCH[-OS[MAJOR][.MINOR]] where ARCH contains no hyphen '-'
#   and MAJOR and MINOR are decimal integers:
#
#   - SWIFTC_DEFAULT_TARGET_ARCH, eg: x86_64, armv7
#   - SWIFTC_DEFAULT_TARGET_OS, eg: unknown-linux-gnu, apple-macosx
#   - SWIFTC_DEFAULT_TARGET_MAJOR, eg: 10
#   - SWIFTC_DEFAULT_TARGET_MINOR, eg: 8
#
# LICENSE
#
#   Copyright (C) 2016-2017 Flinders University
#
#   This program is free software; you can redistribute it and/or modify it
#   under the terms of the GNU General Public License as published by the
#   Free Software Foundation; either version 2 of the License, or (at your
#   option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
#   Public License for more details.
#
#   You should have received a copy of the GNU General Public License along
#   with this program. If not, see <http://www.gnu.org/licenses/>.
#
#   As a special exception, the respective Autoconf Macro's copyright owner
#   gives unlimited permission to copy, distribute and modify the configure
#   scripts that are the output of Autoconf when processing the Macro. You
#   need not follow the terms of the GNU General Public License when using
#   or distributing such scripts, even though portions of the text of the
#   Macro appear in them. The GNU General Public License (GPL) does govern
#   all other use of the material that constitutes the Autoconf Macro.
#
AU_ALIAS([AC_PROG_SWIFTC_DEFAULT_TARGET], [AX_PROG_SWIFTC_DEFAULT_TARGET])
AC_DEFUN([AX_PROG_SWIFTC_DEFAULT_TARGET],[
    AC_CACHE_CHECK([Swift compiler default target], ac_cv_prog_swiftc_default_target, [
        ac_cv_prog_swiftc_default_target=`$SWIFTC $SWIFTCFLAGS -v 2>&1 | sed -n -e 's/^Target:  *//p'`
        SWIFTC_DEFAULT_TARGET="$ac_cv_prog_swiftc_default_target"
        SWIFTC_DEFAULT_TARGET_ARCH=`echo "$SWIFTC_DEFAULT_TARGET" | sed -n -e ['s/-.*$//p']`
        SWIFTC_DEFAULT_TARGET_OS=`echo "$SWIFTC_DEFAULT_TARGET" | sed -n -e ['s/^[^-]*-//p']`
        SWIFTC_DEFAULT_TARGET_MINOR=`echo "$SWIFTC_DEFAULT_TARGET_OS" | sed -n -e ['s/^.*\.\([0-9][0-9]*\)$/\1/p']`
        if test "x$SWIFTC_DEFAULT_TARGET_MINOR" != x; then
            SWIFTC_DEFAULT_TARGET_OS=`echo "$SWIFTC_DEFAULT_TARGET_OS" | sed -n -e "s/\.$SWIFTC_DEFAULT_TARGET_MINOR\$//p"`
        fi
        SWIFTC_DEFAULT_TARGET_MAJOR=`echo "$SWIFTC_DEFAULT_TARGET_OS" | sed -n -e ['s/^.*[^0-9]\([0-9][0-9]*\)$/\1/p']`
        if test "x$SWIFTC_DEFAULT_TARGET_MAJOR" != x; then
            SWIFTC_DEFAULT_TARGET_OS=`echo "$SWIFTC_DEFAULT_TARGET_OS" | sed -n -e "s/$SWIFTC_DEFAULT_TARGET_MAJOR\$//p"`
        fi
    ])
    AC_PROVIDE([$0])dnl
])
