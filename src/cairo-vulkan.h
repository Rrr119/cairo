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
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 */

#ifndef CAIRO_VULKAN_H
#define CAIRO_VULKAN_H

#include "cairo.h"

CAIRO_BEGIN_DECLS

#if CAIRO_HAS_VULKAN_SURFACE

/**
 * SECTION:cairo-vulkan
 * @Title: Vulkan Surfaces
 * @Short_Description: Rendering to host-visible Vulkan buffers
 * @See_Also: #cairo_surface_t
 *
 * The Vulkan surface backend rasterizes filled paths, linear and radial
 * gradients, and clipping operations with a Vulkan compute shader.  The
 * resulting ARGB32 pixels are stored in a host-visible buffer, which makes
 * the surface suitable for high-DPI offscreen rendering and for uploading
 * to a Vulkan swapchain.
 *
 * Paths are flattened on the CPU and rasterized on the GPU with coverage
 * sampling.  The number of samples follows the antialias mode requested for
 * each operation, so high-DPI output can trade quality for speed with
 * cairo_set_antialias().
 *
 * Operations that cannot be represented by the compute pipeline, such as
 * surface-pattern fills, masks, stroking, and extended blend modes, are
 * transparently rendered by the image backend into the same buffer.
 **/

/**
 * cairo_vulkan_surface_create:
 * @width: width of the surface, in pixels
 * @height: height of the surface, in pixels
 *
 * Creates a Vulkan-backed surface with an ARGB32, premultiplied pixel
 * buffer.  The surface can be used with the regular cairo drawing API.
 *
 * The pixels are kept in host-visible, host-coherent memory.  Use
 * cairo_vulkan_surface_get_data() together with cairo_surface_flush()
 * to access them directly.
 *
 * If no Vulkan compute device is available, the returned surface has the
 * status %CAIRO_STATUS_DEVICE_ERROR.
 *
 * Return value: a pointer to the newly created surface.  The caller
 * owns the returned surface and should destroy it with
 * cairo_surface_destroy().
 *
 * Since: 1.18
 **/
cairo_public cairo_surface_t *
cairo_vulkan_surface_create (int width, int height);

/**
 * cairo_vulkan_surface_get_width:
 * @surface: a Vulkan surface
 *
 * Gets the width of the Vulkan surface in pixels.
 *
 * Return value: the width of @surface, or 0 if @surface is not a Vulkan
 * surface.
 *
 * Since: 1.18
 **/
cairo_public int
cairo_vulkan_surface_get_width (cairo_surface_t *surface);

/**
 * cairo_vulkan_surface_get_height:
 * @surface: a Vulkan surface
 *
 * Gets the height of the Vulkan surface in pixels.
 *
 * Return value: the height of @surface, or 0 if @surface is not a Vulkan
 * surface.
 *
 * Since: 1.18
 **/
cairo_public int
cairo_vulkan_surface_get_height (cairo_surface_t *surface);

/**
 * cairo_vulkan_surface_get_stride:
 * @surface: a Vulkan surface
 *
 * Gets the stride of the Vulkan surface in bytes.
 *
 * Return value: the stride of @surface, or 0 if @surface is not a Vulkan
 * surface.
 *
 * Since: 1.18
 **/
cairo_public int
cairo_vulkan_surface_get_stride (cairo_surface_t *surface);

/**
 * cairo_vulkan_surface_get_data:
 * @surface: a Vulkan surface
 *
 * Gets a pointer to the pixels of the Vulkan surface.  The data is in
 * %CAIRO_FORMAT_ARGB32, in native byte order and premultiplied alpha.
 *
 * Call cairo_surface_flush() before reading from the pointer.  After
 * modifying the data, call cairo_surface_mark_dirty() before using the
 * surface with cairo again.
 *
 * Return value: a pointer to the pixel data, or %NULL if @surface is not
 * a Vulkan surface.
 *
 * Since: 1.18
 **/
cairo_public unsigned char *
cairo_vulkan_surface_get_data (cairo_surface_t *surface);

#endif /* CAIRO_HAS_VULKAN_SURFACE */

CAIRO_END_DECLS

#endif /* CAIRO_VULKAN_H */
