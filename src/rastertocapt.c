/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 *
 * Licensed under the GNU General Public License Version 3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "std.h"
#include "printer.h"
#include "paper.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <cups/raster.h>
#include <cups/cups.h>
#include <stdlib.h>
#include <math.h>


struct cached_page_s {
	struct page_dims_s dims;
	struct band_list_s *bands;
};

struct band_list_s {
	struct band_list_s *next;
	size_t size;
	uint8_t data[];
};

/* printer and job state */
const struct printer_ops_s *ops;
struct printer_state_s *state = NULL;
struct cached_page_s *cached_page = NULL;
cups_raster_t *raster;

/* fine-tuning offsets in mm (+x = right, +y = down), from job options
 * capt-xshift-mm and capt-yshift-mm */
static double xshift_mm = 0.0;
static double yshift_mm = 0.0;

/* copy line_size bytes starting at bit 'off' of src (may be negative) */
static void copy_bits(uint8_t *dst, unsigned nbytes, const uint8_t *src, long srclen, long off)
{
	unsigned d;
	for (d = 0; d < nbytes; ++d) {
		long b = off + 8L * d;
		long byte = (b >= 0) ? b / 8 : -((-b + 7) / 8);
		unsigned sh = (unsigned) (b - byte * 8);
		unsigned hi = (byte >= 0 && byte < srclen) ? src[byte] : 0;
		unsigned lo = (byte + 1 >= 0 && byte + 1 < srclen) ? src[byte + 1] : 0;
		dst[d] = (uint8_t) (sh ? ((hi << sh) | (lo >> (8 - sh))) : hi);
	}
}

/* compressor state */
uint8_t *linebuf = NULL;
uint8_t *bandbuf = NULL;
uint8_t *compbuf = NULL;

static inline size_t sizeof_struct_band_list_s(size_t size)
	{ return sizeof(struct band_list_s) + size; }

static size_t center_pixels(size_t small, size_t large, unsigned bpp)
{
	unsigned small_p;
	unsigned large_p;
	unsigned bypp;
	while (bpp % 8)
		bpp *= 2;
	bypp = bpp / 8;
	small_p = small / bypp;
	large_p = large / bypp;
	return bypp * ((large_p - small_p) / 2);
}

static void free_cached_page(struct cached_page_s *cached_page)
{
	while (cached_page->bands) {
		void *p = cached_page->bands;
		cached_page->bands = cached_page->bands->next;
		free(p);
	}
	free(cached_page);
}

static void free_state(void)
{
	if (state) {
		if (ops->free_state)
			ops->free_state(state);
		else
			free(state);
		state = NULL;
	}
}

static void free_buffers(void)
{
	if (compbuf) {
		free(compbuf);
		compbuf = NULL;
	}
	if (bandbuf) {
		free(bandbuf);
		bandbuf = NULL;
	}
	if (linebuf) {
		free(linebuf);
		linebuf = NULL;
	}
}

