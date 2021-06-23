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
#include <linux/platform_device.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

// ~ 20 timings line + comments
#define READ_SIZE_MAX 2048
#define LINE_SIZE_MAX 256

static char read_buf[READ_SIZE_MAX];
static const char *timings_path = "/boot/timings.txt";

static struct drm_display_mode *dpidac_display_mode_from_timings(struct drm_connector *connector, const char *line) {
    int ret, hsync, vsync, interlace, ratio;
    struct drm_display_mode *mode = NULL;
    struct videomode vm;

    if (line != NULL) {
        memset(&vm, 0, sizeof(vm));
        ret = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %*s %*s %*s %*s %d %ld %d",
                     &vm.hactive, &hsync, &vm.hfront_porch, &vm.hsync_len, &vm.hback_porch,
                     &vm.vactive, &vsync, &vm.vfront_porch, &vm.vsync_len, &vm.vback_porch,
                     &interlace, &vm.pixelclock, &ratio);
        if (ret != 13) {
            printk(KERN_WARNING "[RPI-DPIDAC]: malformed mode requested, skipping (%s)\n", line);
            return NULL;
        }

        // setup flags
        vm.flags = interlace ? DISPLAY_FLAGS_INTERLACED : 0;
        vm.flags |= hsync ? DISPLAY_FLAGS_HSYNC_LOW : DISPLAY_FLAGS_HSYNC_HIGH;
        vm.flags |= vsync ? DISPLAY_FLAGS_VSYNC_LOW : DISPLAY_FLAGS_VSYNC_HIGH;

        // create/init display mode, convert from video mode
        mode = drm_mode_create(connector->dev);
        if (mode == NULL) {
            printk(KERN_WARNING "[RPI-DPIDAC]: drm_mode_create failed, skipping (%s)\n", line);
            return NULL;
        }

        drm_display_mode_from_videomode(&vm, mode);

        return mode;
    }

    return NULL;
}

int dpidac_load_timings(struct drm_connector *connector) {
    struct file *fp = NULL;
    ssize_t read_size = 0;
    size_t cursor = 0;
    char line[LINE_SIZE_MAX];
    size_t line_start = 0;
    size_t line_len = 0;
    struct drm_display_mode *mode = NULL;
    int mode_count = 0;

    fp = filp_open(timings_path, O_RDONLY, 0);
    if (IS_ERR(fp) || !fp) {
        printk(KERN_WARNING "[RPI-DPIDAC]: timings file not found, skipping custom modes loading\n");
        return 0;
    }

    read_size = kernel_read(fp, &read_buf, READ_SIZE_MAX, &fp->f_pos);
    if (read_size <= 0) {
        filp_close(fp, NULL);
        printk(KERN_WARNING "[RPI-DPIDAC]: empty timings file found, skipping custom modes loading\n");
        return 0;
    }
    filp_close(fp, NULL);

    for (cursor = 0; cursor < read_size; cursor++) {
        line[cursor - line_start] = read_buf[cursor];
        line_len++;
        if (line_len >= LINE_SIZE_MAX || read_buf[cursor] == '\n' || read_buf[cursor] == '\0') {
            if (line_len > 32 && line[0] != '#') {
                line[line_len - 1] = '\0';
                if ((mode = dpidac_display_mode_from_timings(connector, line)) != NULL) {
                    mode->type = mode_count ? DRM_MODE_TYPE_DRIVER : DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
                    //printk(KERN_INFO "[RPI-DPIDAC]: \t" DRM_MODE_FMT, DRM_MODE_ARG(mode));
                    drm_mode_probed_add(connector, mode);
                    mode_count++;
                }
            }
            line_start += line_len;
            line_len = 0;
            memset(line, 0, 128);
        }
    }

    return mode_count;
}

struct dpidac {
    struct drm_bridge bridge;
    struct drm_connector connector;
    struct display_timings *timings;
};

static inline struct dpidac *drm_bridge_to_dpidac(struct drm_bridge *bridge) {
    return container_of(bridge, struct dpidac, bridge);
}

