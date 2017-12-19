/*
Serval string buffer primitives
Copyright (C) 2012-2015 Serval Project Inc.

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

#ifndef __STRBUF_H__
#define __STRBUF_H__

#include <stddef.h>
#include "lang.h"

/**
    A strbuf provides a convenient set of primitives for assembling a
    nul-terminated string in a fixed-size, caller-provided backing buffer,
    using a sequence of append operations.

    An append operation that would overflow the buffer is truncated with a nul
    terminator and the "overrun" property of the strbuf becomes true until the
    next strbuf_init() or strbuf_trunc().  Any append to an overrun strbuf will
    be fully truncated, ie, nothing more will be appended to the buffer.

    The string in the buffer is guaranteed to always be nul terminated, which
    means that the maximum strlen() of the assembled string is one less than
    the buffer size.  In other words, the following invariants always hold:
        strbuf_len(sb) < strbuf_size(sb)
        strbuf_str(sb)[strbuf_len(sb)] == '\0'

    char buf[100];
    struct strbuf b;
    strbuf_init(&b, buf, sizeof buf);
    strbuf_puts(&b, "text");
    strbuf_sprintf(&b, "fmt", val...);
    if (strbuf_overrun(&b))
        // error...
    else
        // use buf

    A strbuf counts the total number of chars appended to it, even ones that
    were truncated.  This count is always available via strbuf_count().

    A NULL buffer can be provided.  This causes the strbuf operations to
    perform all character counting and truncation calculations as usual, but
    not actually assemble the string; it is as though the strbuf is permanently
    overrun, but no nul terminator is appended.  This allows a strbuf to be
    used for calculating the size needed for a buffer, which the caller may
    then allocate and replay the same operations to fill.

    A buffer length of -1 can be given.  This causes the strbuf operations to
    treat the buffer as unlimited in size.  This is useful for when the caller
    is 100% certain that the strbuf will not be overrun.  For example, if the
    required buffer size was already computed by a preliminary run of the same
    strbuf operations on a NULL buffer, and the necessary size allocated.

    The strbuf operations will never write any data beyond the length of the
    assembled string plus one for the nul terminator.  So, for example, the
    following code will never alter buf[4]:

    char buf[5];
    buf[4] = 'x';
    strbuf b;
    strbuf_init(b, buf, sizeof buf);
    strbuf_puts(&b, "abc");
    assert buf[4] == 'x'; // always passes

*/

#include <sys/types.h>
#include <stdint.h> // for SIZE_MAX on Debian/Unbuntu/...
#include <limits.h> // for SIZE_MAX on Android
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
#include <assert.h>

#ifndef __STRBUF_INLINE
# if __GNUC__ && !__GNUC_STDC_INLINE__
#  define __STRBUF_INLINE extern inline
# else
#  define __STRBUF_INLINE inline
# endif
#endif

struct strbuf {
    char *start; // NULL after strbuf_init(buffer=NULL)
    char *end; // NULL after strbuf_init(size=-1), otherwise end=&start[size-1]
    char *current;
};

/** Static constant for initialising a struct strbuf to empty:
 *      struct strbuf ssb = STRUCT_STRBUF_EMPTY;
 * Immediately following this assignment, the following properties hold:
 *      strbuf_is_empty(&ssb)
 *      strbuf_len(&ssb) == 0
 *      strbuf_count(&ssb) == 0
 *      strbuf_str(&ssb) == NULL
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define STRUCT_STRBUF_EMPTY ((struct strbuf){NULL, NULL, NULL})

/** Constant for initialising a struct strbuf to a static backing buffer:
 *      char buf[n];
 *      struct strbuf ssb = STRUCT_STRBUF_INIT_STATIC(buf);
 * Immediately following this assignment, the following properties hold:
 *      strbuf_is_empty(&ssb)
 *      strbuf_len(&ssb) == 0
 *      strbuf_count(&ssb) == 0
 *      strbuf_str(&ssb) == buf
 *      strbuf_size(sb) == n
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define STRUCT_STRBUF_INIT_STATIC(B) ((struct strbuf){(B), (B) + sizeof(B) - 1, (B)})

typedef struct strbuf *strbuf;
typedef const struct strbuf *const_strbuf;

/** The number of bytes occupied by a strbuf (not counting its backing buffer).
 */
#define SIZEOF_STRBUF (sizeof(struct strbuf))


