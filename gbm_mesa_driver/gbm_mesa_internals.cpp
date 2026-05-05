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

#define LOG_TAG "GBM-MESA-GRALLOC"

extern "C" {
#include "drv_helpers.h"
}

#include "gbm_mesa_wrapper.h"

#include "UniqueFd.h"
#include "drv_priv.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <gbm.h>
#include <glob.h>
#include <iterator>
#include <linux/dma-buf.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define GBM_WRAPPER_NAME "libgbm_mesa_wrapper.so"
#define GBM_GET_OPS_SYMBOL "get_gbm_ops"

/* Forward declarations */
struct GbmMesaDriver;
static std::shared_ptr<GbmMesaDriver> gbm_mesa_get_or_init_driver(struct driver *drv,
								  bool mapper_sphal);

void gbm_mesa_resolve_format_and_use_flags(struct driver *drv, uint32_t format, uint64_t use_flags,
					   uint32_t *out_format, uint64_t *out_use_flags)
{
	*out_format = format;
	*out_use_flags = use_flags;
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
			*out_format = DRM_FORMAT_NV12;
		} else {
			/*HACK: See b/28671744 */
			*out_format = DRM_FORMAT_XBGR8888;
		}
		break;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		*out_format = DRM_FORMAT_NV12;
		break;
	case DRM_FORMAT_BGR565:
		/* mesa3d doesn't support BGR565 */
		*out_format = DRM_FORMAT_RGB565;
		break;
	}
}

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						   DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
						   DRM_FORMAT_RGB565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

/* Check if format is YUV (can use dumb buffer fallback - CPU only, no GPU texture) */
static bool is_yuv_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
	case DRM_FORMAT_YUV420:
		return true;
	default:
		return false;
	}
}

static struct format_metadata linear_metadata = { 1, 0, DRM_FORMAT_MOD_LINEAR };

int gbm_mesa_driver_init(struct driver *drv)
{
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &linear_metadata, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &linear_metadata, BO_USE_TEXTURE_MASK | BO_USE_SCANOUT);

	drv_add_combination(drv, DRM_FORMAT_R8, &linear_metadata, BO_USE_SW_MASK | BO_USE_LINEAR);

	// Fixes android.hardware.cts.HardwareBufferTest#testCreate CTS test
	drv_add_combination(drv, DRM_FORMAT_BGR888, &linear_metadata, BO_USE_SW_MASK);

	drv_modify_combination(drv, DRM_FORMAT_NV12, &linear_metadata,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &linear_metadata, BO_USE_HW_VIDEO_ENCODER);

	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera and input/output from hardware decoder/encoder.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &linear_metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_HW_VIDEO_ENCODER);

	/*
	 * Android also frequently requests YV12 formats for some camera implementations
	 * (including the external provider implmenetation).
	 */
	drv_modify_combination(drv, DRM_FORMAT_YVU420_ANDROID, &linear_metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	int ret = drv_modify_linear_combinations(drv);

	/* Pre-initialize the GBM device to avoid delays on first buffer allocation */
	if (ret == 0) {
		auto gbm_drv = gbm_mesa_get_or_init_driver(drv, false);
		if (!gbm_drv) {
			drv_loge("Failed to pre-initialize GBM Mesa driver");
		}
	}

	return ret;
}

struct GbmMesaDriver {
	~GbmMesaDriver()
	{
		if (gbm_dev)
			wrapper->dev_destroy(gbm_dev);

		if (gbm_gpu_dev)
			wrapper->dev_destroy(gbm_gpu_dev);

		if (dl_handle)
			dlclose(dl_handle);
	}

	struct gbm_ops *wrapper = nullptr;
	struct gbm_device *gbm_dev = nullptr;      /* KMS device (meson-drm) for scanout */
	struct gbm_device *gbm_gpu_dev = nullptr;  /* GPU device (panfrost) for fallback */
	void *dl_handle = nullptr;

	UniqueFd gbm_node_fd;
	UniqueFd gpu_node_fd;
	bool has_separate_gpu = false;  /* true if GPU and KMS are separate devices */
};

struct GbmMesaDriverPriv {
	std::shared_ptr<GbmMesaDriver> gbm_mesa_drv;
};

/*
 * Check if target device has KMS.
 */
int is_kms_dev(int fd)
{
	auto res = drmModeGetResources(fd);
	if (!res)
		return false;

	bool is_kms = res->count_crtcs > 0 && res->count_connectors > 0 && res->count_encoders > 0;

	drmModeFreeResources(res);

	return is_kms;
}

