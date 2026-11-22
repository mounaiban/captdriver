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

#pragma once

#include "std.h"

struct cups_page_header2_s;

struct page_dims_s {
	/* set by CUPS */
	unsigned media_type;
	unsigned media_adapt;
	char media_size[0x40];
	unsigned paper_width;
	unsigned paper_height;
	unsigned toner_save;
	unsigned manual_duplex;
	unsigned ink_k;
	unsigned margin_height;
	unsigned margin_width;
	/* set by printer ops */
	unsigned line_size;
	unsigned band_size;
	unsigned num_lines;
	/* Canon-style geometry: how much of the CUPS raster to skip */
	int crop_x_bits;	/* may be negative: pad instead of crop */
	int crop_y_lines;
	unsigned xdpi;
	unsigned ydpi;
};

/* 1 = Canon-style geometry, 0 = original captdriver behaviour,
 * -1 = automatic (Canon-style on printers known to need it: LBP6000) */
extern int capt_geometry_canon;
/* set by printer detection */
extern int capt_is_lbp6000;
extern double capt_margin_mm;
/* 1 = send the LBP6000 page-parameter header seen from Canon's driver */
extern int capt_canon_magic;

void page_set_dims(struct page_dims_s *dims, const struct cups_page_header2_s *header);