// clang doesn't force the allignment of alloca() which can lead to undefined behaviour. eg SIGBUS
// TODO write autoconf test for this
#ifdef __clang__
  #define __ALIGNMENT_OF(T) offsetof( struct { char x; T dummy; }, dummy)
  #define alloca_aligned(size, T) (void*)((uintptr_t)alloca(size+__ALIGNMENT_OF(T)-1)+__ALIGNMENT_OF(T)-1 & ~(__ALIGNMENT_OF(T)-1) )
#else
  #define alloca_aligned(size, T) alloca(size)
#endif


/** Convenience macro for allocating a strbuf and its backing buffer on the
 * heap using a single call to malloc(3).
 *
 *      strbuf func1() {
 *          strbuf b = strbuf_malloc(1024);
 *          strbuf_puts(b, "some text");
 *          strbuf_puts(b, " some more text");
 *          return b;
 *      }
 *      strbuf func2() {
 *          strbuf b = func1();
 *          printf("%s\n", strbuf_str(b));
 *          free(b);
 *      }
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define strbuf_malloc(size) strbuf_make(malloc(SIZEOF_STRBUF + (size)), SIZEOF_STRBUF + (size))

/** Convenience macro for allocating a strbuf and its backing buffer on the
 * stack within the calling function.  The returned strbuf is only valid for
 * the duration of the function, so it must not be returned.  See alloca(3) for
 * more information.
 *
 *      void func() {
 *          strbuf b = strbuf_alloca(1024);
 *          strbuf_puts(b, "some text");
 *          strbuf_puts(b, " some more text");
 *          printf("%s\n", strbuf_str(b));
 *      }
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define strbuf_alloca(size) strbuf_make(alloca_aligned(SIZEOF_STRBUF + (size), strbuf), SIZEOF_STRBUF + (size))

/** Convenience macro that calls strbuf_alloca() to allocate a large enough
 * buffer to hold the entire content produced by a given expression that
 * appends to the strbuf.  The first strbuf_alloca() will use the supplied
 * initial length, and if that overruns, then a second strbuf_alloca() will use
 * the strbuf_count() from the first pass, so as long as the expression is
 * stable (ie, always produces the same output), the final assert() will not
 * be triggered.
 *
 *      strbuf b;
 *      STRBUF_ALLOCA_FIT(b, 20, (strbuf_append_variable_content(b, ...)));
 *
 * WARNING: this macro expands its third argument twice, so the third argument
 * must have no side effects.
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define STRBUF_ALLOCA_FIT(__SB, __INITIAL_LEN, __EXPR) \
    do { \
        __SB = strbuf_alloca((__INITIAL_LEN) + 1); \
        __EXPR; \
        if (strbuf_overrun(__SB)) { \
            __SB = strbuf_alloca(strbuf_count(__SB) + 1); \
            __EXPR; \
        } \
        assert(!strbuf_overrun(__SB)); \
    } while (0)

/** Convenience macro for filling a strbuf from the calling function's
 * printf(3)-like variadic arguments.
 *
 *      #include <stdarg.h>
 *
 *      void funcf(const char *format, ...) {
 *          strbuf b = strbuf_alloca(1024);
 *          strbuf_va_printf(b, format);
 *          ...
 *      }
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define strbuf_va_printf(sb,fmt) do { \
            va_list __strbuf_ap; \
            va_start(__strbuf_ap, fmt); \
            strbuf_vsprintf(sb, (fmt), __strbuf_ap); \
            va_end(__strbuf_ap); \
        } while (0)

/** Convenience macro for filling a strbuf from the calling function's va_list
 * variadic argument pointer.
 *
 *      #include <stdarg.h>
 *
 *      void funcf(const char *format, va_list ap) {
 *          strbuf b = strbuf_alloca(1024);
 *          strbuf_va_vprintf(b, format, ap);
 *          ...
 *      }
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#define strbuf_va_vprintf(sb,fmt,ap) do { \
            va_list __strbuf_ap; \
            va_copy(__strbuf_ap, (ap)); \
            strbuf_vsprintf(sb, (fmt), __strbuf_ap); \
            va_end(__strbuf_ap); \
        } while (0)

/** Convenience macro to allocate a strbuf for use within the calling function,
 * based on a caller-supplied backing buffer.  The returned strbuf is only valid
 * for the duration of the function, so it must not be returned.  See alloca(3)
 * for more information.  However, the backing buffer may have any scope.
 *
 *      void func(char *buf, size_t len) {
 *          strbuf b = strbuf_local(buf, len);
 *          strbuf_puts(b, "some text");
 *          strbuf_puts(b, " some more text");
 *      }
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#ifdef __GNUC__
#define strbuf_local(buf,len) __strbuf_init_chk(alloca(SIZEOF_STRBUF), (char*)(buf), (len), __builtin_object_size((buf), 1))
#else
#define strbuf_local(buf,len) strbuf_init(alloca(SIZEOF_STRBUF), (char*)(buf), (len))
#endif

/** Convenience variant of the strbuf_local() macro that computes the 'len'
 * parameter from 'sizeof buf'.
 *
 *      void print_integer(int value) {
 *          char temp[20];
 *          strbuf b = strbuf_local_buf(temp);
 *          strbuf_puts(b, "[");
 *          strbuf_sprintf(b, "%d", value);
 *          strbuf_puts(b, "]");
 *          printf("%s\n", temp);
 *      }
 *
 * WARNING: 'buf' must name a char[] array, not a char* pointer.  The following
 * code is wrong:
 *
 *      char *p = malloc(50);
 *      ...
 *      strbuf b = strbuf_local_buf(p); // ERROR!
 *
 * In the above example, sizeof(p) will be 8 (4 on 32-bit architectures) which
 * is NOT the size of the buffer that p points to (50), and not the desired
 * effect: the string in strbuf b will be limited to 7 chars in length.  If the
 * buffer pointed to by p were less than 8 in size, then appending to strbuf b
 * would cause memory corruption and a likely SIGSEGV.
 *
 * If compiled with the GNU C compiler (or equivalent, like Clang), then the
 * above example would produce a build error (see below).  However, if the
 * compiler does not support __attribute__((alloc_size(n)) (such as Clang 3.5),
 * then the check is not performed, because it would also cause errors for
 * perfectly legitimate uses, eg, strbuf_local_buf(a->buf).
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
#if defined(__GNUC__) && defined(HAVE_FUNC_ATTRIBUTE_ALLOC_SIZE)

#   define strbuf_local_buf(buf) strbuf_local((char*)(buf), (__buffer_size_chk(sizeof(buf), __builtin_object_size(buf, 1))))

// If the following assertion fails, it means that the argument to
// strbuf_local_buf() was not an array whose size is known at compile time.
// The most common cause of this is passing a pointer as the argument.  The
// solution is to use strbuf_local(b, len) instead of strbuf_local_buf(b),
// and supply the length of the buffer explicitly.
__STRBUF_INLINE size_t __buffer_size_chk(size_t size, size_t chk) {
    assert(chk == (size_t)-1 || size == chk);
    return size;
}

#else
#   define strbuf_local_buf(buf) strbuf_local((char*)(buf), sizeof(buf))
#endif

/** Initialise a strbuf with a caller-supplied backing buffer.  The current
 * backing buffer and its contents are forgotten, and all strbuf operations
 * henceforward will operate on the new backing buffer.  Returns its first
 * argument.
 *
 * Immediately following strbuf_init(sb,b,n), the following properties hold:
 *      strbuf_str(sb) == b
 *      strbuf_size(sb) == n
 *      strbuf_len(sb) == 0
 *      strbuf_count(sb) == 0
 *      b == NULL || b[0] == '\0'
 *
 * If the 'buffer' argument is NULL, the strbuf is marked as "empty" and all
 * subsequent strbuf operations will all act as usual with the sole exception
 * that no chars will be copied into a backing buffer.  This allows strbuf to
 * be used for summing the lengths of strings.
 *
 * If the 'size' argument is zero, then strbuf does not write into its backing
 * buffer, not even a terminating nul.
 *
 * The __strbuf_init_chk() function calls strbuf_init() after ensuring that if
 * the given buffer's size is known at compile time (chk != -1) and the strbuf
 * is not being initialised to "indefinite" length (size != -1) then the given
 * size does not exceed the size of the buffer (size <= chk).
 * https://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html#Object-Size-Checking
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_init(strbuf sb, char *buffer, ssize_t size);

#ifdef __GNUC__
__STRBUF_INLINE strbuf __strbuf_init_chk(strbuf sb, char *buffer, ssize_t size, ssize_t chk) {
    if (chk != -1 && size != -1)
        assert(size <= chk); // buffer overflow
    return strbuf_init(sb, buffer, size);
}
#endif

/** Initialise a strbuf and its backing buffer inside the caller-supplied
 * buffer of the given size.  If the 'size' argument is less than
 * SIZEOF_STRBUF, then strbuf_make() returns NULL.
 *
 * Immediately following sb = strbuf_make(buf,len) where len >= SIZEOF_STRBUF,
 * the following properties hold:
 *      (char*) sb == buf
 *      strbuf_str(sb) == &buf[SIZEOF_STRBUF];
 *      strbuf_size(sb) == len - SIZEOF_STRBUF;
 *      strbuf_len(sb) == 0
 *      strbuf_count(sb) == 0
 *      strbuf_str(sb)[0] == '\0'
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE strbuf strbuf_make(char *buffer, size_t size) {
  return size < SIZEOF_STRBUF ? NULL : strbuf_init((strbuf) buffer, buffer + SIZEOF_STRBUF, (ssize_t)(size - SIZEOF_STRBUF));
}

/** Reset a strbuf.  The current position is set to the start of the buffer, so
 * the next append will write at the start of the buffer.  The prior contents
 * of the buffer are forgotten and will be overwritten.
 *
 * Immediately following strbuf_reset(sb), the following properties hold:
 *      strbuf_len(sb) == 0
 *      strbuf_count(sb) == 0
 *      strbuf_str(sb) == NULL || strbuf_str(sb)[0] == '\0'
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_reset(strbuf sb);

/** Append a nul-terminated string to the strbuf up to a maximum number,
 * truncating if necessary to avoid buffer overrun, and terminating with a nul
 * which is not counted in the maximum.  Return a pointer to the strbuf so that
 * concatenations can be chained in a single line: eg,
 * strbuf_ncat(strbuf_ncat(sb, "abc", 1), "def", 2) gives a strbuf containing
 * "ade";
 *
 * After these operations:
 *      n = strbuf_len(sb);
 *      c = strbuf_count(sb);
 *      strbuf_ncat(text, len);
 * the following invariants hold:
 *      strbuf_count(sb) == c + min(strlen(text), len)
 *      strbuf_len(sb) >= n
 *      strbuf_len(sb) <= n + len
 *      strbuf_len(sb) <= n + strlen(text)
 *      strbuf_str(sb) == NULL || strbuf_len(sb) == n || strncmp(strbuf_str(sb) + n, text, strbuf_len(sb) - n) == 0
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_ncat(strbuf sb, const char *text, size_t len);

/** Append a nul-terminated string to the strbuf, truncating if necessary to
 * avoid buffer overrun.  Return a pointer to the strbuf so that concatenations
 * can be chained in a single line: strbuf_puts(strbuf_puts(sb, "a"), "b");
 *
 * After these operations:
 *      n = strbuf_len(sb);
 *      c = strbuf_count(sb);
 *      strbuf_puts(text);
 * the following invariants hold:
 *      strbuf_count(sb) == c + strlen(text)
 *      strbuf_len(sb) >= n
 *      strbuf_len(sb) <= n + strlen(text)
 *      strbuf_str(sb) == NULL || strbuf_len(sb) == n || strncmp(strbuf_str(sb) + n, text, strbuf_len(sb) - n) == 0
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_puts(strbuf sb, const char *text);

/** Append binary data strbuf, as up to 'len' characters of uppercase
 * hexadecimal format, truncating if necessary to avoid buffer overrun.  Return
 * a pointer to the strbuf.
 *
 * After these operations:
 *      n = strbuf_len(sb);
 *      c = strbuf_count(sb);
 *      strbuf_tohex(len, data);
 * the following invariants hold:
 *      strbuf_count(sb) == c + len
 *      strbuf_len(sb) >= n
 *      strbuf_len(sb) <= n + len
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_tohex(strbuf sb, size_t strlen, const unsigned char *data);

/** Append a single character to the strbuf if there is space, and place a
 * terminating nul after it.  Return a pointer to the strbuf so that
 * concatenations can be chained in a single line.
 *
 * After these operations:
 *      n = strbuf_len(sb);
 *      c = strbuf_count(sb);
 *      strbuf_putc(ch);
 * the following invariants hold:
 *      strbuf_count(sb) == c + 1
 *      strbuf_len(sb) >= n
 *      strbuf_len(sb) <= n + 1
 *      strbuf_str(sb) == NULL || strbuf_len(sb) == n || strbuf_str(sb)[n] == ch
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_putc(strbuf sb, char ch);

/** Append the results of sprintf(fmt,...) to the string buffer, truncating if
 * necessary to avoid buffer overrun.  Return a pointer to the strbuf.
 *
 * This is equivalent to char tmp[...]; sprintf(tmp, fmt, ...); strbuf_puts(tmp);
 * assuming that tmp[] is large enough to contain the entire string produced by
 * the sprintf().
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_sprintf(strbuf sb, const char *fmt, ...) __attribute__((__ATTRIBUTE_format(printf, 2, 3)));
strbuf strbuf_vsprintf(strbuf sb, const char *fmt, va_list ap);

/** Return a pointer to the current nul-terminated string in the strbuf.
 *
 * This is the same as the 'buffer' argument passed to the most recent
 * strbuf_init().  If the caller still has that pointer, then can safely use it
 * instead of calling strbuf_str().
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE char *strbuf_str(const_strbuf sb) {
  return sb->start;
}

/** Return a pointer to the nul-terminator at the end of the string in the
 * strbuf.
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE char *strbuf_end(const_strbuf sb) {
  return sb->end && sb->current > sb->end ? sb->end : sb->current;
}

/** Return a pointer to the substring starting at a given offset.  If the
 * offset is negative, then it is taken from the end of the string, ie, the
 * length of the string is added to it.  The returned pointer always points
 * within the string.  If offset >= strbuf_len(sb), it points to the
 * terminating nul.  If offset <= -strbuf_len(sb) then it points to
 * strbuf_str(sb).
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
char *strbuf_substr(const_strbuf sb, int offset);

/** Truncate the string in the strbuf to a given offset.  If the offset is
 * negative, then it is taken from the end of the string, ie, the length of the
 * string is added to it.  If the string is shorter than the given offset, then
 * it is unchanged.  Otherwise, a terminating nul char is written at the offset
 * and the string's length truncated accordingly.  Return a pointer to the
 * strbuf so that operations can be chained in a single line.
 *
 * After the operations:
 *      count = strbuf_count(sb);
 *      len = strbuf_len(sb);
 *      strbuf_trunc(sb, off);
 * the following invariants hold:
 *  if count <= off, sb is unchanged:
 *      strbuf_count(sb) == count
 *      strbuf_len(sb) == len
 *  if len <= off < count:
 *      strbuf_count(sb) == off
 *      strbuf_len(sb) == len
 *  if 0 <= off < len:
 *      strbuf_count(sb) == off
 *      strbuf_len(sb) == off
 *  if -len <= off < 0:
 *      strbuf_count(sb) == len + off
 *      strbuf_len(sb) == len + off
 *  if off < -len:
 *      strbuf_count(sb) == 0
 *      strbuf_len(sb) == 0
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
strbuf strbuf_trunc(strbuf sb, int offset);

/** Return true if the given strbuf is "empty", ie, not modified since being
 * initialised to STRUCT_STRBUF_EMPTY or with strbuf_init(sb, NULL, 0);
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE size_t strbuf_is_empty(const_strbuf sb) {
  return sb->start == NULL && sb->end == NULL && sb->current == NULL;
}

/** Return the size of the backing buffer.  Return -1 if the buffer is of
 * undefined size.
 *
 * This is the same as the 'size' argument passed to the most recent
 * strbuf_init().
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE ssize_t strbuf_size(const_strbuf sb) {
  return sb->end ? sb->end - sb->start + 1 : -1;
}

/** Return length of current string in the strbuf, not counting the terminating
 * nul.
 *
 * Invariant: strbuf_len(sb) == strlen(strbuf_str(sb))
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE size_t strbuf_len(const_strbuf sb) {
  return (size_t)(strbuf_end(sb) - sb->start);
}

/** Return remaining space in the strbuf, not counting the terminating nul.
 * Return SIZE_MAX if the strbuf is of undefined size.
 *
 * Invariant: strbuf_size(sb) == -1 || strbuf_remaining(sb) == strbuf_size(sb) - strbuf_len(sb)
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE size_t strbuf_remaining(const_strbuf sb) {
  return !sb->end ? SIZE_MAX : sb->current > sb->end ? 0 : (size_t)(sb->end - sb->current);
}

/** Return the number of chars appended to the strbuf so far, not counting the
 * terminating nul.
 *
 * Invariant: strbuf_len(sb) <= strbuf_count(sb)
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE size_t strbuf_count(const_strbuf sb) {
  return (size_t)(sb->current - sb->start);
}

/** Return true iff the strbuf has been overrun, ie, any appended string has
 * been truncated since strbuf_init().
 *
 * Invariant: strbuf_overrun(sb) == strbuf_count(sb) != strbuf_len(sb)
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
__STRBUF_INLINE int strbuf_overrun(const_strbuf sb) {
  return sb->end && sb->current > sb->end;
}

#endif // __STRBUF_H__