/*
 * Search for a KMS device. Return opened file descriptor on success.
 */
int open_drm_dev(bool card_node, std::function<bool(int, bool, std::string)> found)
{
	glob_t glob_buf;
	memset(&glob_buf, 0, sizeof(glob_buf));
	int fd;
	const char *pattern = card_node ? "/dev/dri/card*" : "/dev/dri/renderD*";

	int ret = glob(pattern, 0, NULL, &glob_buf);
	if (ret) {
		globfree(&glob_buf);
		return -EINVAL;
	}

	for (size_t i = 0; i < glob_buf.gl_pathc; ++i) {
		fd = open(glob_buf.gl_pathv[i], O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			drv_loge("Unable to open %s with error %s", glob_buf.gl_pathv[i],
				 strerror(errno));
			continue;
		}

		drmVersionPtr drm_version;
		drm_version = drmGetVersion(fd);
		std::string drm_name(drm_version->name);
		drmFreeVersion(drm_version);

		if (found(fd, is_kms_dev(fd), drm_name))
			continue;

		close(fd);
		fd = 0;
	}

	globfree(&glob_buf);

	return 0;
}

/*
 * List of GPU which rely on separate display controller drivers,
 * For this GPUs we have to find and open /dev/cardX KMS node
 * Other GPUs can be accessed via renderD GPU node.
 */
static std::array<std::string, 6> separate_dc_gpu_list = { "v3d",      "vc4",  "etnaviv",
							   "panfrost", "lima", "freedreno" };

static bool is_separate_dc_gpu(UniqueFd *out_gpu_fd)
{
	UniqueFd gpu_fd;
	bool separate_dc = false;
	std::string gpu_name;

	open_drm_dev(false, [&](int fd, bool is_kms, std::string drm_name) -> bool {
		if (separate_dc)
			return false;

		for (const auto &name : separate_dc_gpu_list) {
			if (drm_name == std::string(name))
				separate_dc = true;
		}
		gpu_fd = UniqueFd(fd);
		gpu_name = drm_name;

		return true;
	});

	*out_gpu_fd = std::move(gpu_fd);

	drv_logi("Found GPU %s\n", gpu_name.c_str());

	return separate_dc;
}

static std::shared_ptr<GbmMesaDriver> gbm_mesa_get_or_init_driver(struct driver *drv,
								  bool mapper_sphal)
{
	std::shared_ptr<GbmMesaDriver> gbm_mesa_drv;

	if (!drv->priv) {
		gbm_mesa_drv = std::make_unique<GbmMesaDriver>();

		bool look_for_kms = is_separate_dc_gpu(&gbm_mesa_drv->gpu_node_fd);
		gbm_mesa_drv->has_separate_gpu = look_for_kms;

		if (look_for_kms && !mapper_sphal) {
			drv_logi("GPU require KMSRO entry, searching for separate KMS driver...\n");
			open_drm_dev(true, [&](int fd, bool is_kms, std::string drm_name) -> bool {
				if (!is_kms || gbm_mesa_drv->gbm_node_fd)
					return false;

				gbm_mesa_drv->gbm_node_fd = UniqueFd(fd);
				drv_logi("Found KMS dev %s\n", drm_name.c_str());
				return true;
			});
			/* cardX KMS node need this otherwise composer won't be able to configure
			 * KMS state */
			if (gbm_mesa_drv->gbm_node_fd)
				drmDropMaster(gbm_mesa_drv->gbm_node_fd.Get());
			else
				drv_loge(
				    "Unable to find/open /dev/card node with KMS capabilities.\n");
		} else {
			// Non-KMSRO systems: use GPU node for both rendering and scanout
			gbm_mesa_drv->gbm_node_fd = UniqueFd(dup(gbm_mesa_drv->gpu_node_fd.Get()));
		}

		if (!gbm_mesa_drv->gbm_node_fd) {
			drv_loge("Unable to find or open DRM node");
			return nullptr;
		}

		gbm_mesa_drv->dl_handle = dlopen(GBM_WRAPPER_NAME, RTLD_NOW);
		if (gbm_mesa_drv->dl_handle == nullptr) {
			drv_loge("%s", dlerror());
			drv_loge("Unable to open '%s' shared library", GBM_WRAPPER_NAME);
			return nullptr;
		}

		auto get_gbm_ops =
		    (struct gbm_ops * (*)(void)) dlsym(gbm_mesa_drv->dl_handle, GBM_GET_OPS_SYMBOL);
		if (get_gbm_ops == nullptr) {
			drv_loge("Unable to find '%s' symbol", GBM_GET_OPS_SYMBOL);
			return nullptr;
		}

		gbm_mesa_drv->wrapper = get_gbm_ops();
		if (gbm_mesa_drv->wrapper == nullptr) {
			drv_loge("Unable to get wrapper ops");
			return nullptr;
		}

		gbm_mesa_drv->gbm_dev =
		    gbm_mesa_drv->wrapper->dev_create(gbm_mesa_drv->gbm_node_fd.Get());
		if (!gbm_mesa_drv->gbm_dev) {
			drv_loge("Unable to create gbm_mesa driver");
			return nullptr;
		}

		/* Create GPU device for fallback allocations when KMS (CMA) is exhausted */
		if (gbm_mesa_drv->has_separate_gpu && gbm_mesa_drv->gpu_node_fd.Get() >= 0) {
			gbm_mesa_drv->gbm_gpu_dev =
			    gbm_mesa_drv->wrapper->dev_create(gbm_mesa_drv->gpu_node_fd.Get());
			if (gbm_mesa_drv->gbm_gpu_dev) {
				drv_logi("GPU fallback device initialized (for non-scanout buffers)\n");
			} else {
				drv_logi("Failed to create GPU fallback device, CMA exhaustion may cause failures\n");
			}
		}

		drv_logi("GBM Mesa driver initialized successfully\n");

		auto priv = new GbmMesaDriverPriv();
		priv->gbm_mesa_drv = gbm_mesa_drv;
		drv->priv = priv;
	} else {
		gbm_mesa_drv = ((GbmMesaDriverPriv *)drv->priv)->gbm_mesa_drv;
	}

	return gbm_mesa_drv;
}

