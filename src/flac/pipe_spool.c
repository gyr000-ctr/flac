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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "share/compat.h"
#include "pipe_spool.h"

/* Fixed transfer buffer: resident memory never exceeds this per spool. */
#define PIPE_SPOOL_BUFFER_SIZE 65536

#ifdef _WIN32

#include <windows.h>

/* Creates and names an empty temporary file, copying its path into 'path'. */
static FLAC__bool create_temp_path_(char *path, size_t path_size, const char **error)
{
	char dir[MAX_PATH], tmppath[MAX_PATH];
	DWORD n;

	n = GetTempPathA((DWORD)sizeof(dir), dir);
	if(n == 0 || n >= sizeof(dir)) {
		if(error) *error = "could not determine temporary directory";
		return false;
	}
	/* GetTempFileNameA creates the file, avoiding the temp-name race. */
	if(GetTempFileNameA(dir, "fla", 0, tmppath) == 0) {
		if(error) *error = "could not create temporary file";
		return false;
	}
	flac_snprintf(path, path_size, "%s", tmppath);
	return true;
}

#else /* !_WIN32 */

#include <unistd.h>

/* Creates and names an empty temporary file, copying its path into 'path'. */
static FLAC__bool create_temp_path_(char *path, size_t path_size, const char **error)
{
	const char *dir;
	char tmpl[4096];
	int fd;

	dir = getenv("TMPDIR");
	if(0 == dir || 0 == dir[0])
		dir = getenv("TMP");
	if(0 == dir || 0 == dir[0])
		dir = getenv("TEMP");
	if(0 == dir || 0 == dir[0])
		dir = "/tmp";

	if(flac_snprintf(tmpl, sizeof(tmpl), "%s/flac-XXXXXX", dir) >= (int)sizeof(tmpl)) {
		if(error) *error = "temporary directory path is too long";
		return false;
	}
	/* mkstemp creates the file with mode 0600, avoiding the temp-name race. */
	if((fd = mkstemp(tmpl)) < 0) {
		if(error) *error = strerror(errno);
		return false;
	}
	/* Only the path is needed; callers open their own handle. */
	(void)close(fd);
	/* Keep the name on disk: the foreign metadata code re-opens the file by
	 * path and it must stay seekable through the whole run.  pipe_spool_delete()
	 * removes it on the way out. */
	flac_snprintf(path, path_size, "%s", tmpl);
	return true;
}

#endif /* _WIN32 */

pipe_spool_t *flac__pipe_spool_new(const char **error)
{
	pipe_spool_t *spool = calloc(1, sizeof(*spool));
	FILE *f;
	if(0 == spool) {
		if(error) *error = "out of memory";
		return 0;
	}
	if(!create_temp_path_(spool->path, sizeof(spool->path), error)) {
		free(spool);
		return 0;
	}
	if(0 == (f = flac_fopen(spool->path, "w+b"))) {
		(void)flac_unlink(spool->path);
		free(spool);
		if(error) *error = "could not open temporary file for updating";
		return 0;
	}
	spool->file = f;
	return spool;
}

pipe_spool_t *flac__pipe_spool_new_for_output(const char **error)
{
	pipe_spool_t *spool = calloc(1, sizeof(*spool));
	if(0 == spool) {
		if(error) *error = "out of memory";
		return 0;
	}
	if(!create_temp_path_(spool->path, sizeof(spool->path), error)) {
		free(spool);
		return 0;
	}
	/* No FILE* here: the encode/decode session opens and closes the path. */
	spool->file = 0;
	return spool;
}

void flac__pipe_spool_delete(pipe_spool_t *spool)
{
	if(0 == spool)
		return;
	/* Only close a handle we still own; an output spool never had one and an
	 * input spool's handle is detached once the session takes it over. */
	if(spool->file)
		fclose(spool->file);
	(void)flac_unlink(spool->path);
	free(spool);
}

/*
 * The encoder/decoder sessions take ownership of a FILE* handed to them
 * and close it on teardown.  Detach the handle from the spool so the
 * delete path does not close it a second time; the backing file is still
 * removed by name.
 */
void flac__pipe_spool_detach_file(pipe_spool_t *spool)
{
	if(spool)
		spool->file = 0;
}

FLAC__bool flac__pipe_spool_rewind(pipe_spool_t *spool, const char **error)
{
	if(0 == spool->file) {
		if(error) *error = "internal error: spool has no open handle";
		return false;
	}
	if(fseeko(spool->file, 0, SEEK_SET) < 0) {
		if(error) *error = "seek failed in temporary file";
		return false;
	}
	return true;
}

FLAC__bool flac__pipe_spool_copy_from_stream(pipe_spool_t *spool, FILE *src, const char **error)
{
	return flac__pipe_spool_copy_from_stream_with_prefix(spool, src, 0, 0, error);
}

FLAC__bool flac__pipe_spool_copy_from_stream_with_prefix(pipe_spool_t *spool, FILE *src, const void *prefix, size_t prefix_length, const char **error)
{
	char buffer[PIPE_SPOOL_BUFFER_SIZE];
	size_t n;

	if(prefix_length > 0) {
		if(fwrite(prefix, 1, prefix_length, spool->file) != prefix_length) {
			if(error) *error = "write failed in temporary file";
			return false;
		}
		spool->size += prefix_length;
	}

	for(;;) {
		n = fread(buffer, 1, sizeof(buffer), src);
		if(n > 0) {
			if(fwrite(buffer, 1, n, spool->file) != n) {
				if(error) *error = "write failed in temporary file";
				return false;
			}
			spool->size += n;
		}
		if(ferror(src)) {
			if(error) *error = "read failed on input stream";
			return false;
		}
		if(n < sizeof(buffer)) {
			if(feof(src))
				break;
			if(ferror(src)) {
				if(error) *error = "read failed on input stream";
				return false;
			}
		}
	}
	if(fflush(spool->file) != 0) {
		if(error) *error = "write failed in temporary file";
		return false;
	}
	return true;
}

FLAC__bool flac__pipe_spool_dump_to_stream(pipe_spool_t *spool, FILE *dst, const char **error)
{
	char buffer[PIPE_SPOOL_BUFFER_SIZE];
	/*
	 * Output spools never hold a handle (the producing session owns its
	 * own), so always open a short-lived read handle by path.
	 */
	FILE *f = flac_fopen(spool->path, "rb");
	if(0 == f) {
		if(error) *error = "could not reopen temporary file for reading";
		return false;
	}

	/* Read until EOF: an output spool is written by the session, which
	 * does not update the size counter. */
	for(;;) {
		size_t n = fread(buffer, 1, sizeof(buffer), f);
		if(n > 0) {
			if(fwrite(buffer, 1, n, dst) != n) {
				if(error) *error = "write failed on output stream";
				fclose(f);
				return false;
			}
		}
		if(ferror(f)) {
			if(error) *error = "read failed in temporary file";
			fclose(f);
			return false;
		}
		if(n < sizeof(buffer)) {
			if(feof(f))
				break;
			if(ferror(f)) {
				if(error) *error = "read failed in temporary file";
				fclose(f);
				return false;
			}
		}
	}
	fclose(f);
	if(fflush(dst) != 0) {
		if(error) *error = "write failed on output stream";
		return false;
	}
	return true;
}