static void compress_page_data(struct printer_state_s *state,
		struct cached_page_s *page,
		cups_raster_t *raster,
		const struct cups_page_header2_s *header)
{
	const struct page_dims_s *dims = &page->dims;
	const unsigned compsize = 2 * dims->line_size * dims->band_size;

	struct band_list_s *last_band = NULL;
	unsigned i;
	unsigned iband;

	unsigned shiftb = 0;
	unsigned shiftl = 0;
	unsigned csize = header->cupsBytesPerLine;


	if (header->cupsBytesPerLine < dims->line_size) {
		csize = header->cupsBytesPerLine;
		shiftb = center_pixels(header->cupsBytesPerLine, dims->line_size,
				header->cupsBitsPerPixel);
	}

	if (dims->line_size < header->cupsBytesPerLine) {
		csize = dims->line_size;
		shiftl = center_pixels(dims->line_size, header->cupsBytesPerLine,
				header->cupsBitsPerPixel);
	}

	if (header->cupsBytesPerLine > dims->line_size)
		fprintf(stderr, "INFO: cutting line from %u to %u bytes\n",
				header->cupsBytesPerLine, dims->line_size);

	if (header->cupsHeight > dims->num_lines)
		fprintf(stderr, "INFO: cutting image from %u to %u lines\n",
				header->cupsHeight, dims->num_lines);

	linebuf = calloc(1, header->cupsBytesPerLine);
	if (! linebuf)
		abort();
	bandbuf = calloc(dims->line_size, dims->band_size);
	if (! bandbuf)
		abort();
	compbuf = calloc(1, compsize);
	if (! compbuf)
		abort();

	long xoff_bits = (long) dims->crop_x_bits - lround(xshift_mm * dims->xdpi / 25.4);
	long yoff = (long) dims->crop_y_lines - lround(yshift_mm * dims->ydpi / 25.4);
	int bitmode = capt_geometry_canon || xoff_bits != 0;
	unsigned next_src = 0;

	/* skip raster lines above the image area */
	for (; yoff > 0 && (long) next_src < yoff && next_src < header->cupsHeight; ++next_src)
		cupsRasterReadPixels(raster, linebuf, header->cupsBytesPerLine);

	if (! capt_geometry_canon && xoff_bits) {
		/* legacy centring plus a manual shift */
		xoff_bits += 8L * (long) shiftl - 8L * (long) shiftb;
	}
	fprintf(stderr, "DEBUG: CAPT: raster offset x=%ld bits, y=%ld lines\n", xoff_bits, yoff);

	for (iband = 0; iband * dims->band_size < dims->num_lines; ++iband) {
		struct band_list_s *new_band;
		unsigned start = iband * dims->band_size;
		unsigned nlines = dims->band_size;
		unsigned iline;
		size_t size;
		memset(bandbuf, 0, dims->line_size * dims->band_size);
		if (nlines + start >= dims->num_lines)
			nlines = dims->num_lines - start;
		for (iline = 0; iline < nlines; ++iline) {
			long src = (long) (iline + start) + yoff;
			if (src >= 0 && src < (long) header->cupsHeight) {
				if (cupsRasterReadPixels(raster, linebuf, header->cupsBytesPerLine)
								!= header->cupsBytesPerLine) {
					fprintf(stderr, "ERROR: CAPT: broken raster file\n");
					exit(1);
				}
				++next_src;
			} else {
				memset(linebuf, 0, header->cupsBytesPerLine);
			}
			if (bitmode)
				copy_bits(bandbuf + iline * dims->line_size, dims->line_size,
					linebuf, header->cupsBytesPerLine, xoff_bits);
			else
				memcpy(bandbuf + iline * dims->line_size + shiftb, linebuf + shiftl, csize);
		}
		size = state->ops->compress_band(state, compbuf, compsize, bandbuf, dims->line_size, nlines);
		new_band = calloc(1, sizeof_struct_band_list_s(size));
		if (! new_band)
			abort();
		new_band->size = size;
		if (size)
			memcpy(new_band->data, compbuf, size);
		if (! page->bands)
			page->bands = new_band;
		if (last_band)
			last_band->next = new_band;
		last_band = new_band;
	}

	/* discard end of image, if any */
	for (i = next_src; i < header->cupsHeight; ++i)
		cupsRasterReadPixels(raster, linebuf, header->cupsBytesPerLine);

	free_buffers();
}

static void send_page_data(struct printer_state_s *state, const struct cached_page_s *page)
{
	const struct band_list_s *band;

	state->isend = 0;
	state->iband = 0;

	for (band = page->bands; band; band = band->next, ++state->iband)
		state->ops->send_band(state, band->data, band->size);
}

static void do_cancel(int s)
{
	(void) s;

	if (ops)
		ops->cancel_cleanup(state);

	if (raster) {
		cupsRasterClose(raster);
		raster = NULL;
	}

	free_buffers();

	if (cached_page) {
		free_cached_page(cached_page);
		cached_page = NULL;
	}

	if (state)
		free_state();

	exit(1);
}

