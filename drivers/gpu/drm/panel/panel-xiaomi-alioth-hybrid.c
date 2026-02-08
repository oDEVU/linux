// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi Alioth Hybrid Panel Driver
 * Wraps panel-samsung-ams667xx01 to support Generic Aftermarket LCDs.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define MODE_AUTO    0
#define MODE_OLED    1
#define MODE_LCD     2

static int mode = MODE_AUTO;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "Panel mode: 0=auto, 1=force OLED, 2=force LCD");

#undef mipi_dsi_driver_register
#undef module_mipi_dsi_driver

#define mipi_dsi_driver_register(...)  (0)
#define module_mipi_dsi_driver(...)

#include "panel-samsung-ams667xx01.c"

#undef mipi_dsi_driver_register
#undef module_mipi_dsi_driver

static const struct drm_display_mode alioth_lcd_mode = {
	.clock = (1080 + 16 + 8 + 8) * (2400 + 600 + 32 + 560) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 16,
	.hsync_end = 1080 + 16 + 8,
	.htotal = 1080 + 16 + 8 + 8,
	.vdisplay = 2400,
	.vsync_start = 2400 + 600,
	.vsync_end = 2400 + 600 + 32,
	.vtotal = 2400 + 600 + 32 + 560,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

struct alioth_hybrid_ctx {
	struct ams667xx01 base;
	bool is_lcd;
	bool detected;
};

#define to_hybrid_ctx(ptr) container_of(ptr, struct alioth_hybrid_ctx, base)

static int alioth_hybrid_prepare(struct drm_panel *panel)
{
	struct ams667xx01 *ctx = to_ams667xx01(panel);
	struct alioth_hybrid_ctx *hybrid_ctx = to_hybrid_ctx(ctx);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	u8 id_buf[3] = {0};
	int ret;
	bool is_lcd = false;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	if (!hybrid_ctx->detected) {
		switch (mode) {
		case MODE_OLED:
			dev_info(dev, "Forced OLED mode\n");
			is_lcd = false;
			break;
		case MODE_LCD:
			dev_info(dev, "Forced LCD mode\n");
			is_lcd = true;
			break;
		case MODE_AUTO:
		default:
			dsi->mode_flags |= MIPI_DSI_MODE_LPM;
			ret = mipi_dsi_dcs_read(dsi, 0xDA, id_buf, sizeof(id_buf));

			if (ret > 0 && id_buf[0] == 0x05) {
				dev_info(dev, "Detected Samsung OLED (ID 0x%02x)\n", id_buf[0]);
				is_lcd = false;
			} else {
				dev_info(dev, "Detected Generic LCD (ID 0x%02x)\n", id_buf[0]);
				is_lcd = true;
			}
			break;
		}

		hybrid_ctx->is_lcd = is_lcd;
		hybrid_ctx->detected = true;
	} else {
		is_lcd = hybrid_ctx->is_lcd;
	}

	if (!is_lcd) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ams667xx01_prepare(panel);
	}

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	dsi->mode_flags |= MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	dsi->dsc = NULL;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		goto err;
	msleep(120);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0)
		dev_warn(dev, "Failed to set tear on: %d\n", ret);

	mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x0437);
	mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x095f);

	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		goto err;
	msleep(28);

	return 0;

err:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	return ret;
}

static int alioth_hybrid_enable(struct drm_panel *panel)
{
	struct ams667xx01 *ctx = to_ams667xx01(panel);
	struct alioth_hybrid_ctx *hybrid_ctx = to_hybrid_ctx(ctx);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	if (!hybrid_ctx->is_lcd)
		return ams667xx01_enable(panel);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	msleep(28);
	return 0;
}

static int alioth_hybrid_disable(struct drm_panel *panel)
{
	struct ams667xx01 *ctx = to_ams667xx01(panel);
	struct alioth_hybrid_ctx *hybrid_ctx = to_hybrid_ctx(ctx);
	struct mipi_dsi_device *dsi = ctx->dsi;

	if (!hybrid_ctx->is_lcd)
		return ams667xx01_disable(panel);

	mipi_dsi_dcs_set_display_off(dsi);
	msleep(20);

	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	mipi_dsi_dcs_enter_sleep_mode(dsi);
	msleep(120);

	return 0;
}

static int alioth_hybrid_unprepare(struct drm_panel *panel)
{
	struct ams667xx01 *ctx = to_ams667xx01(panel);
	struct alioth_hybrid_ctx *hybrid_ctx = to_hybrid_ctx(ctx);

	if (!hybrid_ctx->is_lcd)
		return ams667xx01_unprepare(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int alioth_hybrid_get_modes(struct drm_panel *panel,
				   struct drm_connector *connector)
{
	struct ams667xx01 *ctx = to_ams667xx01(panel);
	struct alioth_hybrid_ctx *hybrid_ctx = to_hybrid_ctx(ctx);
	struct drm_display_mode *mode;

	if (!hybrid_ctx->is_lcd) {
		return ams667xx01_get_modes(panel, connector);
	}

	mode = drm_mode_duplicate(connector->dev, &alioth_lcd_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = ctx->desc->width_mm;
	connector->display_info.height_mm = ctx->desc->height_mm;
	connector->display_info.bpc = ctx->desc->bpc;
	ctx->connector = connector;

	return 1;
}

static const struct drm_panel_funcs alioth_hybrid_funcs = {
	.prepare = alioth_hybrid_prepare,
	.enable = alioth_hybrid_enable,
	.disable = alioth_hybrid_disable,
	.unprepare = alioth_hybrid_unprepare,
	.get_modes = alioth_hybrid_get_modes,
};

static int alioth_hybrid_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct alioth_hybrid_ctx *hybrid_ctx;
	struct ams667xx01 *ctx;
	int ret;

	hybrid_ctx = devm_kzalloc(dev, sizeof(*hybrid_ctx), GFP_KERNEL);
	if (!hybrid_ctx)
		return -ENOMEM;

	ctx = &hybrid_ctx->base;
	hybrid_ctx->is_lcd = false;
	hybrid_ctx->detected = false;

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->desc = of_device_get_match_data(dev);
	if (!ctx->desc)
		return -ENODEV;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	drm_panel_init(&ctx->panel, dev, &alioth_hybrid_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ams667xx01_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	dsi->lanes = ctx->desc->lanes;
	dsi->format = ctx->desc->format;
	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->dsc = ctx->desc->dsc;
	dsi->dsc_slice_per_pkt = 2;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	dev_info(dev, "Alioth hybrid panel probed (mode=%d)\n", mode);
	return 0;
}

static void alioth_hybrid_remove(struct mipi_dsi_device *dsi)
{
	struct ams667xx01 *ctx = mipi_dsi_get_drvdata(dsi);
	drm_panel_remove(&ctx->panel);
}

static struct mipi_dsi_driver alioth_hybrid_driver = {
	.probe = alioth_hybrid_probe,
	.remove = alioth_hybrid_remove,
	.driver = {
		.name = "panel-xiaomi-alioth-hybrid",
		.of_match_table = ams667xx01_of_match,
	},
};

static int __init alioth_hybrid_init(void)
{
	return mipi_dsi_driver_register_full(&alioth_hybrid_driver, THIS_MODULE);
}
late_initcall(alioth_hybrid_init);

static void __exit alioth_hybrid_exit(void)
{
	mipi_dsi_driver_unregister(&alioth_hybrid_driver);
}
__exitcall(alioth_hybrid_exit);

MODULE_AUTHOR("DEVU");
MODULE_DESCRIPTION("Xiaomi Alioth Hybrid Panel Driver");
MODULE_LICENSE("GPL");
