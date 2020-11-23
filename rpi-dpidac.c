/*
 * Copyright (C) 2018 Hugh Cole-Baker
 *
 * Hugh Cole-Baker <sigmaris@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/module.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>

struct dpidac {
	struct drm_bridge	bridge;
	struct drm_connector	connector;
	struct display_timings	*timings;
};

static inline struct dpidac *
drm_bridge_to_dpidac(struct drm_bridge *bridge)
{
	return container_of(bridge, struct dpidac, bridge);
}

static inline struct dpidac *
drm_connector_to_dpidac(struct drm_connector *connector)
{
	return container_of(connector, struct dpidac, connector);
}

static int dpidac_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct dpidac *vga = drm_connector_to_dpidac(connector);
	struct display_timings *timings = vga->timings;
	int i;

	if(timings) {
		DRM_DEBUG("using display-timings to create modes\n");
		for (i = 0; i < timings->num_timings; i++) {
			struct drm_display_mode *mode = drm_mode_create(dev);
			struct videomode vm;

			if (videomode_from_timings(timings, &vm, i))
				break;

			drm_display_mode_from_videomode(&vm, mode);

			mode->type = DRM_MODE_TYPE_DRIVER;

			if (timings->native_mode == i)
				mode->type |= DRM_MODE_TYPE_PREFERRED;

			drm_mode_set_name(mode);
			drm_mode_probed_add(connector, mode);
		}
	} else {
		DRM_DEBUG("fallback to XGA modes\n");
		/* Since there is no timing data, use XGA standard modes */
		i = drm_add_modes_noedid(connector, 1920, 1200);

		/* And prefer a mode pretty much anyone can handle */
		drm_set_preferred_mode(connector, 1024, 768);
	}

	return i;
}

static const struct drm_connector_helper_funcs dpidac_con_helper_funcs = {
	.get_modes	= dpidac_get_modes,
};

static enum drm_connector_status
dpidac_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_funcs dpidac_con_funcs = {
	.detect			= dpidac_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int dpidac_attach(struct drm_bridge *bridge)
{
	struct dpidac *vga = drm_bridge_to_dpidac(bridge);
	u32 bus_format = MEDIA_BUS_FMT_RGB666_1X18;
	int ret;

	if (!bridge->encoder) {
		DRM_ERROR("Missing encoder\n");
		return -ENODEV;
	}

	drm_connector_helper_add(&vga->connector,
				 &dpidac_con_helper_funcs);
	ret = drm_connector_init(bridge->dev, &vga->connector,
				 &dpidac_con_funcs, DRM_MODE_CONNECTOR_VGA);
	if (ret) {
		DRM_ERROR("Failed to initialize connector\n");
		return ret;
	}
	ret = drm_display_info_set_bus_formats(&vga->connector.display_info,
					       &bus_format, 1);
	if (ret) {
		DRM_ERROR("Failed to set bus format\n");
		return ret;
	}

	vga->connector.interlace_allowed = 1;
	vga->connector.doublescan_allowed = 1;

	drm_connector_attach_encoder(&vga->connector,
					  bridge->encoder);

	return 0;
}

static const struct drm_bridge_funcs dpidac_bridge_funcs = {
	.attach		= dpidac_attach,
};

static int dpidac_probe(struct platform_device *pdev)
{
	struct dpidac *vga;

	vga = devm_kzalloc(&pdev->dev, sizeof(*vga), GFP_KERNEL);
	if (!vga)
		return -ENOMEM;
	platform_set_drvdata(pdev, vga);

	vga->timings = of_get_display_timings(pdev->dev.of_node);
	DRM_DEBUG("display-timings from DT: %p\n", vga->timings);

	vga->bridge.funcs = &dpidac_bridge_funcs;
	vga->bridge.of_node = pdev->dev.of_node;

	drm_bridge_add(&vga->bridge);

	return 0;
}

static int dpidac_remove(struct platform_device *pdev)
{
	struct dpidac *vga = platform_get_drvdata(pdev);

	if (vga->timings) {
		display_timings_release(vga->timings);
	}

	drm_bridge_remove(&vga->bridge);

	return 0;
}

static const struct of_device_id dpidac_match[] = {
	{ .compatible = "raspberrypi,dpidac" },
	{},
};
MODULE_DEVICE_TABLE(of, dpidac_match);

static struct platform_driver dpidac_driver = {
	.probe	= dpidac_probe,
	.remove	= dpidac_remove,
	.driver		= {
		.name		= "rpi-dpidac",
		.of_match_table	= dpidac_match,
	},
};
module_platform_driver(dpidac_driver);

MODULE_AUTHOR("Hugh Cole-Baker <sigmaris@gmail.com>");
MODULE_DESCRIPTION("Raspberry Pi DPI DAC bridge driver");
MODULE_LICENSE("GPL");
