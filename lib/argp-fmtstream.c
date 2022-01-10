/* Word-wrapping and line-truncating streams
   Copyright (C) 1997-2022 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Written by Miles Bader <miles@gnu.ai.mit.edu>.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* This package emulates glibc 'line_wrap_stream' semantics for systems that
   don't have that.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <wchar.h>

#include "argp-fmtstream.h"
#include "argp-namefrob.h"
#include "mbswidth.h"

#ifndef ARGP_FMTSTREAM_USE_LINEWRAP

#ifndef isblank
#define isblank(ch) ((ch)==' ' || (ch)=='\t')
#endif

#ifdef _LIBC
# include <wchar.h>
# include <libio/libioP.h>
# define __vsnprintf(s, l, f, a) _IO_vsnprintf (s, l, f, a)
#else
# include <unilbrk.h>
#endif

#define INIT_BUF_SIZE 200
#define PRINTF_SIZE_GUESS 150

/* Return an argp_fmtstream that outputs to STREAM, and which prefixes lines
   written on it with LMARGIN spaces and limits them to RMARGIN columns
   total.  If WMARGIN >= 0, words that extend past RMARGIN are wrapped by
   replacing the whitespace before them with a newline and WMARGIN spaces.
   Otherwise, chars beyond RMARGIN are simply dropped until a newline.
   Returns NULL if there was an error.  */
argp_fmtstream_t
__argp_make_fmtstream (FILE *stream,
                       size_t lmargin, size_t rmargin, ssize_t wmargin)
{
  argp_fmtstream_t fs;

  fs = (struct argp_fmtstream *) malloc (sizeof (struct argp_fmtstream));
  if (fs != NULL)
    {
      fs->stream = stream;

      fs->lmargin = lmargin;
      fs->rmargin = rmargin;
      fs->wmargin = wmargin;
      fs->point_col = 0;
      fs->point_offs = 0;

      fs->buf = (char *) malloc (INIT_BUF_SIZE);
      if (! fs->buf)
        {
          free (fs);
          fs = 0;
        }
      else
        {
          fs->p = fs->buf;
          fs->end = fs->buf + INIT_BUF_SIZE;
        }
    }

  return fs;
}
#if 0
/* Not exported.  */
#ifdef weak_alias
weak_alias (__argp_make_fmtstream, argp_make_fmtstream)
#endif
#endif

/* Flush FS to its stream, and free it (but don't close the stream).  */
void
__argp_fmtstream_free (argp_fmtstream_t fs)
{
  __argp_fmtstream_update (fs);
  if (fs->p > fs->buf)
    {
#ifdef _LIBC
      __fxprintf (fs->stream, "%.*s", (int) (fs->p - fs->buf), fs->buf);
#else
      fwrite_unlocked (fs->buf, 1, fs->p - fs->buf, fs->stream);
#endif
    }
  free (fs->buf);
  free (fs);
}
#if 0
/* Not exported.  */
#ifdef weak_alias
weak_alias (__argp_fmtstream_free, argp_fmtstream_free)
#endif
#endif

/* Insert suffix and left margin at POINT_OFFS, flushing as needed.  */
static void
line_at_point (argp_fmtstream_t fs, const char *suffix)
{
  size_t i, space_needed = fs->wmargin;
  char *nl, *queue = fs->buf + fs->point_offs;

  if (suffix)
    space_needed += strlen(suffix);

  while (fs->p + space_needed > fs->end)
    {
      /* Output the first line so we can use the space.  */
      nl = memchr (fs->buf, '\n', fs->point_offs);
      if (nl == NULL)
        {
          /* Line longer than buffer - shouldn't happen.  Truncate.  */
          nl = queue - 1;
          *nl = '\n';
        }

#ifdef _LIBC
      __fxprintf (fs->stream, "%.*s\n",
                  (int) (nl - fs->buf), fs->buf);
#else
      if (nl > fs->buf)
        fwrite_unlocked (fs->buf, 1, nl - fs->buf, fs->stream);
      putc_unlocked ('\n', fs->stream);
#endif

      memmove (fs->buf, nl + 1, nl + 1 - fs->buf);
      fs->p -= nl + 1 - fs->buf;
      fs->point_offs -= nl + 1 - fs->buf;
    }

  memmove (queue + space_needed, queue, fs->p - queue);
  if (suffix)
    {
      memcpy (queue, suffix, strlen (suffix));
      fs->point_offs += strlen (suffix);
    }
  for (i = 0; i < fs->lmargin && fs->point_col != -1; i++)
    fs->buf[fs->point_offs++] = ' ';
  fs->point_col = fs->lmargin;
}

#ifdef _LIBC
# define UC_BREAK_UNDEFINED 0
# define UC_BREAK_POSSIBLE 1
# define UC_BREAK_HYPHENATION 2
# define UC_BREAK_MANDATORY 3
static int
uwl_shim(const char *s, size_t n, int width, int start_column,
         const char *encoding, char *p)
{
  size_t i = 0, cur_col = start_column, last_option = 0, c_len;
  mbstate_t ps;
  wchar_t c;

  memset(&ps, 0, sizeof (ps));
  memset(p, UC_BREAK_UNDEFINED, n);

  while (i < n)
    {
      c_len = mbrtowc (&c, s + i, n, &ps);
      if (c_len == 0 || c_len == -2)
        {
          break;
        }
      else if (c_len == -1)
        {
          /* Malformed.  Walk forward until things make sense again.  */
          i++;
          cur_col++;
          continue;
        }

      if (c == L'\n')
        {
          p[i] = UC_BREAK_MANDATORY;
          last_option = 0;
          cur_col = 0;
        }
      else if (c == L' ' || c == '\t')
        {
          /* Best effort - won't help with Chinese or Thai.  */
          last_option = i;
        }
      else if (cur_col >= width && last_option != 0)
        {
          p[last_option] = UC_BREAK_POSSIBLE;
          last_option = 0;
          cur_col = 0;
        }

      i += c_len;
      cur_col += wcwidth(c);
    }

  return n - last_break;
}
#define ulc_width_linebreaks(s, n, width, start_column, at_end_columns, \
                             o, encoding, p)                            \
  uwl_shim(s, n, width, start_column, encoding, p)