static void do_print(int fd)
{
	bool in_job = false;
	ops = printer_detect();

	if (ops->alloc_state)
		state = ops->alloc_state();
	else
		state = calloc(1, sizeof(struct printer_state_s));
	state->ops = ops;
	state->ipage = 0;

	raster = cupsRasterOpen(fd, CUPS_RASTER_READ);

	fprintf(stderr, "DEBUG: CAPT: rastertocapt is rendering\n");

	while (1) {
		bool page_printed = false;

		if (! cached_page) {
			struct cups_page_header2_s header;

			if (! cupsRasterReadHeader2(raster, &header))
				break; /* no more pages */

			cached_page = calloc(1, sizeof(struct cached_page_s));
			if (! cached_page)
				abort();

			state->ipage += 1;

			page_set_dims(&cached_page->dims, &header);

			ops->page_setup(state, &cached_page->dims,
					header.cupsWidth, header.cupsHeight);

			compress_page_data(state, cached_page, raster, &header);
		}

		if (! in_job) {
			fprintf(stderr, "DEBUG: CAPT: rastertocapt: start job\n");
			if (ops->job_prologue)
				ops->job_prologue(state);
			in_job = true;
		}

		fprintf(stderr, "DEBUG: CAPT: rastertocapt: start page %u\n", state->ipage);
		if (ops->page_prologue) {
			if (cached_page->dims.manual_duplex && state->ipage > 1) {
				fprintf(stderr, "DEBUG: CAPT: rastertocapt: manual duplex: press button to continue\n");
				ops->wait_user(state);
			}
			bool ok = ops->page_prologue(state, &cached_page->dims);
			if (! ok) {
				fprintf(stderr, "DEBUG: CAPT: rastertocapt: can't start page\n");
				ops->wait_user(state);
				if (in_job) {
					fprintf(stderr, "DEBUG: CAPT: rastertocapt: retry job\n");
					if (ops->job_epilogue)
						ops->job_epilogue(state);
					in_job = false;
				}
				continue;
			}
		}

		fprintf(stderr, "DEBUG: CAPT: rastertocapt: sending page data\n");
		send_page_data(state, cached_page);

		fprintf(stderr, "DEBUG: CAPT: rastertocapt: end page %u\n", state->ipage);
		if (ops->page_epilogue) {
			bool ok = ops->page_epilogue(state, &cached_page->dims);
			if (! ok) {
				fprintf(stderr, "DEBUG: CAPT: rastertocapt: page not printed\n");
				ops->wait_user(state);
				continue;
			}
		}

		page_printed = true;

		if (page_printed) {
			while (cached_page->bands) {
				void *p = cached_page->bands;
				cached_page->bands = cached_page->bands->next;
				free(p);
			}
			free(cached_page);
			cached_page = NULL;
		}
	}

	if (in_job) {
		fprintf(stderr, "DEBUG: CAPT: rastertocapt: end job\n");
		if (ops->job_epilogue)
			ops->job_epilogue(state);
		in_job = false;
	}

	if (! state->ipage)
		fprintf(stderr, "ERROR: CAPT: no pages in job\n");

	cupsRasterClose(raster);
	free_state();
}


int main(int argc, char *argv[])
{

#if POSIX_C_SOURCE >= 199309L
	struct sigaction act_ign;
	struct sigaction act_cancel;

	/* ignore SIGPIPE */
	act_ign.sa_handler = SIG_IGN;
	sigemptyset(&act_ign.sa_mask);
	sigaction(SIGPIPE, &act_ign, NULL);
	/* handle SIGTERM */
	act_cancel.sa_handler = do_cancel();
	sigemptyset(&act_cancel.sa_mask);
	sigaddset(&act_cancel.sa_mask, SIGINT);
	sigaction(SIGTERM, &act_cancel, NULL);
#else
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, do_cancel);
#endif

	int fd = 0;

	if (argc < 6 || argc > 7) {
		fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
		return 1;
	}

	{
		cups_option_t *opts = NULL;
		int nopts = cupsParseOptions(argv[5], 0, &opts);
		const char *v;
		if ((v = cupsGetOption("capt-geometry", nopts, opts))) {
			if (! strcmp(v, "legacy"))
				capt_geometry_canon = 0;
			else if (! strcmp(v, "canon"))
				capt_geometry_canon = 1;
		}
		if ((v = cupsGetOption("capt-canon-magic", nopts, opts)) && ! strcmp(v, "off"))
			capt_canon_magic = 0;
		if ((v = cupsGetOption("capt-margin-mm", nopts, opts))) {
			double m = strtod(v, NULL);
			if (m >= 0.0 && m <= 20.0)
				capt_margin_mm = m;
		}
		if ((v = cupsGetOption("capt-xshift-mm", nopts, opts))) {
			double m = strtod(v, NULL);
			if (m >= -20.0 && m <= 20.0)
				xshift_mm = m;
		}
		if ((v = cupsGetOption("capt-yshift-mm", nopts, opts))) {
			double m = strtod(v, NULL);
			if (m >= -30.0 && m <= 30.0)
				yshift_mm = m;
		}
		cupsFreeOptions(nopts, opts);
	}

	if (argc == 7) {
		fd = open(argv[6], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "ERROR: unable to open %s: %s\n", argv[6], strerror(errno));
			return 1;
		}
	}

	fprintf(stderr, "DEBUG: CAPT: rastertocapt started\n");
	do_print(fd);
	fprintf(stderr, "DEBUG: CAPT: rastertocapt finished\n");

	if (argc == 7)
		close(fd);

	return 0;
}
