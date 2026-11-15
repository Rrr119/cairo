/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2026 The Cairo developers
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 */

#include <cairo.h>
#include <cairo-vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void
draw (cairo_t *cr, double width, double height)
{
    cairo_pattern_t *background;
    cairo_pattern_t *disc;

    background = cairo_pattern_create_linear (0, 0, width, height);
    cairo_pattern_add_color_stop_rgb (background, 0, 0.05, 0.12, 0.24);
    cairo_pattern_add_color_stop_rgb (background, 1, 0.30, 0.05, 0.20);
    cairo_set_source (cr, background);
    cairo_paint (cr);
    cairo_pattern_destroy (background);

    cairo_save (cr);
    cairo_arc (cr, width * 0.5, height * 0.5, height * 0.42,
	       0, 2 * M_PI);
    cairo_clip (cr);

    disc = cairo_pattern_create_radial (width * 0.42, height * 0.40, 4,
					width * 0.50, height * 0.50, height * 0.42);
    cairo_pattern_add_color_stop_rgba (disc, 0, 1.0, 0.95, 0.35, 1.0);
    cairo_pattern_add_color_stop_rgba (disc, 0.55, 1.0, 0.35, 0.15, 0.85);
    cairo_pattern_add_color_stop_rgba (disc, 1, 0.15, 0.05, 0.35, 0.0);
    cairo_set_source (cr, disc);
    cairo_paint (cr);
    cairo_pattern_destroy (disc);
    cairo_restore (cr);

    cairo_set_line_width (cr, 3);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.85);
    cairo_arc (cr, width * 0.5, height * 0.5, height * 0.42,
	       0, 2 * M_PI);
    cairo_stroke (cr);

    cairo_set_source_rgba (cr, 0.45, 0.9, 1.0, 0.9);
    cairo_set_line_width (cr, 2);
    cairo_move_to (cr, width * 0.20, height * 0.70);
    cairo_curve_to (cr,
		    width * 0.35, height * 0.15,
		    width * 0.65, height * 0.85,
		    width * 0.80, height * 0.30);
    cairo_stroke (cr);

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
			    CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, 20);
    cairo_move_to (cr, 20, height - 24);
    cairo_show_text (cr, "Cairo Vulkan backend");
}

#if !CAIRO_HAS_PNG_FUNCTIONS
static int
write_ppm (cairo_surface_t *surface, const char *filename)
{
    unsigned char *data;
    int width, height, stride;
    int x, y;
    FILE *file;

    cairo_surface_flush (surface);

    data = cairo_vulkan_surface_get_data (surface);
    width = cairo_vulkan_surface_get_width (surface);
    height = cairo_vulkan_surface_get_height (surface);
    stride = cairo_vulkan_surface_get_stride (surface);

    file = fopen (filename, "wb");
    if (file == NULL)
	return 0;

    fprintf (file, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; y++) {
	for (x = 0; x < width; x++) {
	    unsigned int pixel =
		*(unsigned int *) (data + y * stride + x * 4);
	    unsigned char rgb[3];

	    rgb[0] = (pixel >> 16) & 0xff;
	    rgb[1] = (pixel >> 8) & 0xff;
	    rgb[2] = pixel & 0xff;
	    fwrite (rgb, 1, sizeof (rgb), file);
	}
    }

    fclose (file);
    return 1;
}
#endif

int
main (int argc, char **argv)
{
    const char *filename = argc > 1 ? argv[1] : "vulkan-surface.png";
    const int width = 1024;
    const int height = 768;
    cairo_surface_t *surface;
    cairo_status_t status;
    cairo_t *cr;

    surface = cairo_vulkan_surface_create (width, height);
    status = cairo_surface_status (surface);
    if (status != CAIRO_STATUS_SUCCESS) {
	fprintf (stderr, "Unable to create Vulkan surface: %s\n",
		 cairo_status_to_string (status));
	cairo_surface_destroy (surface);
	return EXIT_FAILURE;
    }

    cairo_surface_set_device_scale (surface, 2, 2);

    cr = cairo_create (surface);
    draw (cr, width / 2.0, height / 2.0);
    cairo_destroy (cr);

    cairo_surface_flush (surface);

#if CAIRO_HAS_PNG_FUNCTIONS
    status = cairo_surface_write_to_png (surface, filename);
    if (status != CAIRO_STATUS_SUCCESS)
	fprintf (stderr, "Unable to write %s: %s\n", filename,
		 cairo_status_to_string (status));
#else
    status = write_ppm (surface, filename) ?
	     CAIRO_STATUS_SUCCESS : CAIRO_STATUS_WRITE_ERROR;
    if (status != CAIRO_STATUS_SUCCESS)
	fprintf (stderr, "Unable to write %s\n", filename);
#endif

    cairo_surface_destroy (surface);

    return status == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}