#endif

/* Process FS's buffer so that line wrapping is done from POINT_OFFS to the
   end of its buffer.  */
void
__argp_fmtstream_update (argp_fmtstream_t fs)
{
  char *lns, *nl, *c, *queue = fs->buf + fs->point_offs;
  int col;
  size_t i, proc_len = fs->p - queue;

  if (queue >= fs->p)
    return;

  if (fs->wmargin == -1)
    {
      while (fs->buf + fs->point_offs < fs->p)
        {
          nl = memchr (fs->buf + fs->point_offs, '\n',
                       fs->rmargin - fs->point_col);
          if (nl)
            {
              fs->point_offs = nl - queue;
              line_at_point (fs, NULL);
              continue;
            }

          /* Truncate until end of line.  */
          fs->point_offs += fs->rmargin - fs->point_col;
          nl = memchr (fs->buf + fs->point_offs + fs->rmargin - fs->point_col,
                       '\n', fs->p - fs->buf - fs->point_offs);
          if (! nl)
            {
              /* This is the last line.  */
              fs->buf[fs->point_offs++] = '\n';
              fs->p = fs->buf + fs->point_offs;
              line_at_point(fs, NULL);
              return;
            }

          fs->buf[fs->point_offs++] = '\n';
          memmove (fs->buf + fs->point_offs, nl + 1,
                   nl + 1 - fs->buf - fs->point_offs);
          line_at_point(fs, NULL);
        }
      return;
    }

  lns = (char *) malloc (proc_len);
  if (! lns)
    return;
  
  col = ulc_width_linebreaks (queue, proc_len, fs->rmargin - fs->wmargin,
                              fs->point_col == -1 ? 0 : fs->point_col, 0,
                              NULL, locale_charset (), lns);
  for (i = 0; i < proc_len; i++)
    {
      if (lns[i] == UC_BREAK_HYPHENATION)
        {
          line_at_point (fs, "-\n");
        }
      else if (lns[i] == UC_BREAK_POSSIBLE)
        {
          line_at_point (fs, "\n");
        }
      else if (lns[i] == UC_BREAK_MANDATORY)
        {
          fs->point_offs++;
          line_at_point (fs, NULL);
        }
      else
        {
          fs->point_offs++;
        }
    }
  fs->point_col = col;

  free (lns);
}

/* Ensure that FS has space for AMOUNT more bytes in its buffer, either by
   growing the buffer, or by flushing it.  True is returned iff we succeed. */
int
__argp_fmtstream_ensure (struct argp_fmtstream *fs, size_t amount)
{
  if ((size_t) (fs->end - fs->p) < amount)
    {
      ssize_t wrote;

      /* Flush FS's buffer.  */
      __argp_fmtstream_update (fs);

#ifdef _LIBC
      __fxprintf (fs->stream, "%.*s", (int) (fs->p - fs->buf), fs->buf);
      wrote = fs->p - fs->buf;
#else
      wrote = fwrite_unlocked (fs->buf, 1, fs->p - fs->buf, fs->stream);
#endif
      if (wrote == fs->p - fs->buf)
        {
          fs->p = fs->buf;
          fs->point_offs = 0;
        }
      else
        {
          fs->p -= wrote;
          fs->point_offs -= wrote;
          memmove (fs->buf, fs->buf + wrote, fs->p - fs->buf);
          return 0;
        }

      if ((size_t) (fs->end - fs->buf) < amount)
        /* Gotta grow the buffer.  */
        {
          size_t old_size = fs->end - fs->buf;
          size_t new_size = old_size + amount;
          char *new_buf;

          if (new_size < old_size || ! (new_buf = realloc (fs->buf, new_size)))
            {
              __set_errno (ENOMEM);
              return 0;
            }

          fs->buf = new_buf;
          fs->end = new_buf + new_size;
          fs->p = fs->buf;
        }
    }

  return 1;
}

ssize_t
__argp_fmtstream_printf (struct argp_fmtstream *fs, const char *fmt, ...)
{
  int out;
  size_t avail;
  size_t size_guess = PRINTF_SIZE_GUESS; /* How much space to reserve. */

  do
    {
      va_list args;

      if (! __argp_fmtstream_ensure (fs, size_guess))
        return -1;

      va_start (args, fmt);
      avail = fs->end - fs->p;
      out = __vsnprintf (fs->p, avail, fmt, args);
      va_end (args);
      if ((size_t) out >= avail)
        size_guess = out + 1;
    }
  while ((size_t) out >= avail);

  fs->p += out;

  return out;
}
#if 0
/* Not exported.  */
#ifdef weak_alias
weak_alias (__argp_fmtstream_printf, argp_fmtstream_printf)
#endif
#endif

#endif /* !ARGP_FMTSTREAM_USE_LINEWRAP */
