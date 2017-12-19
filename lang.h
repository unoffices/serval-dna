/*
Copyright (C) 2015 Serval Project Inc.
Copyright (C) 2016 Flinders University

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef __SERVAL_DNA__LANG_H
#define __SERVAL_DNA__LANG_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Conveniences to assist readability.
 */

typedef char bool_t;


/* Useful macros not specific to Serval DNA that assist with using the C
 * language.
 */

// Concatenate two preprocessor symbols, eg, _APPEND(__FUNC__, __LINE__).
#define __APPEND_(X,Y) X ## Y
#define _APPEND(X,Y) __APPEND_(X,Y)

// Number of elements in an array (Warning: does not work if A is a pointer!).
#define NELS(A) (sizeof (A) / sizeof *(A))

// Support for various GCC attributes.

#ifdef HAVE_FUNC_ATTRIBUTE_ERROR
#   define __ATTRIBUTE_error(m)  __error__(m)
#else
#   define __ATTRIBUTE_error(m)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
#   define __ATTRIBUTE_format(a,b,c)  __format__(a,b,c)
#else
#   define __ATTRIBUTE_format(a,b,c)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_MALLOC
#   define __ATTRIBUTE_malloc  __malloc__
#else
#   define __ATTRIBUTE_malloc
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_ALLOC_SIZE
#   define __ATTRIBUTE_alloc_size(n)  __alloc_size__(n)
#else
#   define __ATTRIBUTE_alloc_size(n)
#endif

// To suppress the "unused parameter" warning from -Wunused-parameter.
#ifdef HAVE_FUNC_ATTRIBUTE_UNUSED
#   define __ATTRIBUTE_unused  __unused__
#   define UNUSED(x) x __attribute__((__unused__))
#else
#   define __ATTRIBUTE_unused
#   define UNUSED(x) x
#endif

// To suppress the "may fall through" warning from -Wimplicit-fallthrough.
#ifdef HAVE_STMT_ATTRIBUTE_FALLTHROUGH
#   define __ATTRIBUTE_fallthrough  __fallthrough__
#   define FALLTHROUGH __attribute__((__fallthrough__))
#else
#   define __ATTRIBUTE_fallthrough
#   define FALLTHROUGH
#endif

#endif // __SERVAL_DNA__LANG_H
