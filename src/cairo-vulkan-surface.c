/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
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

#include "cairoint.h"

#include "cairo-clip-private.h"
#include "cairo-default-context-private.h"
#include "cairo-error-private.h"
#include "cairo-fixed-private.h"
#include "cairo-image-surface-private.h"
#include "cairo-mutex-private.h"
#include "cairo-path-fixed-private.h"
#include "cairo-pattern-private.h"
#include "cairo-region-private.h"
#include "cairo-surface-backend-private.h"
#include "cairo-surface-private.h"
#include "cairo-vulkan.h"
#include "cairo-vulkan-shader.h"

#include <vulkan/vulkan.h>

#include <string.h>

#define CAIRO_VULKAN_MAX_STOPS 16
#define CAIRO_VULKAN_WORKGROUP_SIZE 8

typedef enum {
    CAIRO_VULKAN_PAINT_SOLID = 0,
    CAIRO_VULKAN_PAINT_LINEAR = 1,
    CAIRO_VULKAN_PAINT_RADIAL = 2
} cairo_vulkan_paint_type_t;

typedef struct _cairo_vulkan_edge {
    float x0, y0, x1, y1;
} cairo_vulkan_edge_t;

typedef struct _cairo_vulkan_path {
    uint32_t first_edge;
    uint32_t num_edges;
    uint32_t fill_rule;
    uint32_t samples;
} cairo_vulkan_path_t;

typedef struct _cairo_vulkan_params {
    int32_t surface_width;
    int32_t surface_height;
    int32_t offset_x;
    int32_t offset_y;

    uint32_t paint_type;
    uint32_t op;
    uint32_t n_stops;
    uint32_t has_fill;

    uint32_t extend;
    uint32_t num_clip_paths;
    uint32_t num_clip_rects;
    uint32_t opaque;

    float opacity;
    float pad1;
    float pad2;
    float pad3;

    float solid[4];
    float p1[4];
    float p2[4];
    float matrix[16];
    float stops[CAIRO_VULKAN_MAX_STOPS][4];
    float stop_offsets[CAIRO_VULKAN_MAX_STOPS];
} cairo_vulkan_params_t;

typedef char cairo_vulkan_params_size_check[
    sizeof (cairo_vulkan_params_t) == 496 ? 1 : -1];

typedef struct _cairo_vulkan_edge_list {
    cairo_vulkan_edge_t *edges;
    uint32_t num_edges;
    uint32_t capacity;
} cairo_vulkan_edge_list_t;

typedef struct _cairo_vulkan_path_list {
    cairo_vulkan_path_t *paths;
    uint32_t num_paths;
    uint32_t capacity;
} cairo_vulkan_path_list_t;

typedef struct _cairo_vulkan_rect_list {
    float *rects;
    uint32_t num_rects;
    uint32_t capacity;
} cairo_vulkan_rect_list_t;

typedef struct _cairo_vulkan_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize capacity;
} cairo_vulkan_buffer_t;

typedef struct _cairo_vulkan_device {
    int ref_count;

    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkDeviceSize max_storage_buffer_range;
    uint32_t max_workgroup_count[3];

    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
} cairo_vulkan_device_t;

typedef struct _cairo_vulkan_surface {
    cairo_surface_t base;

    cairo_vulkan_device_t *device;
    cairo_image_surface_t *image;

    unsigned char *pixels;
    int width;
    int height;
    int stride;

    cairo_vulkan_buffer_t pixel_buffer;
    cairo_vulkan_buffer_t edge_buffer;
    cairo_vulkan_buffer_t path_buffer;
    cairo_vulkan_buffer_t clip_rect_buffer;
    cairo_vulkan_buffer_t params_buffer;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    cairo_bool_t pending;
} cairo_vulkan_surface_t;

typedef struct _cairo_vulkan_flatten_context {
    cairo_vulkan_edge_list_t *edges;
    cairo_point_t current;
    cairo_point_t start;
    cairo_bool_t has_subpath;
} cairo_vulkan_flatten_context_t;

static cairo_vulkan_device_t *_cairo_vulkan_device;

static const cairo_surface_backend_t _cairo_vulkan_surface_backend;

static cairo_vulkan_surface_t *
_cairo_vulkan_surface_from_surface (cairo_surface_t *surface)
{
    if (surface == NULL ||
	surface->status != CAIRO_STATUS_SUCCESS ||
	surface->backend != &_cairo_vulkan_surface_backend)
    {
	return NULL;
    }

    return (cairo_vulkan_surface_t *) surface;
}

static cairo_int_status_t
_cairo_vulkan_buffer_fini (cairo_vulkan_device_t *device,
			   cairo_vulkan_buffer_t *buffer)
{
    if (buffer->mapped != NULL) {
	vkUnmapMemory (device->device, buffer->memory);
	buffer->mapped = NULL;
    }

    if (buffer->buffer != VK_NULL_HANDLE) {
	vkDestroyBuffer (device->device, buffer->buffer, NULL);
	buffer->buffer = VK_NULL_HANDLE;
    }

    if (buffer->memory != VK_NULL_HANDLE) {
	vkFreeMemory (device->device, buffer->memory, NULL);
	buffer->memory = VK_NULL_HANDLE;
    }

    buffer->capacity = 0;

    return CAIRO_INT_STATUS_SUCCESS;
}

static uint32_t
_cairo_vulkan_find_memory_type (cairo_vulkan_device_t *device,
				uint32_t type_bits,
				VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties (device->physical_device,
					 &memory_properties);

    for (i = 0; i < memory_properties.memoryTypeCount; i++) {
	if ((type_bits & (1u << i)) == 0)
	    continue;

	if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
	    return i;
    }

    return UINT32_MAX;
}