static inline struct dpidac *drm_connector_to_dpidac(struct drm_connector *connector) {
    return container_of(connector, struct dpidac, connector);
}

static int dpidac_get_modes(struct drm_connector *connector) {
    struct drm_device *dev = connector->dev;
    struct dpidac *vga = drm_connector_to_dpidac(connector);
    struct display_timings *timings = vga->timings;
    int i;

    i = dpidac_load_timings(connector);
    if (i) {
        //printk(KERN_INFO "[RPI-DPIDAC]: dpidac_get_modes: %i custom modes loaded\n", i);
        return i;
    } else if (timings) {
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
        //printk(KERN_INFO "[RPI-DPIDAC]: dpidac_get_modes: %i modes loaded from dtb overlay\n", i);
    } else {
        /* Since there is no timing data, use XGA standard modes */
        i = drm_add_modes_noedid(connector, 1920, 1200);
        /* And prefer a mode pretty much anyone can handle */
        drm_set_preferred_mode(connector, 1024, 768);
        //printk(KERN_INFO "[RPI-DPIDAC]: dpidac_get_modes: fallback to XGA mode...\n");
    }

    return i;
}

static const struct drm_connector_helper_funcs dpidac_con_helper_funcs = {
        .get_modes    = dpidac_get_modes,
};

static enum drm_connector_status dpidac_connector_detect(struct drm_connector *connector, bool force) {
    return connector_status_connected;
}

static const struct drm_connector_funcs dpidac_con_funcs = {
        .detect            = dpidac_connector_detect,
        .fill_modes        = drm_helper_probe_single_connector_modes,
        .destroy        = drm_connector_cleanup,
        .reset            = drm_atomic_helper_connector_reset,
        .atomic_duplicate_state    = drm_atomic_helper_connector_duplicate_state,
        .atomic_destroy_state    = drm_atomic_helper_connector_destroy_state,
};

static int dpidac_attach(struct drm_bridge *bridge, enum drm_bridge_attach_flags flags) {
    struct dpidac *vga = drm_bridge_to_dpidac(bridge);
    u32 bus_format = MEDIA_BUS_FMT_RGB666_1X18;
    u32 mode;
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

    of_property_read_u32(vga->bridge.of_node, "vc4-vga666-mode", &mode);
    printk(KERN_INFO "[RPI-DPIDAC]: vc4-vga666 mode: %i\n", mode);
    if(mode == 6) {
        bus_format = MEDIA_BUS_FMT_RGB666_1X24_CPADHI;
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
        .attach        = dpidac_attach,
};

static int dpidac_probe(struct platform_device *pdev) {
    struct dpidac *vga;

    vga = devm_kzalloc(&pdev->dev, sizeof(*vga), GFP_KERNEL);
    if (!vga)
        return -ENOMEM;
    platform_set_drvdata(pdev, vga);

    vga->timings = of_get_display_timings(pdev->dev.of_node);
    printk(KERN_INFO "[RPI-DPIDAC]: display-timings from DT: %p\n", vga->timings);

    vga->bridge.funcs = &dpidac_bridge_funcs;
    vga->bridge.of_node = pdev->dev.of_node;

    drm_bridge_add(&vga->bridge);

    return 0;
}

static int dpidac_remove(struct platform_device *pdev) {
    struct dpidac *vga = platform_get_drvdata(pdev);

    if (vga->timings) {
        display_timings_release(vga->timings);
    }

    drm_bridge_remove(&vga->bridge);

    return 0;
}

static const struct of_device_id dpidac_match[] = {
        {.compatible = "raspberrypi,dpidac"},
        {},
};
MODULE_DEVICE_TABLE(of, dpidac_match);

static struct platform_driver dpidac_driver = {
        .probe  = dpidac_probe,
        .remove = dpidac_remove,
        .driver = {
                .name        = "rpi-dpidac",
                .of_match_table    = dpidac_match,
        },
};

module_platform_driver(dpidac_driver);

MODULE_AUTHOR("Hugh Cole-Baker and cpasjuste");
MODULE_DESCRIPTION("Raspberry Pi DPI DAC bridge driver");
MODULE_LICENSE("GPL");