void gbm_mesa_driver_close(struct driver *drv)
{
	if (drv->priv) {
		delete (GbmMesaDriverPriv *)(drv->priv);
		drv->priv = nullptr;
	}
}

struct GbmMesaBoPriv {
	~GbmMesaBoPriv()
	{
		if (gbm_bo) {
			auto wr = drv->wrapper;
			wr->free(gbm_bo);
		}
	}

	std::shared_ptr<GbmMesaDriver> drv;
	uint32_t map_stride = 0;
	UniqueFd fds[DRV_MAX_PLANES];
	struct gbm_bo *gbm_bo = nullptr;
	bool use_dumb = false;  /* true if allocated via DRM dumb buffer */
};

/* Allocate a dumb buffer via DRM for formats not supported by GBM */
static int gbm_mesa_alloc_dumb(struct bo *bo, uint32_t width, uint32_t height,
			       uint32_t format, uint64_t use_flags,
			       std::shared_ptr<GbmMesaDriver> gbm_drv)
{
	/* Use the KMS node fd for dumb buffer allocation, not the GPU fd.
	 * The GPU fd (Panfrost) doesn't support dumb buffer ioctls,
	 * but the KMS fd (meson-drm) does. */
	int fd = gbm_drv->gbm_node_fd.Get();
	if (fd < 0) {
		drv_loge("gbm_mesa_alloc_dumb: no valid KMS fd");
		return -EINVAL;
	}

	struct drm_mode_create_dumb create_arg = {};
	int ret;

	/* Calculate layout first */
	drv_bo_from_format(bo, width, 1, height, format);

	/* Align for camera if needed */
	if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
		bo->meta.total_size = ALIGN(bo->meta.total_size, 4096);
	}

	/* Create dumb buffer - use total size as width with height=1 for simplicity */
	create_arg.width = bo->meta.total_size;
	create_arg.height = 1;
	create_arg.bpp = 8;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	if (ret) {
		drv_loge("DRM_IOCTL_MODE_CREATE_DUMB failed: %s (fd=%d)", strerror(errno), fd);
		return -errno;
	}

	/* Export as dma-buf */
	int prime_fd = -1;
	ret = drmPrimeHandleToFD(fd, create_arg.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd);
	if (ret) {
		drv_loge("drmPrimeHandleToFD failed: %s", strerror(errno));
		struct drm_mode_destroy_dumb destroy_arg = { .handle = create_arg.handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
		return -errno;
	}

	/* Destroy the GEM handle - we'll use the dma-buf fd from now on */
	struct drm_mode_destroy_dumb destroy_arg = { .handle = create_arg.handle };
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

	auto priv = new GbmMesaBoPriv();
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		priv->fds[plane] = UniqueFd(dup(prime_fd));
	}
	close(prime_fd);

	priv->drv = gbm_drv;
	priv->map_stride = bo->meta.strides[0];
	priv->use_dumb = true;
	bo->meta.format_modifier = DRM_FORMAT_MOD_LINEAR;
	bo->priv = priv;

	drv_logv("Allocated dumb buffer: %ux%u format=0x%x size=%u",
		 width, height, format, bo->meta.total_size);

	return 0;
}