static cairo_int_status_t
_cairo_vulkan_buffer_ensure (cairo_vulkan_surface_t *surface,
			     cairo_vulkan_buffer_t *buffer,
			     VkDeviceSize size)
{
    cairo_vulkan_device_t *device = surface->device;
    VkBufferCreateInfo buffer_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate_info;
    VkDeviceSize needed;
    uint32_t memory_type;
    VkResult result;

    if (size == 0)
	size = 1;

    if (size > device->max_storage_buffer_range)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    if (buffer->buffer != VK_NULL_HANDLE && buffer->capacity >= size)
	return CAIRO_INT_STATUS_SUCCESS;

    _cairo_vulkan_buffer_fini (device, buffer);

    needed = size;
    if (needed < 256)
	needed = 256;

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = needed;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    result = vkCreateBuffer (device->device, &buffer_info, NULL, &buffer->buffer);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    vkGetBufferMemoryRequirements (device->device, buffer->buffer,
				   &requirements);

    memory_type = _cairo_vulkan_find_memory_type (device,
						  requirements.memoryTypeBits,
						  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
						  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
	_cairo_vulkan_buffer_fini (device, buffer);
	return CAIRO_INT_STATUS_DEVICE_ERROR;
    }

    memset (&allocate_info, 0, sizeof (allocate_info));
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;

    result = vkAllocateMemory (device->device, &allocate_info, NULL,
			       &buffer->memory);
    if (result != VK_SUCCESS) {
	_cairo_vulkan_buffer_fini (device, buffer);
	return CAIRO_INT_STATUS_DEVICE_ERROR;
    }

    result = vkBindBufferMemory (device->device, buffer->buffer,
				 buffer->memory, 0);
    if (result != VK_SUCCESS) {
	_cairo_vulkan_buffer_fini (device, buffer);
	return CAIRO_INT_STATUS_DEVICE_ERROR;
    }

    result = vkMapMemory (device->device, buffer->memory, 0,
			  requirements.size, 0, &buffer->mapped);
    if (result != VK_SUCCESS) {
	_cairo_vulkan_buffer_fini (device, buffer);
	return CAIRO_INT_STATUS_DEVICE_ERROR;
    }

    buffer->capacity = requirements.size;

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_vulkan_device_t *
_cairo_vulkan_device_create (void)
{
    cairo_vulkan_device_t *device;
    VkApplicationInfo application_info;
    VkInstanceCreateInfo instance_info;
    VkPhysicalDevice *physical_devices = NULL;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t num_devices = 0;
    uint32_t chosen_family = 0;
    uint32_t chosen_score = 0;
    uint32_t i, j;
    VkResult result;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    float priority = 1.0f;
    VkDescriptorSetLayoutBinding bindings[5];
    VkDescriptorSetLayoutCreateInfo set_layout_info;
    VkPipelineLayoutCreateInfo pipeline_layout_info;
    VkShaderModule shader_module;
    VkShaderModuleCreateInfo shader_info;
    VkComputePipelineCreateInfo pipeline_info;

    device = _cairo_calloc (sizeof (cairo_vulkan_device_t));
    if (device == NULL)
	return NULL;

    device->ref_count = 0;

    memset (&application_info, 0, sizeof (application_info));
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "cairo";
    application_info.applicationVersion = VK_MAKE_VERSION (1, 18, 8);
    application_info.pEngineName = "cairo-vulkan";
    application_info.engineVersion = VK_MAKE_VERSION (1, 18, 8);
    application_info.apiVersion = VK_API_VERSION_1_0;

    memset (&instance_info, 0, sizeof (instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    result = vkCreateInstance (&instance_info, NULL, &device->instance);
    if (result != VK_SUCCESS)
	goto fail;

    result = vkEnumeratePhysicalDevices (device->instance, &num_devices, NULL);
    if (result != VK_SUCCESS || num_devices == 0)
	goto fail;

    physical_devices = _cairo_malloc_ab (num_devices, sizeof (VkPhysicalDevice));
    if (physical_devices == NULL)
	goto fail;

    result = vkEnumeratePhysicalDevices (device->instance, &num_devices,
					 physical_devices);
    if (result != VK_SUCCESS)
	goto fail;

    for (i = 0; i < num_devices; i++) {
	uint32_t num_families = 0;
	VkQueueFamilyProperties *families;
	uint32_t score;

	vkGetPhysicalDeviceProperties (physical_devices[i], &properties);

	switch (properties.deviceType) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
	    score = 4;
	    break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
	    score = 3;
	    break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
	    score = 2;
	    break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
	    score = 1;
	    break;
	case VK_PHYSICAL_DEVICE_TYPE_OTHER:
	case VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM:
	default:
	    score = 0;
	    break;
	}

	if (score <= chosen_score && chosen != VK_NULL_HANDLE)
	    continue;

	vkGetPhysicalDeviceQueueFamilyProperties (physical_devices[i],
						  &num_families, NULL);
	if (num_families == 0)
	    continue;

	families = _cairo_malloc_ab (num_families,
				     sizeof (VkQueueFamilyProperties));
	if (families == NULL)
	    goto fail;

	vkGetPhysicalDeviceQueueFamilyProperties (physical_devices[i],
						  &num_families, families);
	for (j = 0; j < num_families; j++) {
	    if ((families[j].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
		chosen = physical_devices[i];
		chosen_family = j;
		chosen_score = score;
		vkGetPhysicalDeviceProperties (chosen, &properties);
		break;
	    }
	}

	free (families);
    }

    free (physical_devices);
    physical_devices = NULL;

    if (chosen == VK_NULL_HANDLE)
	goto fail;

    device->physical_device = chosen;
    device->queue_family = chosen_family;
    device->max_storage_buffer_range = properties.limits.maxStorageBufferRange;
    device->max_workgroup_count[0] = properties.limits.maxComputeWorkGroupCount[0];
    device->max_workgroup_count[1] = properties.limits.maxComputeWorkGroupCount[1];
    device->max_workgroup_count[2] = properties.limits.maxComputeWorkGroupCount[2];

    memset (&queue_info, 0, sizeof (queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = chosen_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    memset (&device_info, 0, sizeof (device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    result = vkCreateDevice (chosen, &device_info, NULL, &device->device);
    if (result != VK_SUCCESS)
	goto fail;

    vkGetDeviceQueue (device->device, chosen_family, 0, &device->queue);

    memset (bindings, 0, sizeof (bindings));
    for (i = 0; i < 5; i++) {
	bindings[i].binding = i;
	bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[i].descriptorCount = 1;
	bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    memset (&set_layout_info, 0, sizeof (set_layout_info));
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 5;
    set_layout_info.pBindings = bindings;

    result = vkCreateDescriptorSetLayout (device->device, &set_layout_info,
					  NULL, &device->descriptor_set_layout);
    if (result != VK_SUCCESS)
	goto fail;

    memset (&pipeline_layout_info, 0, sizeof (pipeline_layout_info));
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &device->descriptor_set_layout;

    result = vkCreatePipelineLayout (device->device, &pipeline_layout_info,
				     NULL, &device->pipeline_layout);
    if (result != VK_SUCCESS)
	goto fail;

    memset (&shader_info, 0, sizeof (shader_info));
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = _cairo_vulkan_shader_spv_size;
    shader_info.pCode = _cairo_vulkan_shader_spv;

    result = vkCreateShaderModule (device->device, &shader_info, NULL,
				   &shader_module);
    if (result != VK_SUCCESS)
	goto fail;

    memset (&pipeline_info, 0, sizeof (pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = shader_module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = device->pipeline_layout;

    result = vkCreateComputePipelines (device->device, VK_NULL_HANDLE, 1,
				       &pipeline_info, NULL, &device->pipeline);
    vkDestroyShaderModule (device->device, shader_module, NULL);
    if (result != VK_SUCCESS)
	goto fail;

    return device;

fail:
    free (physical_devices);

    if (device->device != VK_NULL_HANDLE)
	vkDestroyDevice (device->device, NULL);
    if (device->instance != VK_NULL_HANDLE)
	vkDestroyInstance (device->instance, NULL);

    free (device);

    return NULL;
}

static void
_cairo_vulkan_device_destroy (cairo_vulkan_device_t *device)
{
    vkDeviceWaitIdle (device->device);

    if (device->pipeline != VK_NULL_HANDLE)
	vkDestroyPipeline (device->device, device->pipeline, NULL);
    if (device->pipeline_layout != VK_NULL_HANDLE)
	vkDestroyPipelineLayout (device->device, device->pipeline_layout, NULL);
    if (device->descriptor_set_layout != VK_NULL_HANDLE)
	vkDestroyDescriptorSetLayout (device->device,
				      device->descriptor_set_layout, NULL);

    vkDestroyDevice (device->device, NULL);
    vkDestroyInstance (device->instance, NULL);
    free (device);
}

static cairo_vulkan_device_t *
_cairo_vulkan_device_acquire (void)
{
    CAIRO_MUTEX_LOCK (_cairo_vulkan_mutex);

    if (_cairo_vulkan_device == NULL)
	_cairo_vulkan_device = _cairo_vulkan_device_create ();

    if (_cairo_vulkan_device != NULL)
	_cairo_vulkan_device->ref_count++;

    CAIRO_MUTEX_UNLOCK (_cairo_vulkan_mutex);

    return _cairo_vulkan_device;
}

static void
_cairo_vulkan_device_release (cairo_vulkan_device_t *device)
{
    cairo_bool_t destroy = FALSE;

    CAIRO_MUTEX_LOCK (_cairo_vulkan_mutex);

    if (device != NULL && --device->ref_count == 0) {
	if (_cairo_vulkan_device == device)
	    _cairo_vulkan_device = NULL;
	destroy = TRUE;
    }

    CAIRO_MUTEX_UNLOCK (_cairo_vulkan_mutex);

    if (destroy)
	_cairo_vulkan_device_destroy (device);
}

static cairo_int_status_t
_cairo_vulkan_surface_wait (cairo_vulkan_surface_t *surface)
{
    VkResult result;

    if (surface->device == NULL || surface->fence == VK_NULL_HANDLE ||
	! surface->pending)
	return CAIRO_INT_STATUS_SUCCESS;

    result = vkWaitForFences (surface->device->device, 1, &surface->fence,
			      VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    surface->pending = FALSE;

    return CAIRO_INT_STATUS_SUCCESS;
}

static void
_cairo_vulkan_edge_list_fini (cairo_vulkan_edge_list_t *list)
{
    free (list->edges);
    list->edges = NULL;
    list->num_edges = 0;
    list->capacity = 0;
}

static cairo_int_status_t
_cairo_vulkan_edge_list_append (cairo_vulkan_edge_list_t *list,
				float x0, float y0, float x1, float y1)
{
    cairo_vulkan_edge_t *edge;
    uint32_t capacity;

    if (list->num_edges == list->capacity) {
	capacity = list->capacity == 0 ? 256 : list->capacity * 2;
	edge = _cairo_realloc_ab (list->edges, capacity,
				  sizeof (cairo_vulkan_edge_t));
	if (unlikely (edge == NULL))
	    return CAIRO_INT_STATUS_NO_MEMORY;

	list->edges = edge;
	list->capacity = capacity;
    }

    edge = &list->edges[list->num_edges++];
    edge->x0 = x0;
    edge->y0 = y0;
    edge->x1 = x1;
    edge->y1 = y1;

    return CAIRO_INT_STATUS_SUCCESS;
}

static void
_cairo_vulkan_path_list_fini (cairo_vulkan_path_list_t *list)
{
    free (list->paths);
    list->paths = NULL;
    list->num_paths = 0;
    list->capacity = 0;
}

static cairo_int_status_t
_cairo_vulkan_path_list_append (cairo_vulkan_path_list_t *list,
				uint32_t first_edge,
				uint32_t num_edges,
				cairo_fill_rule_t fill_rule,
				unsigned int samples)
{
    cairo_vulkan_path_t *path;
    uint32_t capacity;

    if (list->num_paths == list->capacity) {
	capacity = list->capacity == 0 ? 8 : list->capacity * 2;
	path = _cairo_realloc_ab (list->paths, capacity,
				  sizeof (cairo_vulkan_path_t));
	if (unlikely (path == NULL))
	    return CAIRO_INT_STATUS_NO_MEMORY;

	list->paths = path;
	list->capacity = capacity;
    }

    path = &list->paths[list->num_paths++];
    path->first_edge = first_edge;
    path->num_edges = num_edges;
    path->fill_rule = fill_rule;
    path->samples = samples;

    return CAIRO_INT_STATUS_SUCCESS;
}

static void
_cairo_vulkan_rect_list_fini (cairo_vulkan_rect_list_t *list)
{
    free (list->rects);
    list->rects = NULL;
    list->num_rects = 0;
    list->capacity = 0;
}

static cairo_int_status_t
_cairo_vulkan_rect_list_append (cairo_vulkan_rect_list_t *list,
				float x0, float y0, float x1, float y1)
{
    float *rect;
    uint32_t capacity;

    if (list->num_rects == list->capacity) {
	capacity = list->capacity == 0 ? 8 : list->capacity * 2;
	rect = _cairo_realloc_ab (list->rects, capacity * 4, sizeof (float));
	if (unlikely (rect == NULL))
	    return CAIRO_INT_STATUS_NO_MEMORY;

	list->rects = rect;
	list->capacity = capacity;
    }

    rect = &list->rects[list->num_rects * 4];
    rect[0] = x0;
    rect[1] = y0;
    rect[2] = x1;
    rect[3] = y1;
    list->num_rects++;

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_vulkan_add_line (cairo_vulkan_flatten_context_t *context,
			const cairo_point_t *p0,
			const cairo_point_t *p1)
{
    return _cairo_vulkan_edge_list_append (context->edges,
					   _cairo_fixed_to_double (p0->x),
					   _cairo_fixed_to_double (p0->y),
					   _cairo_fixed_to_double (p1->x),
					   _cairo_fixed_to_double (p1->y));
}

static cairo_status_t
_cairo_vulkan_flatten_move_to (void *closure, const cairo_point_t *point)
{
    cairo_vulkan_flatten_context_t *context = closure;

    if (context->has_subpath) {
	cairo_int_status_t status = _cairo_vulkan_add_line (context,
							    &context->current,
							    &context->start);
	if (unlikely (status))
	    return status;
    }

    context->start = *point;
    context->current = *point;
    context->has_subpath = TRUE;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_vulkan_flatten_line_to (void *closure, const cairo_point_t *point)
{
    cairo_vulkan_flatten_context_t *context = closure;
    cairo_int_status_t status;

    if (! context->has_subpath) {
	context->start = *point;
	context->current = *point;
	context->has_subpath = TRUE;
	return CAIRO_STATUS_SUCCESS;
    }

    status = _cairo_vulkan_add_line (context, &context->current, point);
    if (unlikely (status))
	return status;

    context->current = *point;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_vulkan_flatten_close_path (void *closure)
{
    cairo_vulkan_flatten_context_t *context = closure;
    cairo_int_status_t status;

    if (! context->has_subpath)
	return CAIRO_STATUS_SUCCESS;

    status = _cairo_vulkan_add_line (context, &context->current,
				     &context->start);
    if (unlikely (status))
	return status;

    context->current = context->start;
    context->has_subpath = FALSE;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_vulkan_flatten_path (const cairo_path_fixed_t *path,
			    double tolerance,
			    cairo_vulkan_edge_list_t *edges,
			    uint32_t *first_edge,
			    uint32_t *num_edges)
{
    cairo_vulkan_flatten_context_t context;
    cairo_status_t status;

    *first_edge = edges->num_edges;

    memset (&context, 0, sizeof (context));
    context.edges = edges;

    status = _cairo_path_fixed_interpret_flat (path,
					       _cairo_vulkan_flatten_move_to,
					       _cairo_vulkan_flatten_line_to,
					       _cairo_vulkan_flatten_close_path,
					       &context,
					       tolerance);
    if (unlikely (status))
	return status;

    if (context.has_subpath) {
	status = _cairo_vulkan_add_line (&context, &context.current,
					 &context.start);
	if (unlikely (status))
	    return status;
    }

    *num_edges = edges->num_edges - *first_edge;

    return CAIRO_INT_STATUS_SUCCESS;
}

static unsigned int
_cairo_vulkan_samples_for_antialias (cairo_antialias_t antialias)
{
    switch (antialias) {
    case CAIRO_ANTIALIAS_NONE:
	return 1;
    case CAIRO_ANTIALIAS_FAST:
	return 2;
    case CAIRO_ANTIALIAS_GOOD:
	return 4;
    case CAIRO_ANTIALIAS_BEST:
	return 8;
    case CAIRO_ANTIALIAS_DEFAULT:
    case CAIRO_ANTIALIAS_GRAY:
    case CAIRO_ANTIALIAS_SUBPIXEL:
    default:
	return 4;
    }
}

static void
_cairo_vulkan_set_color (double red, double green, double blue, double alpha,
			 float *rgba)
{
    alpha = _cairo_restrict_value (alpha, 0.0, 1.0);

    rgba[0] = red * alpha;
    rgba[1] = green * alpha;
    rgba[2] = blue * alpha;
    rgba[3] = alpha;
}

static cairo_bool_t
_cairo_vulkan_pattern_supported (const cairo_pattern_t *pattern)
{
    if (pattern->has_component_alpha)
	return FALSE;

    switch (pattern->type) {
    case CAIRO_PATTERN_TYPE_SOLID:
	return TRUE;
    case CAIRO_PATTERN_TYPE_LINEAR:
    case CAIRO_PATTERN_TYPE_RADIAL: {
	const cairo_gradient_pattern_t *gradient =
	    (const cairo_gradient_pattern_t *) pattern;
	return gradient->n_stops > 0 &&
	       gradient->n_stops <= CAIRO_VULKAN_MAX_STOPS;
    }
    case CAIRO_PATTERN_TYPE_SURFACE:
    case CAIRO_PATTERN_TYPE_MESH:
    case CAIRO_PATTERN_TYPE_RASTER_SOURCE:
    default:
	return FALSE;
    }
}

static void
_cairo_vulkan_set_matrix (cairo_vulkan_params_t *params,
			  const cairo_matrix_t *matrix)
{
    params->matrix[0] = matrix->xx;
    params->matrix[1] = matrix->yx;
    params->matrix[2] = 0.0f;
    params->matrix[3] = 0.0f;

    params->matrix[4] = matrix->xy;
    params->matrix[5] = matrix->yy;
    params->matrix[6] = 0.0f;
    params->matrix[7] = 0.0f;

    params->matrix[8] = 0.0f;
    params->matrix[9] = 0.0f;
    params->matrix[10] = 1.0f;
    params->matrix[11] = 0.0f;

    params->matrix[12] = matrix->x0;
    params->matrix[13] = matrix->y0;
    params->matrix[14] = 0.0f;
    params->matrix[15] = 1.0f;
}

static cairo_int_status_t
_cairo_vulkan_set_pattern (cairo_vulkan_params_t *params,
			   const cairo_pattern_t *pattern)
{
    params->opacity = pattern->opacity;
    params->extend = pattern->extend;
    _cairo_vulkan_set_matrix (params, &pattern->matrix);

    switch (pattern->type) {
    case CAIRO_PATTERN_TYPE_SOLID:
	params->paint_type = CAIRO_VULKAN_PAINT_SOLID;
	params->n_stops = 0;
	{
	    const cairo_color_t *color =
		&((const cairo_solid_pattern_t *) pattern)->color;
	    _cairo_vulkan_set_color (color->red, color->green, color->blue,
				     color->alpha, params->solid);
	}
	break;

    case CAIRO_PATTERN_TYPE_LINEAR: {
	const cairo_linear_pattern_t *linear =
	    (const cairo_linear_pattern_t *) pattern;
	const cairo_gradient_pattern_t *gradient = &linear->base;
	unsigned int i;

	params->paint_type = CAIRO_VULKAN_PAINT_LINEAR;
	params->p1[0] = linear->pd1.x;
	params->p1[1] = linear->pd1.y;
	params->p1[2] = 0.0f;
	params->p1[3] = 0.0f;
	params->p2[0] = linear->pd2.x;
	params->p2[1] = linear->pd2.y;
	params->p2[2] = 0.0f;
	params->p2[3] = 0.0f;

	if (gradient->n_stops > CAIRO_VULKAN_MAX_STOPS)
	    return CAIRO_INT_STATUS_UNSUPPORTED;

	params->n_stops = gradient->n_stops;
	for (i = 0; i < gradient->n_stops; i++) {
	    _cairo_vulkan_set_color (gradient->stops[i].color.red,
				     gradient->stops[i].color.green,
				     gradient->stops[i].color.blue,
				     gradient->stops[i].color.alpha,
				     params->stops[i]);
	    params->stop_offsets[i] = gradient->stops[i].offset;
	}
	break;
    }

    case CAIRO_PATTERN_TYPE_RADIAL: {
	const cairo_radial_pattern_t *radial =
	    (const cairo_radial_pattern_t *) pattern;
	const cairo_gradient_pattern_t *gradient = &radial->base;
	unsigned int i;

	params->paint_type = CAIRO_VULKAN_PAINT_RADIAL;
	params->p1[0] = radial->cd1.center.x;
	params->p1[1] = radial->cd1.center.y;
	params->p1[2] = radial->cd1.radius;
	params->p1[3] = 0.0f;
	params->p2[0] = radial->cd2.center.x;
	params->p2[1] = radial->cd2.center.y;
	params->p2[2] = radial->cd2.radius;
	params->p2[3] = 0.0f;

	if (gradient->n_stops > CAIRO_VULKAN_MAX_STOPS)
	    return CAIRO_INT_STATUS_UNSUPPORTED;

	params->n_stops = gradient->n_stops;
	for (i = 0; i < gradient->n_stops; i++) {
	    _cairo_vulkan_set_color (gradient->stops[i].color.red,
				     gradient->stops[i].color.green,
				     gradient->stops[i].color.blue,
				     gradient->stops[i].color.alpha,
				     params->stops[i]);
	    params->stop_offsets[i] = gradient->stops[i].offset;
	}
	break;
    }

    case CAIRO_PATTERN_TYPE_SURFACE:
    case CAIRO_PATTERN_TYPE_MESH:
    case CAIRO_PATTERN_TYPE_RASTER_SOURCE:
    default:
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_bool_t
_cairo_vulkan_operator_supported (cairo_operator_t op)
{
    return op <= CAIRO_OPERATOR_ADD;
}

static cairo_int_status_t
_cairo_vulkan_collect_clip (const cairo_clip_t *clip,
			    cairo_vulkan_edge_list_t *edges,
			    cairo_vulkan_path_list_t *paths,
			    cairo_vulkan_rect_list_t *rects,
			    uint32_t *num_clip_paths,
			    uint32_t *num_clip_rects)
{
    const cairo_clip_path_t *clip_path;
    cairo_int_status_t status;
    uint32_t i;

    *num_clip_paths = 0;
    *num_clip_rects = 0;

    if (clip == NULL)
	return CAIRO_INT_STATUS_SUCCESS;

    for (clip_path = clip->path; clip_path != NULL; clip_path = clip_path->prev) {
	uint32_t first_edge, num_edges;

	status = _cairo_vulkan_flatten_path (&clip_path->path,
					     clip_path->tolerance,
					     edges,
					     &first_edge,
					     &num_edges);
	if (unlikely (status))
	    return status;

	status = _cairo_vulkan_path_list_append (
	    paths, first_edge, num_edges, clip_path->fill_rule,
	    _cairo_vulkan_samples_for_antialias (clip_path->antialias));
	if (unlikely (status))
	    return status;

	(*num_clip_paths)++;
    }

    for (i = 0; i < (uint32_t) clip->num_boxes; i++) {
	cairo_box_t box = clip->boxes[i];

	status = _cairo_vulkan_rect_list_append (
	    rects,
	    _cairo_fixed_to_double (box.p1.x),
	    _cairo_fixed_to_double (box.p1.y),
	    _cairo_fixed_to_double (box.p2.x),
	    _cairo_fixed_to_double (box.p2.y));
	if (unlikely (status))
	    return status;

	(*num_clip_rects)++;
    }

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_bool_t
_cairo_vulkan_get_draw_extents (cairo_vulkan_surface_t *surface,
				const cairo_clip_t *clip,
				const cairo_path_fixed_t *path,
				cairo_fill_rule_t fill_rule,
				double tolerance,
				int *x0, int *y0, int *x1, int *y1)
{
    if (clip != NULL) {
	*x0 = clip->extents.x;
	*y0 = clip->extents.y;
	*x1 = clip->extents.x + clip->extents.width;
	*y1 = clip->extents.y + clip->extents.height;
    } else {
	*x0 = 0;
	*y0 = 0;
	*x1 = surface->width;
	*y1 = surface->height;
    }

    if (path != NULL) {
	cairo_rectangle_int_t extents;

	_cairo_path_fixed_fill_extents (path, fill_rule, tolerance, &extents);
	if (extents.x - 1 > *x0)
	    *x0 = extents.x - 1;
	if (extents.y - 1 > *y0)
	    *y0 = extents.y - 1;
	if (extents.x + extents.width + 1 < *x1)
	    *x1 = extents.x + extents.width + 1;
	if (extents.y + extents.height + 1 < *y1)
	    *y1 = extents.y + extents.height + 1;
    }

    if (*x0 < 0)
	*x0 = 0;
    if (*y0 < 0)
	*y0 = 0;
    if (*x1 > surface->width)
	*x1 = surface->width;
    if (*y1 > surface->height)
	*y1 = surface->height;

    return *x0 < *x1 && *y0 < *y1;
}

static void
_cairo_vulkan_update_descriptors (cairo_vulkan_surface_t *surface)
{
    VkDescriptorBufferInfo infos[5];
    VkWriteDescriptorSet writes[5];
    cairo_vulkan_buffer_t *buffers[5];
    unsigned int i;

    buffers[0] = &surface->edge_buffer;
    buffers[1] = &surface->path_buffer;
    buffers[2] = &surface->clip_rect_buffer;
    buffers[3] = &surface->params_buffer;
    buffers[4] = &surface->pixel_buffer;

    memset (infos, 0, sizeof (infos));
    memset (writes, 0, sizeof (writes));

    for (i = 0; i < 5; i++) {
	infos[i].buffer = buffers[i]->buffer;
	infos[i].offset = 0;
	infos[i].range = VK_WHOLE_SIZE;

	writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[i].dstSet = surface->descriptor_set;
	writes[i].dstBinding = i;
	writes[i].descriptorCount = 1;
	writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[i].pBufferInfo = &infos[i];
    }

    vkUpdateDescriptorSets (surface->device->device, 5, writes, 0, NULL);
}

static cairo_int_status_t
_cairo_vulkan_run (cairo_vulkan_surface_t *surface,
		   cairo_vulkan_params_t *params,
		   cairo_vulkan_edge_list_t *edges,
		   cairo_vulkan_path_list_t *paths,
		   cairo_vulkan_rect_list_t *rects,
		   int x0, int y0, int x1, int y1)
{
    cairo_vulkan_device_t *device = surface->device;
    VkCommandBufferBeginInfo begin_info;
    VkMemoryBarrier barrier;
    VkResult result;
    VkDeviceSize edge_bytes;
    VkDeviceSize path_bytes;
    VkDeviceSize rect_bytes;
    uint32_t groups_x, groups_y;
    cairo_int_status_t status;

    status = _cairo_vulkan_buffer_ensure (surface, &surface->edge_buffer,
					  edges->num_edges * sizeof (cairo_vulkan_edge_t));
    if (unlikely (status))
	return status;
    status = _cairo_vulkan_buffer_ensure (surface, &surface->path_buffer,
					  paths->num_paths * sizeof (cairo_vulkan_path_t));
    if (unlikely (status))
	return status;
    status = _cairo_vulkan_buffer_ensure (surface, &surface->clip_rect_buffer,
					  rects->num_rects * 4 * sizeof (float));
    if (unlikely (status))
	return status;
    status = _cairo_vulkan_buffer_ensure (surface, &surface->params_buffer,
					  sizeof (cairo_vulkan_params_t));
    if (unlikely (status))
	return status;

    params->surface_width = surface->width;
    params->surface_height = surface->height;
    params->offset_x = x0;
    params->offset_y = y0;

    edge_bytes = edges->num_edges * sizeof (cairo_vulkan_edge_t);
    path_bytes = paths->num_paths * sizeof (cairo_vulkan_path_t);
    rect_bytes = rects->num_rects * 4 * sizeof (float);

    if (edge_bytes != 0)
	memcpy (surface->edge_buffer.mapped, edges->edges, edge_bytes);
    if (path_bytes != 0)
	memcpy (surface->path_buffer.mapped, paths->paths, path_bytes);
    if (rect_bytes != 0)
	memcpy (surface->clip_rect_buffer.mapped, rects->rects, rect_bytes);
    memcpy (surface->params_buffer.mapped, params, sizeof (*params));

    _cairo_vulkan_update_descriptors (surface);

    groups_x = ((uint32_t) (x1 - x0) + CAIRO_VULKAN_WORKGROUP_SIZE - 1) /
	       CAIRO_VULKAN_WORKGROUP_SIZE;
    groups_y = ((uint32_t) (y1 - y0) + CAIRO_VULKAN_WORKGROUP_SIZE - 1) /
	       CAIRO_VULKAN_WORKGROUP_SIZE;

    if (groups_x > device->max_workgroup_count[0] ||
	groups_y > device->max_workgroup_count[1])
    {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    result = vkResetFences (device->device, 1, &surface->fence);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    result = vkResetCommandBuffer (surface->command_buffer, 0);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    memset (&begin_info, 0, sizeof (begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer (surface->command_buffer, &begin_info);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    memset (&barrier, 0, sizeof (barrier));
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
			    VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier (surface->command_buffer,
			  VK_PIPELINE_STAGE_HOST_BIT,
			  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			  0, 1, &barrier, 0, NULL, 0, NULL);

    vkCmdBindPipeline (surface->command_buffer,
		       VK_PIPELINE_BIND_POINT_COMPUTE,
		       device->pipeline);
    vkCmdBindDescriptorSets (surface->command_buffer,
			     VK_PIPELINE_BIND_POINT_COMPUTE,
			     device->pipeline_layout,
			     0, 1, &surface->descriptor_set, 0, NULL);
    vkCmdDispatch (surface->command_buffer, groups_x, groups_y, 1);

    memset (&barrier, 0, sizeof (barrier));
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier (surface->command_buffer,
			  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			  VK_PIPELINE_STAGE_HOST_BIT,
			  0, 1, &barrier, 0, NULL, 0, NULL);

    result = vkEndCommandBuffer (surface->command_buffer);
    if (result != VK_SUCCESS)
	return CAIRO_INT_STATUS_DEVICE_ERROR;

    {
	VkSubmitInfo submit_info;

	memset (&submit_info, 0, sizeof (submit_info));
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &surface->command_buffer;

	result = vkQueueSubmit (device->queue, 1, &submit_info, surface->fence);
	if (result != VK_SUCCESS)
	    return CAIRO_INT_STATUS_DEVICE_ERROR;
    }

    surface->pending = TRUE;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    cairo_surface_mark_dirty_rectangle (&surface->image->base,
					x0, y0, x1 - x0, y1 - y0);

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_vulkan_gpu_operation (cairo_vulkan_surface_t *surface,
			     cairo_operator_t op,
			     const cairo_pattern_t *source,
			     const cairo_path_fixed_t *path,
			     cairo_fill_rule_t fill_rule,
			     double tolerance,
			     cairo_antialias_t antialias,
			     const cairo_clip_t *clip)
{
    cairo_vulkan_params_t params;
    cairo_vulkan_edge_list_t edges;
    cairo_vulkan_path_list_t paths;
    cairo_vulkan_rect_list_t rects;
    cairo_int_status_t status;
    uint32_t num_clip_paths, num_clip_rects;
    int x0, y0, x1, y1;

    if (! _cairo_vulkan_operator_supported (op) ||
	! _cairo_vulkan_pattern_supported (source))
    {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (path != NULL && _cairo_path_fixed_fill_is_empty (path))
	return CAIRO_INT_STATUS_SUCCESS;

    if (! _cairo_vulkan_get_draw_extents (surface, clip, path, fill_rule,
					  tolerance, &x0, &y0, &x1, &y1))
    {
	return CAIRO_INT_STATUS_SUCCESS;
    }

    memset (&params, 0, sizeof (params));
    memset (&edges, 0, sizeof (edges));
    memset (&paths, 0, sizeof (paths));
    memset (&rects, 0, sizeof (rects));

    params.op = op;
    params.has_fill = path != NULL;
    params.opaque = surface->base.content == CAIRO_CONTENT_COLOR;
    params.num_clip_paths = 0;
    params.num_clip_rects = 0;

    status = _cairo_vulkan_set_pattern (&params, source);
    if (unlikely (status)) {
	_cairo_vulkan_edge_list_fini (&edges);
	_cairo_vulkan_path_list_fini (&paths);
	_cairo_vulkan_rect_list_fini (&rects);
	return status;
    }

    if (path != NULL) {
	uint32_t first_edge, num_edges;

	status = _cairo_vulkan_flatten_path (path, tolerance, &edges,
					     &first_edge, &num_edges);
	if (unlikely (status))
	    goto done;

	status = _cairo_vulkan_path_list_append (
	    &paths, first_edge, num_edges, fill_rule,
	    _cairo_vulkan_samples_for_antialias (antialias));
	if (unlikely (status))
	    goto done;
    }

    status = _cairo_vulkan_collect_clip (clip, &edges, &paths, &rects,
					 &num_clip_paths, &num_clip_rects);
    if (unlikely (status))
	goto done;

    params.num_clip_paths = num_clip_paths;
    params.num_clip_rects = num_clip_rects;

    status = _cairo_vulkan_run (surface, &params, &edges, &paths, &rects,
				x0, y0, x1, y1);

done:
    _cairo_vulkan_edge_list_fini (&edges);
    _cairo_vulkan_path_list_fini (&paths);
    _cairo_vulkan_rect_list_fini (&rects);

    return status;
}

static cairo_int_status_t
_cairo_vulkan_cpu_fill (cairo_vulkan_surface_t *surface,
			cairo_operator_t op,
			const cairo_pattern_t *source,
			const cairo_path_fixed_t *path,
			cairo_fill_rule_t fill_rule,
			double tolerance,
			cairo_antialias_t antialias,
			const cairo_clip_t *clip)
{
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    return _cairo_surface_fill (&surface->image->base, op, source, path,
				fill_rule, tolerance, antialias, clip);
}

static cairo_int_status_t
_cairo_vulkan_surface_fill (void *abstract_surface,
			    cairo_operator_t op,
			    const cairo_pattern_t *source,
			    const cairo_path_fixed_t *path,
			    cairo_fill_rule_t fill_rule,
			    double tolerance,
			    cairo_antialias_t antialias,
			    const cairo_clip_t *clip)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    if (op == CAIRO_OPERATOR_DEST)
	return CAIRO_INT_STATUS_SUCCESS;

    status = _cairo_vulkan_gpu_operation (surface, op, source, path,
					  fill_rule, tolerance, antialias, clip);
    if (status != CAIRO_INT_STATUS_UNSUPPORTED)
	return status;

    return _cairo_vulkan_cpu_fill (surface, op, source, path, fill_rule,
				   tolerance, antialias, clip);
}

static cairo_int_status_t
_cairo_vulkan_cpu_paint (cairo_vulkan_surface_t *surface,
			 cairo_operator_t op,
			 const cairo_pattern_t *source,
			 const cairo_clip_t *clip)
{
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    return _cairo_surface_paint (&surface->image->base, op, source, clip);
}

static cairo_int_status_t
_cairo_vulkan_surface_paint (void *abstract_surface,
			     cairo_operator_t op,
			     const cairo_pattern_t *source,
			     const cairo_clip_t *clip)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    if (op == CAIRO_OPERATOR_DEST)
	return CAIRO_INT_STATUS_SUCCESS;

    status = _cairo_vulkan_gpu_operation (surface, op, source, NULL,
					  CAIRO_FILL_RULE_WINDING,
					  0.0, CAIRO_ANTIALIAS_DEFAULT, clip);
    if (status != CAIRO_INT_STATUS_UNSUPPORTED)
	return status;

    return _cairo_vulkan_cpu_paint (surface, op, source, clip);
}

static cairo_int_status_t
_cairo_vulkan_surface_mask (void *abstract_surface,
			    cairo_operator_t op,
			    const cairo_pattern_t *source,
			    const cairo_pattern_t *mask,
			    const cairo_clip_t *clip)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    return _cairo_surface_mask (&surface->image->base, op, source, mask, clip);
}

static cairo_int_status_t
_cairo_vulkan_surface_stroke (void *abstract_surface,
			      cairo_operator_t op,
			      const cairo_pattern_t *source,
			      const cairo_path_fixed_t *path,
			      const cairo_stroke_style_t *style,
			      const cairo_matrix_t *ctm,
			      const cairo_matrix_t *ctm_inverse,
			      double tolerance,
			      cairo_antialias_t antialias,
			      const cairo_clip_t *clip)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    return _cairo_surface_stroke (&surface->image->base, op, source, path,
				  style, ctm, ctm_inverse, tolerance,
				  antialias, clip);
}

static cairo_surface_t *
_cairo_vulkan_surface_create_internal (int width, int height,
				       cairo_content_t content);

static cairo_surface_t *
_cairo_vulkan_surface_create_similar (void *abstract_surface,
				      cairo_content_t content,
				      int width, int height)
{
    return _cairo_vulkan_surface_create_internal (width, height, content);
}

static cairo_surface_t *
_cairo_vulkan_surface_create_similar_image (void *abstract_surface,
					    cairo_format_t format,
					    int width, int height)
{
    return cairo_image_surface_create (format, width, height);
}

static cairo_image_surface_t *
_cairo_vulkan_surface_map_to_image (void *abstract_surface,
				    const cairo_rectangle_int_t *extents)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return (cairo_image_surface_t *)
	    _cairo_surface_create_in_error (status);

    return _cairo_image_surface_map_to_image (surface->image, extents);
}

static cairo_int_status_t
_cairo_vulkan_surface_unmap_image (void *abstract_surface,
				   cairo_image_surface_t *image)
{
    return _cairo_image_surface_unmap_image (abstract_surface, image);
}

static cairo_surface_t *
_cairo_vulkan_surface_source (void *abstract_surface,
			      cairo_rectangle_int_t *extents)
{
    cairo_vulkan_surface_t *surface = abstract_surface;

    if (extents != NULL) {
	extents->x = 0;
	extents->y = 0;
	extents->width = surface->width;
	extents->height = surface->height;
    }

    return &surface->image->base;
}

static cairo_status_t
_cairo_vulkan_surface_acquire_source_image (void *abstract_surface,
					    cairo_image_surface_t **image_out,
					    void **image_extra)
{
    cairo_vulkan_surface_t *surface = abstract_surface;

    *image_out = surface->image;
    *image_extra = NULL;

    return CAIRO_STATUS_SUCCESS;
}

static void
_cairo_vulkan_surface_release_source_image (void *abstract_surface,
					    cairo_image_surface_t *image,
					    void *image_extra)
{
}

static cairo_surface_t *
_cairo_vulkan_surface_snapshot (void *abstract_surface)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_surface_t *snapshot;
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return _cairo_surface_create_in_error (status);

    snapshot = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
					   surface->width, surface->height);
    if (unlikely (snapshot->status))
	return snapshot;

    memcpy (cairo_image_surface_get_data (snapshot), surface->pixels,
	    (size_t) surface->stride * surface->height);
    cairo_surface_mark_dirty (snapshot);

    return snapshot;
}

static cairo_bool_t
_cairo_vulkan_surface_get_extents (void *abstract_surface,
				   cairo_rectangle_int_t *extents)
{
    cairo_vulkan_surface_t *surface = abstract_surface;

    extents->x = 0;
    extents->y = 0;
    extents->width = surface->width;
    extents->height = surface->height;

    return TRUE;
}

static cairo_status_t
_cairo_vulkan_surface_flush (void *abstract_surface, unsigned flags)
{
    cairo_vulkan_surface_t *surface = abstract_surface;

    return _cairo_vulkan_surface_wait (surface);
}

static cairo_status_t
_cairo_vulkan_surface_mark_dirty_rectangle (void *abstract_surface,
					    int x, int y,
					    int width, int height)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_int_status_t status;

    status = _cairo_vulkan_surface_wait (surface);
    if (unlikely (status))
	return status;

    cairo_surface_mark_dirty_rectangle (&surface->image->base,
					x, y, width, height);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_vulkan_surface_finish (void *abstract_surface)
{
    cairo_vulkan_surface_t *surface = abstract_surface;
    cairo_vulkan_device_t *device = surface->device;

    if (device == NULL)
	return CAIRO_STATUS_SUCCESS;

    vkDeviceWaitIdle (device->device);

    if (surface->image != NULL) {
	cairo_surface_finish (&surface->image->base);
	cairo_surface_destroy (&surface->image->base);
	surface->image = NULL;
    }

    if (surface->descriptor_pool != VK_NULL_HANDLE) {
	vkDestroyDescriptorPool (device->device, surface->descriptor_pool, NULL);
	surface->descriptor_pool = VK_NULL_HANDLE;
    }

    if (surface->command_pool != VK_NULL_HANDLE) {
	vkDestroyCommandPool (device->device, surface->command_pool, NULL);
	surface->command_pool = VK_NULL_HANDLE;
	surface->command_buffer = VK_NULL_HANDLE;
    }

    if (surface->fence != VK_NULL_HANDLE) {
	vkDestroyFence (device->device, surface->fence, NULL);
	surface->fence = VK_NULL_HANDLE;
    }

    _cairo_vulkan_buffer_fini (device, &surface->pixel_buffer);
    _cairo_vulkan_buffer_fini (device, &surface->edge_buffer);
    _cairo_vulkan_buffer_fini (device, &surface->path_buffer);
    _cairo_vulkan_buffer_fini (device, &surface->clip_rect_buffer);
    _cairo_vulkan_buffer_fini (device, &surface->params_buffer);

    surface->pixels = NULL;
    surface->device = NULL;
    _cairo_vulkan_device_release (device);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *
_cairo_vulkan_surface_create_internal (int width, int height,
				       cairo_content_t content)
{
    cairo_vulkan_surface_t *surface;
    cairo_vulkan_device_t *device;
    VkCommandPoolCreateInfo command_pool_info;
    VkCommandBufferAllocateInfo command_buffer_info;
    VkFenceCreateInfo fence_info;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo pool_info;
    VkDescriptorSetAllocateInfo set_info;
    cairo_int_status_t status;
    VkResult result;

    if (width <= 0 || height <= 0)
	return _cairo_surface_create_in_error (CAIRO_STATUS_INVALID_SIZE);
    if (! CAIRO_CONTENT_VALID (content))
	return _cairo_surface_create_in_error (CAIRO_STATUS_INVALID_CONTENT);

    device = _cairo_vulkan_device_acquire ();
    if (device == NULL)
	return _cairo_surface_create_in_error (CAIRO_STATUS_DEVICE_ERROR);

    surface = _cairo_calloc (sizeof (cairo_vulkan_surface_t));
    if (unlikely (surface == NULL)) {
	_cairo_vulkan_device_release (device);
	return _cairo_surface_create_in_error (CAIRO_STATUS_NO_MEMORY);
    }

    _cairo_surface_init (&surface->base, &_cairo_vulkan_surface_backend,
			 NULL, content, FALSE);

    surface->device = device;
    surface->width = width;
    surface->height = height;
    surface->stride = width * 4;

    status = _cairo_vulkan_buffer_ensure (
	surface, &surface->pixel_buffer,
	(VkDeviceSize) surface->stride * surface->height);
    if (unlikely (status))
	goto error;

    surface->pixels = surface->pixel_buffer.mapped;
    memset (surface->pixels, 0,
	    (size_t) surface->stride * surface->height);

    surface->image = (cairo_image_surface_t *)
	cairo_image_surface_create_for_data (surface->pixels,
					     CAIRO_FORMAT_ARGB32,
					     width, height,
					     surface->stride);
    if (unlikely (surface->image->base.status)) {
	status = surface->image->base.status;
	goto error;
    }

    memset (&command_pool_info, 0, sizeof (command_pool_info));
    command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = device->queue_family;

    result = vkCreateCommandPool (device->device, &command_pool_info, NULL,
				  &surface->command_pool);
    if (result != VK_SUCCESS) {
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
	goto error;
    }

    memset (&command_buffer_info, 0, sizeof (command_buffer_info));
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = surface->command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;

    result = vkAllocateCommandBuffers (device->device, &command_buffer_info,
				       &surface->command_buffer);
    if (result != VK_SUCCESS) {
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
	goto error;
    }

    memset (&fence_info, 0, sizeof (fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    result = vkCreateFence (device->device, &fence_info, NULL, &surface->fence);
    if (result != VK_SUCCESS) {
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
	goto error;
    }

    memset (&pool_size, 0, sizeof (pool_size));
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 5;

    memset (&pool_info, 0, sizeof (pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    result = vkCreateDescriptorPool (device->device, &pool_info, NULL,
				     &surface->descriptor_pool);
    if (result != VK_SUCCESS) {
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
	goto error;
    }

    memset (&set_info, 0, sizeof (set_info));
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = surface->descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &device->descriptor_set_layout;

    result = vkAllocateDescriptorSets (device->device, &set_info,
				       &surface->descriptor_set);
    if (result != VK_SUCCESS) {
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
	goto error;
    }

    surface->base.is_clear = TRUE;

    return &surface->base;

error:
    if (status == CAIRO_INT_STATUS_UNSUPPORTED)
	status = CAIRO_INT_STATUS_DEVICE_ERROR;
    _cairo_surface_set_error (&surface->base, status);
    return &surface->base;
}

cairo_surface_t *
cairo_vulkan_surface_create (int width, int height)
{
    return _cairo_vulkan_surface_create_internal (width, height,
						  CAIRO_CONTENT_COLOR_ALPHA);
}

int
cairo_vulkan_surface_get_width (cairo_surface_t *surface)
{
    cairo_vulkan_surface_t *vulkan = _cairo_vulkan_surface_from_surface (surface);

    return vulkan != NULL ? vulkan->width : 0;
}

int
cairo_vulkan_surface_get_height (cairo_surface_t *surface)
{
    cairo_vulkan_surface_t *vulkan = _cairo_vulkan_surface_from_surface (surface);

    return vulkan != NULL ? vulkan->height : 0;
}

int
cairo_vulkan_surface_get_stride (cairo_surface_t *surface)
{
    cairo_vulkan_surface_t *vulkan = _cairo_vulkan_surface_from_surface (surface);

    return vulkan != NULL ? vulkan->stride : 0;
}

unsigned char *
cairo_vulkan_surface_get_data (cairo_surface_t *surface)
{
    cairo_vulkan_surface_t *vulkan = _cairo_vulkan_surface_from_surface (surface);

    return vulkan != NULL ? vulkan->pixels : NULL;
}

static const cairo_surface_backend_t _cairo_vulkan_surface_backend = {
    CAIRO_SURFACE_TYPE_VULKAN,
    _cairo_vulkan_surface_finish,

    _cairo_default_context_create,

    _cairo_vulkan_surface_create_similar,
    _cairo_vulkan_surface_create_similar_image,
    _cairo_vulkan_surface_map_to_image,
    _cairo_vulkan_surface_unmap_image,

    _cairo_vulkan_surface_source,
    _cairo_vulkan_surface_acquire_source_image,
    _cairo_vulkan_surface_release_source_image,
    _cairo_vulkan_surface_snapshot,

    NULL, /* copy_page */
    NULL, /* show_page */

    _cairo_vulkan_surface_get_extents,
    NULL, /* get_font_options */

    _cairo_vulkan_surface_flush,
    _cairo_vulkan_surface_mark_dirty_rectangle,

    _cairo_vulkan_surface_paint,
    _cairo_vulkan_surface_mask,
    _cairo_vulkan_surface_stroke,
    _cairo_vulkan_surface_fill,
    NULL, /* fill_stroke */
    NULL, /* show_glyphs */
};
