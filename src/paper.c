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

#include "paper.h"
#include <cups/raster.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

int capt_geometry_canon = -1;
int capt_is_lbp6000 = 0;
double capt_margin_mm = 5.0;
int capt_canon_magic = 1;

static unsigned round4(double v)
{
	return (unsigned) (floor(v / 4.0 + 0.5) * 4.0);
}

void page_set_dims(struct page_dims_s *dims, const struct cups_page_header2_s *header)
{
	dims->media_type = header->cupsMediaType;
	strncpy(dims->media_size, header->MediaType, 64);
	dims->paper_width  = header->cupsWidth;  //header->PageSize[0] * header->HWResolution[0] / 72;
	dims->paper_height = header->cupsHeight; //header->PageSize[1] * header->HWResolution[1] / 72;
	dims->toner_save = header->cupsInteger[0];
	dims->ink_k = header->cupsInteger[1];
	dims->manual_duplex = header->cupsInteger[2];
	dims->line_size = header->PageSize[0];
	dims->num_lines = header->cupsHeight;
	dims->band_size = header->cupsRowCount;
	dims->margin_height = header->Margins[0];
	dims->margin_width = header->Margins[1];

	dims->crop_x_bits = 0;
	dims->crop_y_lines = 0;
	dims->xdpi = header->HWResolution[0];
	dims->ydpi = header->HWResolution[1];

	if (capt_geometry_canon < 0)
		capt_geometry_canon = capt_is_lbp6000 ? 1 : 0;

	if (capt_geometry_canon) {
		/*
		 * Match what Canon's own driver sends (from a USB capture of
		 * the Windows driver printing A4 on an LBP6000): paper size
		 * rounded to a multiple of 4 dots, a fixed margin on every side,
		 * and only the printable area sent as image data.
		 *   bytes 22-23: left margin in horizontal dots
		 *   bytes 24-25: top margin in vertical lines
		 */
		unsigned xdpi = dims->xdpi, ydpi = dims->ydpi;
		unsigned pw = round4(header->PageSize[0] * (double) xdpi / 72.0);
		unsigned ph = round4(header->PageSize[1] * (double) ydpi / 72.0);
		unsigned mx = (unsigned) floor(capt_margin_mm * xdpi / 25.4);
		unsigned my = (unsigned) floor(capt_margin_mm * ydpi / 25.4);
		unsigned ls = (pw - 2 * mx + 7) / 8;
		ls = (ls + 3) & ~3u;

		dims->paper_width = pw;
		dims->paper_height = ph;
		dims->margin_height = mx;	/* sent in bytes 22-23 */
		dims->margin_width = my;	/* sent in bytes 24-25 */
		dims->line_size = ls;
		dims->num_lines = ph - 2 * my;
		/*
		 * The CUPS raster may cover only the PPD's ImageableArea, not
		 * the whole sheet. Work out where the raster starts on the
		 * page so the crop is measured from the paper edge.
		 */
		int ox = 0, oy = 0;
		double full_w = header->PageSize[0] * (double) xdpi / 72.0;
		double full_h = header->PageSize[1] * (double) ydpi / 72.0;
		bool have_bbox = header->ImagingBoundingBox[2] > header->ImagingBoundingBox[0]
			&& header->ImagingBoundingBox[3] > header->ImagingBoundingBox[1];
		if (header->cupsWidth + 8 < full_w) {
			/* bbox is in whole points; if the margins are symmetric,
			 * the raster size gives the exact offset */
			if (have_bbox && abs((int) header->ImagingBoundingBox[0]
					- ((int) header->PageSize[0] - (int) header->ImagingBoundingBox[2])) > 1)
				ox = (int) floor(header->ImagingBoundingBox[0] * (double) xdpi / 72.0 + 0.5);
			else
				ox = (int) floor((full_w - header->cupsWidth) / 2.0 + 0.5);
		}
		if (header->cupsHeight + 8 < full_h) {
			if (have_bbox && abs((int) header->ImagingBoundingBox[1]
					- ((int) header->PageSize[1] - (int) header->ImagingBoundingBox[3])) > 1)
				oy = (int) floor((header->PageSize[1] - (double) header->ImagingBoundingBox[3])
						* ydpi / 72.0 + 0.5);
			else
				oy = (int) floor((full_h - header->cupsHeight) / 2.0 + 0.5);
		}
		/* the line size must not depend on the raster width */
		ls = (pw - 2 * mx + 7) / 8;
		ls = (ls + 3) & ~3u;

		dims->line_size = ls;
		dims->crop_x_bits = (int) mx - ox;
		dims->crop_y_lines = (int) my - oy;
		fprintf(stderr, "DEBUG: CAPT: raster %ux%u origin=%d,%d bbox=%u,%u,%u,%u\n",
			header->cupsWidth, header->cupsHeight, ox, oy,
			header->ImagingBoundingBox[0], header->ImagingBoundingBox[1],
			header->ImagingBoundingBox[2], header->ImagingBoundingBox[3]);
		fprintf(stderr, "DEBUG: CAPT: canon geometry paper=%ux%u margin=%u,%u image=%u bytes x %u lines crop=%d,%d\n",
			pw, ph, mx, my, ls, dims->num_lines, dims->crop_x_bits, dims->crop_y_lines);
	}

	if (header->HWResolution[1] == 400)
	  dims->media_adapt  = 0x81;
	else
	  dims->media_adapt  = 0x11;
}
