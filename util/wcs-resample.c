/*
  This file is part of the Astrometry.net suite.
  Copyright 2009, 2010 Dustin Lang.

  The Astrometry.net suite is free software; you can redistribute
  it and/or modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation, version 2.

  The Astrometry.net suite is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the Astrometry.net suite ; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/param.h>
#include <assert.h>

#include "wcs-resample.h"
#include "sip_qfits.h"
#include "an-bool.h"
#include "qfits.h"
#include "starutil.h"
#include "bl.h"
#include "boilerplate.h"
#include "log.h"
#include "errors.h"
#include "fitsioutils.h"
#include "anwcs.h"
#include "resample.h"

int resample_wcs_files(const char* infitsfn, int infitsext,
					   const char* inwcsfn, int inwcsext,
					   const char* outwcsfn, int outwcsext,
					   const char* outfitsfn, int lorder) {

    anwcs_t* inwcs;
    anwcs_t* outwcs;
    qfitsloader qinimg;
    qfitsdumper qoutimg;
    float* inimg;
    float* outimg;
    qfits_header* hdr;

    int outW, outH;
    int inW, inH;

    double outpixmin, outpixmax;

	// read input WCS.
	inwcs = anwcs_open(inwcsfn, inwcsext);
    if (!inwcs) {
        ERROR("Failed to parse WCS header from %s extension %i", inwcsfn, inwcsext);
		return -1;
    }

	// read output WCS.
	outwcs = anwcs_open(outwcsfn, outwcsext);
    if (!outwcs) {
        ERROR("Failed to parse WCS header from %s extension %i", outwcsfn, outwcsext);
		return -1;
    }

    outW = anwcs_imagew(outwcs);
    outH = anwcs_imageh(outwcs);

    // read input image.
    memset(&qinimg, 0, sizeof(qinimg));
    qinimg.filename = (char*)infitsfn;
    // primary extension
    qinimg.xtnum = infitsext;
    // first pixel plane
    qinimg.pnum = 0;
    // read as floats
    qinimg.ptype = PTYPE_FLOAT;

    if (qfitsloader_init(&qinimg) ||
        qfits_loadpix(&qinimg)) {
        ERROR("Failed to read pixels from input FITS image \"%s\"", infitsfn);
		return -1;
    }

    // lx, ly, fbuf
    inimg = qinimg.fbuf;
    assert(inimg);
    inW = qinimg.lx;
    inH = qinimg.ly;

    logmsg("Input  image is %i x %i pixels.\n", inW, inH);
    logmsg("Output image is %i x %i pixels.\n", outW, outH);

    outimg = calloc(outW * outH, sizeof(float));

	if (resample_wcs(inwcs, inimg, inW, inH,
					 outwcs, outimg, outW, outH, 1, lorder)) {
		ERROR("Failed to resample");
		return -1;
	}

    {
        double pmin, pmax;
		int i;
		/*
		 pmin =  HUGE_VAL;
		 pmax = -HUGE_VAL;
		 for (i=0; i<(inW*inH); i++) {
		 pmin = MIN(pmin, inimg[i]);
		 pmax = MAX(pmax, inimg[i]);
		 }
		 logmsg("Input image bounds: %g to %g\n", pmin, pmax);
		 */
        pmin =  HUGE_VAL;
        pmax = -HUGE_VAL;
        for (i=0; i<(outW*outH); i++) {
            pmin = MIN(pmin, outimg[i]);
            pmax = MAX(pmax, outimg[i]);
        }
        logmsg("Output image bounds: %g to %g\n", pmin, pmax);
        outpixmin = pmin;
        outpixmax = pmax;
	 }

    qfitsloader_free_buffers(&qinimg);

    // prepare output image.
    memset(&qoutimg, 0, sizeof(qoutimg));
    qoutimg.filename = outfitsfn;
    qoutimg.npix = outW * outH;
    qoutimg.ptype = PTYPE_FLOAT;
    qoutimg.fbuf = outimg;
    qoutimg.out_ptype = BPP_IEEE_FLOAT;

    hdr = fits_get_header_for_image(&qoutimg, outW, NULL);
	anwcs_add_to_header(outwcs, hdr);
    fits_header_add_double(hdr, "DATAMIN", outpixmin, "min pixel value");
    fits_header_add_double(hdr, "DATAMAX", outpixmax, "max pixel value");

	if (fits_write_header_and_image(hdr, &qoutimg, 0)) {
        ERROR("Failed to write image to file \"%s\"", outfitsfn);
		return -1;
	}
    free(outimg);
	qfits_header_destroy(hdr);

	anwcs_free(inwcs);
	anwcs_free(outwcs);

	return 0;
}