int gbm_mesa_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
		       uint64_t use_flags)
{
	auto drv = gbm_mesa_get_or_init_driver(bo->drv, false);
	if (drv == nullptr) {
		drv_loge("Failed to init gbm driver");
		return -EINVAL;
	}

	auto wr = drv->wrapper;

	/* For YUV formats not supported by GBM (like NV12), use dumb buffers.
	 * These are CPU-only formats used by camera/video, not GPU textures. */
	if (wr->get_gbm_format(format) == 0 && is_yuv_format(format)) {
		drv_logv("Format 0x%x not supported by GBM, using dumb buffer", format);
		return gbm_mesa_alloc_dumb(bo, width, height, format, use_flags, drv);
	}

	/* Fail early for unsupported non-YUV formats */
	if (wr->get_gbm_format(format) == 0) {
		drv_loge("Format 0x%x not supported by GBM and not a YUV format", format);
		return -EINVAL;
	}

	/* For some ARM SOCs, if no more free CMA available, buffer can be allocated in VRAM but HWC
	 * won't be able to display it directly, using GPU for compositing */
	bool scanout_strong = false;
	bool bo_layout_ready = false;
	uint32_t size_align = 1;
	int err = 0;

	struct alloc_args alloc_args = {
		.gbm = drv->gbm_dev,
		.width = width,
		.height = height,
		.drm_format = format,
		// Force LINEAR for SW access OR scanout (needed for PRIME compatibility)
		.force_linear = ((use_flags & BO_USE_SW_MASK) != 0) ||
		                ((use_flags & BO_USE_SCANOUT) != 0),
		.use_scanout = (use_flags & BO_USE_SCANOUT) != 0,
		.needs_map_stride = (use_flags & BO_USE_SW_MASK) != 0,
	};

	/* Alignment for RPI4 CSI camera. Since we do not care about other cameras, keep this
	 * globally for now.
	 * TODO: Create/use constraints table for camera/codecs */
	if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
		scanout_strong = true;
		alloc_args.use_scanout = true;
		alloc_args.width = ALIGN(alloc_args.width, 32);
		size_align = 4096;
	}

	err = wr->alloc(&alloc_args);

	if (err && !scanout_strong) {
		drv_loge("Failed to allocate for scanout, trying non-scanout");
		alloc_args.use_scanout = false;
		err = wr->alloc(&alloc_args);
	}

	/* Fallback to GPU device when KMS (CMA) allocation fails.
	 * This is safe for virtual displays and composited buffers that don't
	 * need direct scanout on the physical display controller.
	 * Camera buffers (scanout_strong) must stay on KMS for DMA coherency. */
	if (err && !scanout_strong && drv->has_separate_gpu && drv->gbm_gpu_dev) {
		drv_logi("KMS allocation failed (CMA exhausted?), trying GPU fallback");
		alloc_args.gbm = drv->gbm_gpu_dev;
		alloc_args.use_scanout = false;
		alloc_args.force_linear = true;  /* Keep linear for SW access compatibility */
		err = wr->alloc(&alloc_args);
		if (!err) {
			drv_logi("GPU fallback allocation succeeded for %dx%d format=0x%x",
				 width, height, format);
		}
	}

	if (err) {
		/* Only use dumb buffer fallback for YUV formats (camera/video).
		 * RGB formats must be GPU-compatible and cannot use dumb buffers
		 * as they need to be imported as textures by Panfrost. */
		if (is_yuv_format(format)) {
			drv_loge("GBM alloc failed for YUV format, falling back to dumb buffer");
			return gbm_mesa_alloc_dumb(bo, width, height, format, use_flags, drv);
		}
		drv_loge("Failed to allocate buffer %dx%d format=0x%x use_flags=0x%llx err=%d",
			 width, height, format, (unsigned long long)use_flags, err);
		return err;
	}
	drv_logi("gbm_mesa_bo_create OK: %dx%d format=0x%x use_flags=0x%llx",
		 width, height, format, (unsigned long long)use_flags);

	if (!bo_layout_ready)
		drv_bo_from_format(bo, alloc_args.out_stride, 1, alloc_args.height, format);

	drv_logv("Allocated: %dx%d, stride: %d, map_stride: %d", width, height,
		 alloc_args.out_stride, alloc_args.out_map_stride);

	auto priv = new GbmMesaBoPriv();
	// Note: inode tracking removed in newer minigbm versions
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		priv->fds[plane] = UniqueFd(alloc_args.out_fd);
	}

	priv->map_stride = alloc_args.out_map_stride;
	bo->meta.format_modifier = alloc_args.out_modifier;

	bo->priv = priv;
	priv->drv = drv;

	return 0;
}

