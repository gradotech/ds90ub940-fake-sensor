// SPDX-License-Identifier: GPL-2.0
/*
 * DS90UB940 fake sensor driver.
 * Copyright (C) 2024, Grado Technologies Ltd
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define DS90UB940_MAX_PCLK (154 * 1000000)

struct ds90ub940_mode {
	unsigned int width;
	unsigned int height;
	unsigned int code;
	struct v4l2_fract fps;
};

static const struct ds90ub940_mode output_modes[] = {
	{
		.width	= 1920,
		.height	= 1200,
		.code	= MEDIA_BUS_FMT_BGR888_1X24,
		.fps    = {
			.numerator = 10000,
			.denominator = 600000,
		},
	},
};

static const s64 link_freqs[] = {
	414720000,
};

struct ds90ub940 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt fmt;
	const struct ds90ub940_mode *mode;
	struct mutex mutex;
	bool streaming;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct {
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};
};

static inline struct ds90ub940 *to_ds90ub940(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct ds90ub940, sd);
}

static void ds90ub940_set_default_format(struct ds90ub940 *ds90ub940)
{
	struct v4l2_mbus_framefmt *fmt = &ds90ub940->fmt;

	ds90ub940->mode = &output_modes[0];

	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	fmt->width = ds90ub940->mode->width;
	fmt->height = ds90ub940->mode->height;
	fmt->field = V4L2_FIELD_NONE;
}

static int ds90ub940_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);
	struct v4l2_mbus_framefmt *try_fmt_img =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);

	mutex_lock(&ds90ub940->mutex);

	try_fmt_img->width = output_modes[0].width;
	try_fmt_img->height = output_modes[0].height;
	try_fmt_img->code = output_modes[0].code;
	try_fmt_img->field = V4L2_FIELD_NONE;

	mutex_unlock(&ds90ub940->mutex);

	return 0;
}

static int ds90ub940_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(output_modes))
		return -EINVAL;

	code->code = output_modes[code->index].code;

	return 0;
}

static int ds90ub940_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(output_modes))
		return -EINVAL;

	if (fse->code != output_modes[fse->index].code)
		return -EINVAL;

	fse->min_width = output_modes[0].width;
	fse->max_width = fse->min_width;
	fse->min_height = output_modes[0].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void ds90ub940_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static void ds90ub940_update_format(struct ds90ub940 *ds90ub940,
				    const struct ds90ub940_mode *mode,
				    struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	ds90ub940_reset_colorspace(&fmt->format);
}

static int ds90ub940_get_pad_format(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_format *fmt)
{
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);

	mutex_lock(&ds90ub940->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_get_try_format(&ds90ub940->sd, cfg, fmt->pad);

		try_fmt->code = ds90ub940->mode->code;
		fmt->format = *try_fmt;
	} else {
		ds90ub940_update_format(ds90ub940, ds90ub940->mode, fmt);
		fmt->format.code = ds90ub940->mode->code;
	}

	mutex_unlock(&ds90ub940->mutex);
	return 0;
}

static int ds90ub940_set_pad_format(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt;
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);
	const struct ds90ub940_mode *mode;

	mutex_lock(&ds90ub940->mutex);

	fmt->format.code = ds90ub940->mode->code;
	mode = v4l2_find_nearest_size(output_modes,
				      ARRAY_SIZE(output_modes),
				      width, height,
				      fmt->format.width,
				      fmt->format.height);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		*framefmt = fmt->format;
	} else {
		ds90ub940->mode = mode;
	}

	mutex_unlock(&ds90ub940->mutex);

	return 0;
}

static int ds90ub940_g_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi)
{
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);

	fi->interval = ds90ub940->mode->fps;

	return 0;
}

static int ds90ub940_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	mutex_lock(&ds90ub940->mutex);
	if (ds90ub940->streaming == enable) {
		mutex_unlock(&ds90ub940->mutex);
		return 0;
	}

	dev_dbg(&client->dev, "%s: enable %d", __func__, enable);

	ds90ub940->streaming = enable;

	mutex_unlock(&ds90ub940->mutex);

	return 0;
}

static const struct v4l2_subdev_core_ops ds90ub940_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ds90ub940_video_ops = {
	.s_stream = ds90ub940_set_stream,
	.g_frame_interval = ds90ub940_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops ds90ub940_pad_ops = {
	.enum_mbus_code = ds90ub940_enum_mbus_code,
	.get_fmt = ds90ub940_get_pad_format,
	.set_fmt = ds90ub940_set_pad_format,
	.enum_frame_size = ds90ub940_enum_frame_size,
};

static const struct v4l2_subdev_ops ds90ub940_subdev_ops = {
	.core = &ds90ub940_core_ops,
	.video = &ds90ub940_video_ops,
	.pad = &ds90ub940_pad_ops,
};

static const struct v4l2_subdev_internal_ops ds90ub940_internal_ops = {
	.open = ds90ub940_open,
};

static int ds90ub940_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;
}

static const struct v4l2_ctrl_ops ds90ub940_ctrl_ops = {
	.s_ctrl = ds90ub940_set_ctrl,
};

static int ds90ub940_init_controls(struct ds90ub940 *ds90ub940)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct i2c_client *client = v4l2_get_subdevdata(&ds90ub940->sd);
	struct v4l2_fwnode_device_properties props;
	int ret;

	ctrl_hdlr = &ds90ub940->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 9);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &ds90ub940->mutex;

	ds90ub940->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
						  V4L2_CID_PIXEL_RATE,
						  DS90UB940_MAX_PCLK,
						  DS90UB940_MAX_PCLK, 1,
						  DS90UB940_MAX_PCLK);

	ds90ub940->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &ds90ub940_ctrl_ops,
						      V4L2_CID_LINK_FREQ,
						      ARRAY_SIZE(link_freqs) - 1,
						      0, link_freqs);
	if (ds90ub940->link_freq)
		ds90ub940->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ds90ub940->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
					      V4L2_CID_VBLANK, 0, 0xffff, 1, 0);
	ds90ub940->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
					      V4L2_CID_HBLANK, 0, 0xffff, 1, 0);

	ds90ub940->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
					        V4L2_CID_EXPOSURE, 0, 1, 1, 0);

	v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  0, 1, 1, 0);

	ds90ub940->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
					     V4L2_CID_HFLIP, 0, 1, 1, 0);
	ds90ub940->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &ds90ub940_ctrl_ops,
					     V4L2_CID_VFLIP, 0, 1, 1, 0);

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &ds90ub940_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ds90ub940->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int ds90ub940_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ds90ub940 *ds90ub940;
	int ret;

	ds90ub940 = devm_kzalloc(&client->dev, sizeof(*ds90ub940), GFP_KERNEL);
	if (!ds90ub940)
		return -ENOMEM;

	mutex_init(&ds90ub940->mutex);

	v4l2_i2c_subdev_init(&ds90ub940->sd, client, &ds90ub940_subdev_ops);

	ds90ub940_set_default_format(ds90ub940);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = ds90ub940_init_controls(ds90ub940);
	if (ret)
		goto error_pm_runtime;

	ds90ub940->sd.internal_ops = &ds90ub940_internal_ops;
	ds90ub940->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	ds90ub940->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ds90ub940->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&ds90ub940->sd.entity, 1, &ds90ub940->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor_common(&ds90ub940->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	dev_info(dev, "DS90UB940 probe success\n");

	return 0;

error_media_entity:
	media_entity_cleanup(&ds90ub940->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(ds90ub940->sd.ctrl_handler);

error_pm_runtime:
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&ds90ub940->mutex);

	return ret;
}

static int ds90ub940_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ds90ub940 *ds90ub940 = to_ds90ub940(sd);

	mutex_destroy(&ds90ub940->mutex);
	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(ds90ub940->sd.ctrl_handler);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

static const struct of_device_id ds90ub940_dt_ids[] = {
	{ .compatible = "ti,ds90ub940" },
	{ }
};
MODULE_DEVICE_TABLE(of, ds90ub940_dt_ids);

static struct i2c_driver ds90ub940_i2c_driver = {
	.driver = {
		.name = "ds90ub940",
		.of_match_table	= ds90ub940_dt_ids,
	},
	.probe = ds90ub940_probe,
	.remove = ds90ub940_remove,
};

module_i2c_driver(ds90ub940_i2c_driver);

MODULE_AUTHOR("Grado Technologies <customers@gradotech.eu>");
MODULE_DESCRIPTION("DS90UB940 fake sensor driver");
MODULE_LICENSE("GPL v2");
