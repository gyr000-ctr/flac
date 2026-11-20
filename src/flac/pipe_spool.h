/* flac - Command-line FLAC encoder/decoder
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef flac__pipe_spool_h
#define flac__pipe_spool_h

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include "FLAC/ordinals.h"

/*
 * A pipe_spool is a seekable temporary file that lets the foreign
 * metadata machinery, which requires seeking, work on data that is read
 * from a non-seekable input (stdin) or written to a non-seekable output
 * (stdout).  All data movement goes through a fixed-size in-memory
 * buffer, so the amount of resident memory is bounded regardless of the
 * size of the stream; only the temporary backing file may grow.
 *
 * Handle ownership:
 *  - An *input* spool owns a FILE* returned by new().  That handle is
 *    handed to the encode session, which closes it; call detach_file()
 *    afterwards so delete() does not close it twice.
 *  - An *output* spool is created without a handle (new_for_output()).
 *    The encode/decode session creates and closes its own handle on the
 *    same path; dump_to_stream() opens a short-lived read handle.
 */
typedef struct {
	FILE *file; /* NULL for an output spool, or after detach_file() */
	char path[4096];
	FLAC__uint64 size;
} pipe_spool_t;

/*
 * Creates a fresh, empty, binary temporary file opened for reading and
 * writing ("w+b") and returns it with the handle stored in the spool.
 * The directory is taken from TMPDIR/TMP/TEMP when set, otherwise the
 * system default temporary directory is used.
 * Returns NULL and sets *error on failure.
 */
pipe_spool_t *flac__pipe_spool_new(const char **error);

/*
 * Creates a fresh, empty, closed binary temporary file intended to be
 * opened by path by an encode/decode session.  The spool holds no FILE*.
 * Returns NULL and sets *error on failure.
 */
pipe_spool_t *flac__pipe_spool_new_for_output(const char **error);

/* Closes (if still owned), removes and frees the spool.  Accepts NULL. */
void flac__pipe_spool_delete(pipe_spool_t *spool);

/*
 * Releases ownership of the underlying FILE* (e.g. when an encoder or
 * decoder session has taken it over and will close it itself).  The
 * backing temporary file is still removed when the spool is deleted.
 */
void flac__pipe_spool_detach_file(pipe_spool_t *spool);

/*
 * Copies all remaining bytes from 'src' into the spool using a bounded
 * buffer, then returns with the spool positioned at end of file.
 * Returns false and sets *error on a read or write failure.
 */
FLAC__bool flac__pipe_spool_copy_from_stream(pipe_spool_t *spool, FILE *src, const char **error);

/*
 * Writes the 'prefix' bytes already consumed from the input (e.g. the
 * format-detection lookahead) and then all remaining bytes of 'src'
 * into the spool.  The resulting spool is a byte-for-byte copy of the
 * original input.  Returns false and sets *error on failure.
 */
FLAC__bool flac__pipe_spool_copy_from_stream_with_prefix(pipe_spool_t *spool, FILE *src, const void *prefix, size_t prefix_length, const char **error);

/*
 * Copies the complete contents of the spool (rewinding it first) to
 * 'dst' using a bounded buffer, leaving the spool positioned at end of
 * file.  Returns false and sets *error on a read or write failure.
 */
FLAC__bool flac__pipe_spool_dump_to_stream(pipe_spool_t *spool, FILE *dst, const char **error);

/* Rewinds the spool to its beginning; returns false and sets *error on failure. */
FLAC__bool flac__pipe_spool_rewind(pipe_spool_t *spool, const char **error);

#endif
