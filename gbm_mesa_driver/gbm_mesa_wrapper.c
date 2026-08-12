/*
 * Copyright (C) 2021-2022 Roman Stratiienko (r.stratiienko@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "GBM-MESA-WRAPPER"

#include <gbm.h>

#include "gbm_mesa_wrapper.h"

#include <cutils/properties.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <inttypes.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*(A)))
#define DRM_TO_GBM_FORMAT(A)                                                                       \
	{                                                                                          \
		DRM_##A, GBM_##A                                                                   \
	}

/* List of the formats that has mesa3d DRI_ equivalent, therefore can be allocated directly */
static const struct {
	uint32_t drm_format;
	uint32_t gbm_format;
} drm_to_gbm_image_formats[] = {
	DRM_TO_GBM_FORMAT(FORMAT_R8),
	DRM_TO_GBM_FORMAT(FORMAT_GR88),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB1555),
	DRM_TO_GBM_FORMAT(FORMAT_RGB565),
	DRM_TO_GBM_FORMAT(FORMAT_XRGB8888),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB8888),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR8888),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR8888),
	DRM_TO_GBM_FORMAT(FORMAT_XRGB2101010),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR2101010),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB2101010),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR2101010),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR16161616F),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR16161616F),
	/* Do NOT add NV12/YUV420 here. It was tried: gbm_bo_create() rejects them
	 * with EINVAL on this stack whatever the usage flags (tested with
	 * RENDERING|SCANOUT|LINEAR and with LINEAR alone), and listing them here
	 * makes get_gbm_mesa_format() return non-zero, which stops
	 * gbm_mesa_bo_create() from taking the dumb-buffer fallback -- so nothing
	 * gets allocated at all instead of at least getting a linear buffer.
	 */
};

static uint32_t get_gbm_mesa_format(uint32_t drm_format)
{
	uint32_t i;
	for (i = 0; i < ARRAY_SIZE(drm_to_gbm_image_formats); i++) {
		if (drm_to_gbm_image_formats[i].drm_format == drm_format)
			return drm_to_gbm_image_formats[i].gbm_format;
	}

	return 0;
}

static void configure_mesa_environment(void)
{
	static int configured = 0;
	if (configured)
		return;
	configured = 1;

	// Read GBM backends path from property
	char backends_path[PROPERTY_VALUE_MAX];
	property_get("vendor.mesa.gbm_backends_path", backends_path, "/vendor/lib64/gbm");

	// Configure Mesa GBM backend search path
	setenv("GBM_BACKENDS_PATH", backends_path, 1);

	// DRI drivers path (zink_dri.so, powervr_dri.so)
	setenv("LIBGL_DRIVERS_PATH", "/vendor/lib64/dri", 1);

	// Force Zink as DRI driver for PowerVR (Vulkan-only GPU)
	setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);

	ALOGI("GBM-MESA: backends=%s dri=/vendor/lib64/dri override=zink", backends_path);
}

static struct gbm_device *gbm_mesa_dev_create(int fd)
{
	// Configure Mesa environment on first use
	configure_mesa_environment();

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		ALOGE("Unable to create gbm device (fd=%d)", fd);
		ALOGE("Make sure dri_gbm.so is accessible in /vendor/lib64/gbm/");
	} else {
		ALOGI("Successfully created GBM device");
	}

	return gbm;
}

static void gbm_mesa_dev_destroy(struct gbm_device *gbm)
{
	gbm_device_destroy(gbm);
}

