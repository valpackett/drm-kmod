/*-
 * Copyright (c) 2024 Val Packett
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <linux/module.h>

#include <sys/fbio.h>
#include <dev/vt/vt.h>
#include <dev/vt/hw/fb/vt_fb.h>

#include <drm/drm_atomic.h>
// #include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_internal.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_probe_helper.h>

extern struct vt_device *main_vd;

static const struct {
	u32 fourcc;
	u32 bpp;
	struct fb_rgboffs rgb;
} bsd_formats[] = 
{
	{ DRM_FORMAT_RGB565, 16, {11, 5, 0} },
	{ DRM_FORMAT_RGBA5551, 16, {11, 6, 1} },
	{ DRM_FORMAT_XRGB1555, 16, {10, 5, 0} },
	{ DRM_FORMAT_ARGB1555, 16, {10, 5, 0} },
	{ DRM_FORMAT_RGB888, 24, {16, 8, 0} },
	{ DRM_FORMAT_XRGB8888, 32, {16, 8, 0} },
	{ DRM_FORMAT_ARGB8888, 32, {16, 8, 0} },
	{ DRM_FORMAT_XBGR8888, 32, {0, 8, 16} },
	{ DRM_FORMAT_ABGR8888, 32, {0, 8, 16} },
	{ DRM_FORMAT_XRGB2101010, 32, {20, 10, 0} },
	{ DRM_FORMAT_ARGB2101010, 32, {20, 10, 0} },
};

DEFINE_DRM_GEM_FOPS(bsdfb_fops);

static struct drm_driver bsdfb_driver = {
	.name			= "bsdfb",
	.desc			= "DRM driver for the FreeBSD framebuffer",
	.date			= "20240525",
	.major			= 1,
	.minor			= 0,
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &bsdfb_fops,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	DRM_GEM_SHMEM_DRIVER_OPS,
};


static struct class bsdfb_class = {
	.name = "bsdfb",
};

static struct device *base_dev;
static struct drm_device *dev;

static const struct drm_format_info *native_format = NULL;
static uint32_t formats[2] = {};
static size_t nformats = 0;

static struct drm_display_mode mode;
static struct drm_plane plane;
static struct drm_crtc crtc;
static struct drm_encoder encoder;
static struct drm_connector connector;

static int bsdfb_plane_atomic_check(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	// struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state,
	// 									 plane);
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!new_plane_state->fb || WARN_ON(!new_plane_state->crtc))
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state,
					       new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, true);
	return ret;
}

static void bsdfb_primary_plane_update(struct drm_plane *plane,
					    struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state,
									   plane);
	struct drm_framebuffer *fb = plane->state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	struct iosys_map map;
	struct fb_info *info = main_vd->vd_softc;
	unsigned int pitch = drm_format_info_min_pitch(native_format, 0, info->fb_width);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	info->fb_flags |= FB_FLAG_NOWRITE;

	drm_gem_vmap(fb->obj[0], &map);

	drm_atomic_helper_damage_iter_init(&iter, old_state, plane->state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct drm_rect dst_clip = plane->state->dst;
		struct iosys_map dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		iosys_map_set_vaddr(&dst, (void*)info->fb_vbase);
		iosys_map_incr(&dst, drm_fb_clip_offset(pitch, native_format, &dst_clip));
		drm_fb_blit(&dst, &pitch, native_format->format, &map, fb, &damage);
	}

	drm_dev_exit(idx);
}

static void bsdfb_plane_atomic_disable(struct drm_plane *plane,
					    struct drm_atomic_state *state) {
	VT_LOCK(main_vd);
	struct fb_info *info = main_vd->vd_softc;
	info->fb_flags &= ~FB_FLAG_NOWRITE;
	// main_vd->vd_driver->vd_blank(main_vd, TC_BLACK);
	main_vd->vd_flags |= VDF_INVALID;
	vt_resume_flush_timer(main_vd->vd_curwindow, 0);
	VT_UNLOCK(main_vd);
	// vt_fb_invalidate_text(main_vd, &main_vd->vd_curwindow->vw_draw_area);
}

static const struct drm_plane_helper_funcs bsdfb_primary_plane_helper_funcs = {
	.atomic_disable = bsdfb_plane_atomic_disable,
	.atomic_check = bsdfb_plane_atomic_check,
	.atomic_update = bsdfb_primary_plane_update,
};

static const struct drm_plane_funcs bsdfb_primary_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};


static enum drm_mode_status bsdfb_crtc_helper_mode_valid(struct drm_crtc *crtc,
							     const struct drm_display_mode *test_mode)
{
	return drm_crtc_helper_mode_valid_fixed(crtc, test_mode, &mode);
}

static int bsdfb_crtc_helper_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	/* Nothing to check really. */
	return 0;
}