// Check whether output pixels overlap with input pixels,
// on a grid of output pixel positions.
static bool* find_overlap_grid(int B, int outW, int outH,
							   const anwcs_t* outwcs, const anwcs_t* inwcs,
							   int* pBW, int* pBH) {
	int BW, BH;
	bool* bib = NULL;
	bool* bib2 = NULL;
	int i,j;

	BW = (int)ceil(outW / (float)B);
	BH = (int)ceil(outH / (float)B);
	bib = calloc(BW*BH, sizeof(bool));
	for (i=0; i<BH; i++) {
		for (j=0; j<BW; j++) {
			int x,y;
			double ra,dec;
			y = MIN(outH-1, B*i);
			x = MIN(outW-1, B*j);
            if (anwcs_pixelxy2radec(outwcs, x+1, y+1, &ra, &dec))
				continue;
			bib[i*BW+j] = anwcs_radec_is_inside_image(inwcs, ra, dec);
		}
	}
	if (log_get_level() >= LOG_VERB) {
		logverb("Input image overlaps output image:\n");
		for (i=0; i<BH; i++) {
			for (j=0; j<BW; j++)
				logverb((bib[i*BW+j]) ? "*" : ".");
			logverb("\n");
		}
	}
	// Grow the in-bounds area:
	bib2 = calloc(BW*BH, sizeof(bool));
	for (i=0; i<BH; i++)
		for (j=0; j<BW; j++) {
			int di,dj;
			if (!bib[i*BW+j])
				continue;
			for (di=-1; di<=1; di++)
				for (dj=-1; dj<=1; dj++)
					bib2[(MIN(MAX(i+di, 0), BH-1))*BW + (MIN(MAX(j+dj, 0), BW-1))] = TRUE;
		}
	// swap!
	free(bib);
	bib = bib2;
	bib2 = NULL;

	if (log_get_level() >= LOG_VERB) {
		logverb("After growing:\n");
		for (i=0; i<BH; i++) {
			for (j=0; j<BW; j++)
				logverb((bib[i*BW+j]) ? "*" : ".");
			logverb("\n");
		}
	}

	*pBW = BW;
	*pBH = BH;
	return bib;
}



