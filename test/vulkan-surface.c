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

#include <stdio.h>
#include <stdlib.h>

#define SKIP_STATUS 77

typedef struct _pixel {
    unsigned int value;
} pixel_t;

static pixel_t
get_pixel (cairo_surface_t *surface, int x, int y)
{
    unsigned char *data;
    int stride;
    pixel_t pixel;

    cairo_surface_flush (surface);

    if (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_VULKAN) {
	data = cairo_vulkan_surface_get_data (surface);
	stride = cairo_vulkan_surface_get_stride (surface);
    } else {
	data = cairo_image_surface_get_data (surface);
	stride = cairo_image_surface_get_stride (surface);
    }

    pixel.value = *(unsigned int *) (data + y * stride + x * 4);
    return pixel;
}

static int
channel (pixel_t pixel, int shift)
{
    return (pixel.value >> shift) & 0xff;
}

static int
pixel_diff (pixel_t a, pixel_t b)
{
    int diff = 0;
    int shift;

    for (shift = 0; shift < 32; shift += 8) {
	int d = abs (channel (a, shift) - channel (b, shift));
	if (d > diff)
	    diff = d;
    }

    return diff;
}

static int
check_pixel (cairo_surface_t *surface,
	     int x, int y,
	     int red, int green, int blue, int alpha,
	     int tolerance,
	     const char *name)
{
    pixel_t pixel = get_pixel (surface, x, y);
    int expected = (alpha << 24) | (red << 16) | (green << 8) | blue;

    if (pixel_diff (pixel, (pixel_t) { expected }) > tolerance) {
	fprintf (stderr,
		 "%s: pixel (%d,%d) is %08x, expected %08x\n",
		 name, x, y, pixel.value, expected);
	return 0;
    }

    return 1;
}

static int
compare_pixels (cairo_surface_t *reference,
		cairo_surface_t *surface,
		int width, int height,
		int tolerance,
		const char *name)
{
    int x, y;

    for (y = 0; y < height; y++) {
	for (x = 0; x < width; x++) {
	    pixel_t a = get_pixel (reference, x, y);
	    pixel_t b = get_pixel (surface, x, y);
	    if (pixel_diff (a, b) > tolerance) {
		fprintf (stderr,
			 "%s: pixel (%d,%d) is %08x, reference is %08x\n",
			 name, x, y, b.value, a.value);
		return 0;
	    }
	}
    }

    return 1;
}

static cairo_surface_t *
create_vulkan_surface (int width, int height)
{
    cairo_surface_t *surface = cairo_vulkan_surface_create (width, height);

    if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
	int status = cairo_surface_status (surface);
	cairo_surface_destroy (surface);

	if (status == CAIRO_STATUS_DEVICE_ERROR)
	    return NULL;

	fprintf (stderr, "cairo_vulkan_surface_create failed: %s\n",
		 cairo_status_to_string (status));
	return (cairo_surface_t *) -1;
    }

    return surface;
}

static int
test_api (cairo_surface_t *surface)
{
    cairo_rectangle_int_t extents = { 16, 16, 32, 32 };
    cairo_surface_t *similar;
    cairo_surface_t *mapped;
    cairo_t *cr;

    if (cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_VULKAN) {
	fprintf (stderr, "surface is not a Vulkan surface\n");
	return 0;
    }

    if (cairo_vulkan_surface_get_width (surface) != 128 ||
	cairo_vulkan_surface_get_height (surface) != 96 ||
	cairo_vulkan_surface_get_stride (surface) != 128 * 4 ||
	cairo_vulkan_surface_get_data (surface) == NULL)
    {
	fprintf (stderr, "invalid Vulkan surface properties\n");
	return 0;
    }

    cr = cairo_create (surface);
    if (cairo_status (cr) != CAIRO_STATUS_SUCCESS) {
	fprintf (stderr, "failed to create context: %s\n",
		 cairo_status_to_string (cairo_status (cr)));
	cairo_destroy (cr);
	return 0;
    }
    cairo_destroy (cr);

    similar = cairo_surface_create_similar (surface, CAIRO_CONTENT_COLOR_ALPHA,
					    32, 32);
    if (cairo_surface_status (similar) != CAIRO_STATUS_SUCCESS ||
	cairo_surface_get_type (similar) != CAIRO_SURFACE_TYPE_VULKAN)
    {
	fprintf (stderr, "cairo_surface_create_similar failed\n");
	cairo_surface_destroy (similar);
	return 0;
    }
    cairo_surface_destroy (similar);

    mapped = cairo_surface_map_to_image (surface, &extents);
    if (cairo_surface_status (mapped) != CAIRO_STATUS_SUCCESS ||
	cairo_surface_get_type (mapped) != CAIRO_SURFACE_TYPE_IMAGE)
    {
	fprintf (stderr, "cairo_surface_map_to_image failed\n");
	cairo_surface_destroy (mapped);
	return 0;
    }
    cairo_surface_unmap_image (surface, mapped);

    return 1;
}

static int
test_solid_fill (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    int ok;

    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_paint (cr);
    cairo_set_source_rgb (cr, 1, 0, 0);
    cairo_rectangle (cr, 16, 16, 64, 48);
    cairo_fill (cr);
    cairo_destroy (cr);

    ok = check_pixel (surface, 32, 32, 255, 0, 0, 255, 0, "solid");
    ok &= check_pixel (surface, 8, 8, 0, 0, 0, 255, 0, "solid");

    return ok;
}