static const struct drm_crtc_helper_funcs bsdfb_crtc_helper_funcs = {
	.mode_valid = bsdfb_crtc_helper_mode_valid,
	.atomic_check = bsdfb_crtc_helper_atomic_check,
};

static const struct drm_crtc_funcs bsdfb_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs bsdfb_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int bsdfb_connector_helper_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &mode);
}

static const struct drm_connector_helper_funcs bsdfb_connector_helper_funcs = {
	.get_modes = bsdfb_connector_helper_get_modes,
};

static const struct drm_connector_funcs bsdfb_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs bsdfb_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};


static int bsdfb_create(void)
{
	struct fb_info *info = main_vd->vd_softc;
	int ret;

	if ((info->fb_flags & FB_FLAG_NOMMAP) == FB_FLAG_NOMMAP) {
		DRM_ERROR("current framebuffer does not support memory mapping");
		return (-ENOTSUP);
	}

	for (int i = 0; i < ARRAY_SIZE(bsd_formats); i++) {
		if (bsd_formats[i].bpp == info->fb_bpp &&
		    bsd_formats[i].rgb.red == info->fb_rgboffs.red &&
		    bsd_formats[i].rgb.green == info->fb_rgboffs.green &&
		    bsd_formats[i].rgb.blue == info->fb_rgboffs.blue) {
			native_format = drm_format_info(bsd_formats[i].fourcc);
			break;
		}
	}
	if (!native_format) {
		DRM_ERROR("could not find a matching pixel format");
		return (-ENOTSUP);
	}

	base_dev = device_create(&bsdfb_class, &linux_root_device,
				     MKDEV(0, 0), NULL,
				     "bsdfb%d", 0);
	if (IS_ERR(base_dev))
		return PTR_ERR(base_dev);

	dev = drm_dev_alloc(&bsdfb_driver, base_dev);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	ret = drmm_mode_config_init(dev);
	if (ret)
		return (ret);

	dev->mode_config.min_width = info->fb_width;
	dev->mode_config.max_width = info->fb_width;
	dev->mode_config.min_height = info->fb_height;
	dev->mode_config.max_height = info->fb_height;
	dev->mode_config.preferred_depth = info->fb_depth;
	dev->mode_config.funcs = &bsdfb_mode_config_funcs;

	const struct drm_display_mode mode_init = {
		DRM_MODE_INIT(60, info->fb_width, info->fb_height, 0, 0)
	};
	mode = mode_init;

	static const uint32_t extra_fourcc = DRM_FORMAT_XRGB8888;
	nformats = drm_fb_build_fourcc_list(dev, &native_format->format, 1, &extra_fourcc, 1,
					    formats, ARRAY_SIZE(formats));

	static const uint64_t format_modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID
	};
	ret = drm_universal_plane_init(dev, &plane, 0, &bsdfb_primary_plane_funcs,
				       formats, nformats,
				       format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return (ret);
	drm_plane_helper_add(&plane, &bsdfb_primary_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&plane);

	ret = drm_crtc_init_with_planes(dev, &crtc, &plane, NULL,
					&bsdfb_crtc_funcs, NULL);
	if (ret)
		return (ret);
	drm_crtc_helper_add(&crtc, &bsdfb_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &encoder, &bsdfb_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return (ret);
	encoder.possible_crtcs = drm_crtc_mask(&crtc);

	ret = drm_connector_init(dev, &connector, &bsdfb_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return (ret);
	drm_connector_helper_add(&connector, &bsdfb_connector_helper_funcs);
	drm_connector_set_panel_orientation_with_quirk(&connector,
						       DRM_MODE_PANEL_ORIENTATION_UNKNOWN,
						       info->fb_width, info->fb_height);

	ret = drm_connector_attach_encoder(&connector, &encoder);
	if (ret)
		return (ret);

	drm_mode_config_reset(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return (ret);

	// drm_fbdev_generic_setup(dev, 0);

	return (0);
}

static void bsdfb_destroy(void)
{
	if (dev) {
		drm_dev_unplug(dev);
		device_destroy(&bsdfb_class, MKDEV(0, 0));
	}
	base_dev = NULL;
	dev = NULL;
}

static int __init bsdfb_init(void)
{
	int ret;

	ret = class_register(&bsdfb_class);
	if (ret)
		return (ret);

	return (bsdfb_create());
}

static void __exit bsdfb_exit(void)
{
	bsdfb_destroy();
	class_unregister(&bsdfb_class);
}

LKPI_DRIVER_MODULE(bsdfb, bsdfb_init, bsdfb_exit);
MODULE_DEPEND(bsdfb, drmn, 2, 2, 2);
MODULE_DEPEND(bsdfb, linuxkpi, 1, 1, 1);
MODULE_DEPEND(bsdfb, linuxkpi_video, 1, 1, 1);
MODULE_DEPEND(bsdfb, dmabuf, 1, 1, 1);