int resample_wcs(const anwcs_t* inwcs, const float* inimg, int inW, int inH,
				 const anwcs_t* outwcs, float* outimg, int outW, int outH,
				 int overlap_grid, int lorder) {
	int i,j;
    //double inxmin, inxmax, inymin, inymax;
	int B = 20;
	int BW, BH;
	bool* bib = NULL;
	int bi,bj;
	lanczos_args_t largs;
	largs.order = lorder;

	if (overlap_grid)
		bib = find_overlap_grid(B, outW, outH, outwcs, inwcs, &BW, &BH);
	else {
		BH = outH;
		BW = outW;
		B = 1;
	}
	/*
	 inxmax = -HUGE_VAL;
	 inymax = -HUGE_VAL;
	 inxmin =  HUGE_VAL;
	 inymin =  HUGE_VAL;
	 */

	// We've expanded the in-bounds boxes by 1 in each direction,
	// so this (using the lower-left corner) should be ok.
	for (bj=0; bj<BH; bj++) {
		for (bi=0; bi<BW; bi++) {
			int jlo,jhi,ilo,ihi;
			if (overlap_grid && !bib[bj*BW + bi])
				continue;
			jlo = MIN(outH,  bj   *B);
			jhi = MIN(outH, (bj+1)*B);
			ilo = MIN(outW,  bi   *B);
			ihi = MIN(outW, (bi+1)*B);

			for (j=jlo; j<jhi; j++) {
				for (i=ilo; i<ihi; i++) {

					double xyz[3];
					double inx, iny;
					float pix;
					// +1 for FITS pixel coordinates.
					if (anwcs_pixelxy2xyz(outwcs, i+1, j+1, xyz) ||
						anwcs_xyz2pixelxy(inwcs, xyz, &inx, &iny))
						continue;

					inx -= 1.0;
					iny -= 1.0;

					if (lorder == 0) {
						int x,y;
						// Nearest-neighbour resampling
						x = round(inx);
						y = round(iny);
						if (x < 0 || x >= inW || y < 0 || y >= inH)
							continue;
						pix = inimg[y * inW + x];

						/*
						 // keep track of the bounds of the requested pixels in the
						 // input image.
						 inxmax = MAX(inxmax, x);
						 inymax = MAX(inymax, y);
						 inxmin = MIN(inxmin, x);
						 inymin = MIN(inymin, y);
						 */
					} else {
						if (inx < (-lorder) || inx >= (inW+lorder) ||
							iny < (-lorder) || iny >= (inH+lorder))
							continue;
						pix = lanczos_resample_unw_sep_f(inx, iny, inimg, inW, inH, &largs);
					}
					outimg[j * outW + i] = pix;
				}
			}
		}
	}

	if (bib)
		free(bib);

	/*
	 logverb("Bounds of the pixels requested from the input image:\n");
	 logverb("  x: %g to %g\n", inxmin, inxmax);
	 logverb("  y: %g to %g\n", inymin, inymax);
	 */

	return 0;
}


int resample_wcs_rgba(const anwcs_t* inwcs, const unsigned char* inimg,
					  int inW, int inH,
					  const anwcs_t* outwcs, unsigned char* outimg,
					  int outW, int outH) {
	int i,j;
	int B = 20;
	int BW, BH;
	bool* bib;
	int bi,bj;

	bib = find_overlap_grid(B, outW, outH, outwcs, inwcs, &BW, &BH);

	// We've expanded the in-bounds boxes by 1 in each direction,
	// so this (using the lower-left corner) should be ok.
	for (bj=0; bj<BH; bj++) {
		for (bi=0; bi<BW; bi++) {
			int jlo,jhi,ilo,ihi;
			if (!bib[bj*BW + bi])
				continue;
			jlo = MIN(outH,  bj   *B);
			jhi = MIN(outH, (bj+1)*B);
			ilo = MIN(outW,  bi   *B);
			ihi = MIN(outW, (bi+1)*B);
			for (j=jlo; j<jhi; j++) {
				for (i=ilo; i<ihi; i++) {
					double xyz[3];
					double inx, iny;
					int x,y;
					// +1 for FITS pixel coordinates.
					if (anwcs_pixelxy2xyz(outwcs, i+1, j+1, xyz) ||
						anwcs_xyz2pixelxy(inwcs, xyz, &inx, &iny))
						continue;
					// FIXME - Nearest-neighbour resampling!!
					// -1 for FITS pixel coordinates.
					x = round(inx - 1.0);
					y = round(iny - 1.0);
					if (x < 0 || x >= inW || y < 0 || y >= inH)
						continue;
					// HACK -- straight copy
					outimg[4 * (j * outW + i) + 0] = inimg[4 * (y * inW + x) + 0];
					outimg[4 * (j * outW + i) + 1] = inimg[4 * (y * inW + x) + 1];
					outimg[4 * (j * outW + i) + 2] = inimg[4 * (y * inW + x) + 2];
					outimg[4 * (j * outW + i) + 3] = inimg[4 * (y * inW + x) + 3];
				}
			}
		}
	}

	free(bib);

	return 0;

}