static int
test_operators (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    int ok;

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);

    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba (cr, 1, 0, 0, 0.5);
    cairo_rectangle (cr, 16, 16, 32, 32);
    cairo_fill (cr);

    cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle (cr, 16, 16, 16, 16);
    cairo_fill (cr);
    cairo_destroy (cr);

    ok = check_pixel (surface, 24, 24, 0, 0, 0, 0, 0, "operator");
    ok &= check_pixel (surface, 40, 40, 128, 0, 0, 128, 1, "operator");

    return ok;
}

static void
draw_gradient_strip (cairo_t *cr)
{
    cairo_pattern_t *pattern = cairo_pattern_create_linear (0, 0, 128, 0);

    cairo_pattern_add_color_stop_rgb (pattern, 0.0, 1, 0, 0);
    cairo_pattern_add_color_stop_rgb (pattern, 0.5, 0, 1, 0);
    cairo_pattern_add_color_stop_rgb (pattern, 1.0, 0, 0, 1);
    cairo_set_source (cr, pattern);
    cairo_rectangle (cr, 0, 0, 128, 96);
    cairo_fill (cr);
    cairo_pattern_destroy (pattern);
}

static int
test_linear_gradient (cairo_surface_t *surface)
{
    cairo_surface_t *reference =
	cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 96);
    cairo_t *cr;
    int ok;

    cr = cairo_create (reference);
    draw_gradient_strip (cr);
    cairo_destroy (cr);

    cr = cairo_create (surface);
    draw_gradient_strip (cr);
    cairo_destroy (cr);

    ok = compare_pixels (reference, surface, 128, 96, 8, "linear");

    cairo_surface_destroy (reference);
    return ok;
}

static void
draw_radial_gradient (cairo_t *cr)
{
    cairo_pattern_t *pattern = cairo_pattern_create_radial (64, 48, 4, 64, 48, 52);

    cairo_pattern_add_color_stop_rgba (pattern, 0.0, 1, 1, 0, 1);
    cairo_pattern_add_color_stop_rgba (pattern, 1.0, 0, 0, 1, 1);
    cairo_set_source (cr, pattern);
    cairo_rectangle (cr, 0, 0, 128, 96);
    cairo_fill (cr);
    cairo_pattern_destroy (pattern);
}

static int
test_radial_gradient (cairo_surface_t *surface)
{
    cairo_surface_t *reference =
	cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 96);
    cairo_t *cr;
    int ok;

    cr = cairo_create (reference);
    draw_radial_gradient (cr);
    cairo_destroy (cr);

    cr = cairo_create (surface);
    draw_radial_gradient (cr);
    cairo_destroy (cr);

    ok = compare_pixels (reference, surface, 128, 96, 10, "radial");

    cairo_surface_destroy (reference);
    return ok;
}

static void
draw_clipped_shape (cairo_t *cr)
{
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);

    cairo_save (cr);
    cairo_arc (cr, 64, 48, 32, 0, 2 * 3.14159265358979323846);
    cairo_clip (cr);
    cairo_rectangle (cr, 16, 8, 96, 80);
    cairo_clip (cr);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_paint (cr);
    cairo_restore (cr);
}

static int
test_clip (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    int ok;

    draw_clipped_shape (cr);
    cairo_destroy (cr);

    ok = check_pixel (surface, 64, 48, 0, 0, 0, 255, 0, "clip");
    ok &= check_pixel (surface, 8, 48, 255, 255, 255, 255, 0, "clip");
    ok &= check_pixel (surface, 64, 8, 255, 255, 255, 255, 0, "clip");

    return ok;
}

static void
draw_fallback (cairo_t *cr)
{
    cairo_surface_t *source =
	cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 16, 16);
    cairo_t *source_cr = cairo_create (source);

    cairo_set_source_rgb (source_cr, 1, 0, 0);
    cairo_paint (source_cr);
    cairo_destroy (source_cr);

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);
    cairo_set_source_surface (cr, source, 16, 16);
    cairo_rectangle (cr, 16, 16, 16, 16);
    cairo_fill (cr);

    cairo_surface_destroy (source);
}

static int
test_fallback (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    int ok;

    draw_fallback (cr);
    cairo_destroy (cr);

    ok = check_pixel (surface, 24, 24, 255, 0, 0, 255, 0, "fallback");
    ok &= check_pixel (surface, 64, 64, 255, 255, 255, 255, 0, "fallback");

    return ok;
}

static int
test_high_dpi (cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create (surface);
    int ok;

    cairo_surface_set_device_scale (surface, 2, 2);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);
    cairo_set_antialias (cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb (cr, 0, 0, 1);
    cairo_rectangle (cr, 16, 16, 16, 16);
    cairo_fill (cr);
    cairo_destroy (cr);

    ok = check_pixel (surface, 40, 40, 0, 0, 255, 255, 0, "hidpi");
    ok &= check_pixel (surface, 12, 12, 255, 255, 255, 255, 0, "hidpi");

    return ok;
}

int
main (void)
{
    cairo_surface_t *surface;
    int ok = 1;

    surface = create_vulkan_surface (128, 96);
    if (surface == NULL) {
	printf ("Vulkan compute device unavailable, skipping test\n");
	return SKIP_STATUS;
    }
    if (surface == (cairo_surface_t *) -1)
	return EXIT_FAILURE;

    ok &= test_api (surface);
    ok &= test_solid_fill (surface);
    ok &= test_operators (surface);
    ok &= test_linear_gradient (surface);
    ok &= test_radial_gradient (surface);
    ok &= test_clip (surface);
    ok &= test_fallback (surface);
    ok &= test_high_dpi (surface);

    cairo_surface_destroy (surface);

    if (! ok)
	return EXIT_FAILURE;

    printf ("Vulkan surface tests passed\n");
    return EXIT_SUCCESS;
}
