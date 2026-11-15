# Vulkan surface backend

Cairo includes an optional Vulkan surface backend for GPU-accelerated
offscreen rendering. It is intended for high-DPI drawing and for applications
that need a host-visible ARGB32 buffer that can be uploaded to a Vulkan
swapchain or used as a texture.

## Building

The backend is enabled automatically when the Vulkan loader development files
are available:

```sh
meson setup build -Dvulkan=enabled
meson compile -C build
```

Set `-Dvulkan=disabled` to build Cairo without the backend. Applications can
check for it with the `CAIRO_HAS_VULKAN_SURFACE` feature macro and include
`<cairo-vulkan.h>`.

## Creating a surface

```c
#include <cairo.h>
#include <cairo-vulkan.h>

cairo_surface_t *surface = cairo_vulkan_surface_create (1024, 768);
if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
    /* no Vulkan compute device is available */
}

cairo_t *cr = cairo_create (surface);
/* draw with the regular cairo API */
cairo_destroy (cr);
```

The returned surface behaves like any other cairo surface. Fills, linear and
radial gradients, and clipping are rasterized by a Vulkan compute shader.
Surface-pattern fills, masks, stroking, text, and extended blend operators are
transparently rendered by the image backend into the same pixel buffer, so
existing drawing code continues to work.

## High-DPI rendering

Create the surface at physical pixel dimensions and set the device scale to
the display scale factor:

```c
cairo_surface_set_device_scale (surface, scale, scale);
```

Drawing then uses logical coordinates while rasterization happens at the
physical resolution. The antialias mode controls coverage quality:

| Cairo antialias mode | Samples per axis |
| --- | --- |
| `CAIRO_ANTIALIAS_NONE` | 1 |
| `CAIRO_ANTIALIAS_FAST` | 2 |
| `CAIRO_ANTIALIAS_DEFAULT` / `GRAY` / `SUBPIXEL` | 4 |
| `CAIRO_ANTIALIAS_GOOD` | 4 |
| `CAIRO_ANTIALIAS_BEST` | 8 |

For UI rendering, `CAIRO_ANTIALIAS_GOOD` or `BEST` is usually the right choice
at high DPI. `FAST` reduces fragment work for animations.

## Reading pixels

The pixel buffer is allocated in host-visible, host-coherent Vulkan memory:

```c
cairo_surface_flush (surface);

int width = cairo_vulkan_surface_get_width (surface);
int height = cairo_vulkan_surface_get_height (surface);
int stride = cairo_vulkan_surface_get_stride (surface);
unsigned char *data = cairo_vulkan_surface_get_data (surface);
```

The format is `CAIRO_FORMAT_ARGB32` with premultiplied alpha in native byte
order. Call `cairo_surface_mark_dirty()` after modifying the pixels before
drawing to the surface again.

## Example

The `examples/vulkan-surface.c` program renders a high-DPI scene and writes it
to a PNG file:

```sh
./build/examples/cairo-vulkan-surface vulkan-surface.png
```

## Current limitations

- A Vulkan 1.0 compute-capable device is required. If device creation fails,
  `cairo_vulkan_surface_create()` returns a surface in the
  `CAIRO_STATUS_DEVICE_ERROR` state.
- Rendering is headless. Presentation to a window or swapchain is left to the
  application, which can upload the host-visible pixel buffer each frame.
- Surface patterns, masks, stroking, glyph rendering, and extended blend modes
  use the image backend. They remain available, but do not benefit from GPU
  acceleration.
- Gradient stops are limited to 16 per pattern for the GPU path; larger
  gradients fall back to the image backend.