int gbm_mesa_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	if (bo->priv) {
		drv_loge("%s bo isn't empty", __func__);
		return -EINVAL;
	}
	auto priv = new GbmMesaBoPriv();
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		priv->fds[plane] = UniqueFd(dup(data->fds[plane]));
	}

	if (data->use_flags & BO_USE_SW_MASK) {
		// Mapping require importing by gbm_mesa
		auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
		auto wr = drv->wrapper;

		uint32_t s_format = data->format;
		int s_height = data->height;
		int s_width = data->width;
		if (wr->get_gbm_format(s_format) == 0) {
			s_width = bo->meta.total_size;
			s_height = 1;
			s_format = DRM_FORMAT_R8;
		}

		priv->drv = drv;
		priv->gbm_bo = wr->import(drv->gbm_dev, data->fds[0], s_width, s_height,
					  data->strides[0], data->format_modifier, s_format);
	}

	bo->priv = priv;

	return 0;
}

int gbm_mesa_bo_destroy(struct bo *bo)
{
	if (bo->priv) {
		delete (GbmMesaBoPriv *)(bo->priv);
		bo->priv = nullptr;
	}
	return 0;
}

int gbm_mesa_bo_export(struct bo *bo, size_t plane)
{
	return dup(((GbmMesaBoPriv *)bo->priv)->fds[plane].Get());
}

void *gbm_mesa_bo_map_internal(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	auto priv = (GbmMesaBoPriv *)bo->priv;

	vma->length = bo->meta.total_size;

	void *buf = MAP_FAILED;

	/* If GBM import failed (gbm_bo is NULL), fallback to direct mmap on dma-buf fd */
	if (priv->gbm_bo == nullptr) {
		int fd = priv->fds[0].Get();
		if (fd < 0) {
			drv_loge("gbm_mesa_bo_map: no valid fd for mmap fallback");
			return MAP_FAILED;
		}

		int prot = PROT_READ | PROT_WRITE;
		buf = mmap(NULL, vma->length, prot, MAP_SHARED, fd, 0);
		if (buf == MAP_FAILED) {
			drv_loge("gbm_mesa_bo_map: mmap fallback failed: %s", strerror(errno));
			return MAP_FAILED;
		}
		vma->priv = NULL; /* No GBM map_data for direct mmap */
		return buf;
	}

	auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
	auto wr = drv->wrapper;

	uint32_t s_format = bo->meta.format;
	int s_width = bo->meta.width;
	int s_height = bo->meta.height;
	if (wr->get_gbm_format(s_format) == 0) {
		s_width = bo->meta.total_size;
		s_height = 1;
	}

	wr->map(priv->gbm_bo, s_width, s_height, &buf, &vma->priv);

	return buf;
}

int gbm_mesa_bo_unmap(struct bo *bo, struct vma *vma)
{
	auto priv = (GbmMesaBoPriv *)bo->priv;

	/* If this was a direct mmap (vma->priv is NULL), use munmap */
	if (priv->gbm_bo == nullptr || vma->priv == nullptr) {
		if (vma->addr && vma->addr != MAP_FAILED) {
			munmap(vma->addr, vma->length);
		}
		return 0;
	}

	auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
	auto wr = drv->wrapper;

	wr->unmap(priv->gbm_bo, vma->priv);
	vma->priv = nullptr;
	return 0;
}

// Backend API - newer minigbm removed plane parameter
void *gbm_mesa_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	// Map plane 0 by default (most buffers have single plane)
	return gbm_mesa_bo_map_internal(bo, vma, 0, map_flags);
}

// Legacy functions removed from backend struct in newer minigbm
// Kept for potential future use
uint32_t gbm_mesa_bo_get_map_stride(struct bo *bo)
{
	auto priv = (GbmMesaBoPriv *)bo->priv;

	return priv->map_stride;
}