// ALLOCATOR_ONLY!
static int gbm_mesa_alloc(struct alloc_args *args)
{
	struct gbm_bo *bo = NULL;

	uint32_t gbm_format = get_gbm_mesa_format(args->drm_format);
	int usage = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR;

	if (args->use_scanout)
		usage |= GBM_BO_USE_SCANOUT;

	ALOGI("gbm_mesa_alloc: %dx%d fmt=0x%x gbm_fmt=0x%x usage=0x%x", args->width, args->height,
	      args->drm_format, gbm_format, usage);

	bo = gbm_bo_create(args->gbm, args->width, args->height, gbm_format, usage);

	if (!bo) {
		ALOGE("Unable to create BO, size=%dx%d, fmt=0x%x gbm_fmt=0x%x usage=0x%x errno=%d(%s)",
		      args->width, args->height, args->drm_format, gbm_format, usage,
		      errno, strerror(errno));
		return -errno;
	}
	args->out_fd = gbm_bo_get_fd(bo);
	args->out_stride = gbm_bo_get_stride(bo);
	args->out_modifier = gbm_bo_get_modifier(bo);
	uint32_t bo_w = gbm_bo_get_width(bo);
	uint32_t bo_h = gbm_bo_get_height(bo);
	uint32_t bo_fmt = gbm_bo_get_format(bo);
	int bo_plane_count = gbm_bo_get_plane_count(bo);

	ALOGI("gbm_mesa_alloc: BO created %dx%d fmt=0x%x planes=%d fd=%d stride=%d modifier=0x%llx",
	      bo_w, bo_h, bo_fmt, bo_plane_count, args->out_fd, args->out_stride,
	      (unsigned long long)args->out_modifier);

	if (args->out_fd < 0) {
		ALOGE("gbm_mesa_alloc: gbm_bo_get_fd FAILED (fd=%d) errno=%d(%s) - buffer not exportable!",
		      args->out_fd, errno, strerror(errno));
	}

	/* Buffer is now handled through the system via out_fd, we can now destroy gbm_mesa bo */
	gbm_bo_destroy(bo);

	if (args->needs_map_stride) {
		/* At least on Intel and nouveau map_stride after gbm_create is different from
		 * map_stride after gbm_import, We care only about map_stride after importing. */
		struct gbm_import_fd_modifier_data data = {
			.width = args->width,
			.height = args->height,
			.format = gbm_format,
			.num_fds = 1,
			.fds[0] = args->out_fd,
			.strides[0] = (int)args->out_stride,
			.modifier = args->out_modifier,
		};
		void *addr = NULL;
		void *map_data = NULL;

		bo = gbm_bo_import(args->gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);
		if (!bo) {
			/* Import can fail on some drivers (e.g., panfrost with meson-drm).
			 * Use the allocation stride as fallback for map_stride since
			 * the buffer was successfully allocated. */
			ALOGW("Failed to import BO during map_stride query, using stride as fallback");
			args->out_map_stride = args->out_stride;
		} else {
			int flags = GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE;

			addr = gbm_bo_map(bo, 0, 0, args->width, args->height, flags, &args->out_map_stride,
					  &map_data);
			if (addr == MAP_FAILED || addr == NULL) {
				ALOGW("Failed to map buffer for stride query, using stride as fallback");
				args->out_map_stride = args->out_stride;
			} else {
				gbm_bo_unmap(bo, map_data);
			}

			gbm_bo_destroy(bo);
		}
	}

	return 0;
}

// MAPPER ONLY!
static struct gbm_bo *gbm_import(struct gbm_device *gbm, int buf_fd, uint32_t width,
				 uint32_t height, uint32_t stride, uint64_t modifier,
				 uint32_t drm_format)
{
	struct gbm_bo *bo = NULL;
	uint32_t gbm_format = get_gbm_mesa_format(drm_format);
	struct gbm_import_fd_modifier_data data = {
		.width = width,
		.height = height,
		.format = gbm_format,
		.num_fds = 1,
		.fds[0] = buf_fd,
		.strides[0] = (int)stride,
		.modifier = modifier,
	};

	bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);

	return bo;
}

static void gbm_free(struct gbm_bo *bo)
{
	gbm_bo_destroy(bo);
}

static void gbm_map(struct gbm_bo *bo, int w, int h, void **addr, void **map_data)
{
	int flags = GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE;

	uint32_t stride = 0;
	*addr = gbm_bo_map(bo, 0, 0, w, h, flags, &stride, map_data);
	if (*addr == MAP_FAILED || *addr == NULL) {
		ALOGE("Failed to map the buffer at %s:%d", __FILE__, __LINE__);
	}
}

static void gbm_unmap(struct gbm_bo *bo, void *map_data)
{
	gbm_bo_unmap(bo, map_data);
}

struct gbm_ops gbm_ops = {
	.get_gbm_format = get_gbm_mesa_format,
	.dev_create = gbm_mesa_dev_create,
	.dev_destroy = gbm_mesa_dev_destroy,
	.alloc = gbm_mesa_alloc,
	.import = gbm_import,
	.free = gbm_free,
	.map = gbm_map,
	.unmap = gbm_unmap,
};

__attribute__((visibility("default"))) struct gbm_ops *get_gbm_ops()
{
	return &gbm_ops;
}
