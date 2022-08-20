// SPDX-License-Identifier: GPL-2.0+
/*
 * Media driver for Freescale i.MX5/6 SOC
 *
 * Open Firmware parsing.
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 */
#include <linux/of_platform.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/of_graph.h>
#include <video/imx-ipu-v3.h>
#include "imx-media.h"

int imx_media_of_add_csi(struct imx_media_dev *imxmd,
			 struct device_node *csi_np)
{
	struct v4l2_async_subdev *asd;
	int ret = 0;

	if (!of_device_is_available(csi_np)) {
		dev_dbg(imxmd->md.dev, "%s: %pOFn not enabled\n", __func__,
			csi_np);
		return -ENODEV;
	}

	/* add CSI fwnode to async notifier */
	asd = v4l2_async_notifier_add_fwnode_subdev(&imxmd->notifier,
						    of_fwnode_handle(csi_np),
						    sizeof(*asd));
	if (IS_ERR(asd)) {
		ret = PTR_ERR(asd);
		if (ret == -EEXIST)
			dev_dbg(imxmd->md.dev, "%s: already added %pOFn\n",
				__func__, csi_np);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_of_add_csi);

int imx_media_add_of_subdevs(struct imx_media_dev *imxmd,
			     struct device_node *np)
{
	struct device_node *csi_np;
	int i, ret;

	for (i = 0; ; i++) {
		csi_np = of_parse_phandle(np, "ports", i);
		if (!csi_np)
			break;

		ret = imx_media_of_add_csi(imxmd, csi_np);
		if (ret) {
			/* unavailable or already added is not an error */
			if (ret == -ENODEV || ret == -EEXIST) {
				of_node_put(csi_np);
				continue;
			}

			/* other error, can't continue */
			goto err_out;
		}
	}

	return 0;

err_out:
	of_node_put(csi_np);
	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_add_of_subdevs);

/*
 * Create a single media link to/from sd using a fwnode link.
 *
 * NOTE: this function assumes an OF port node is equivalent to
 * a media pad (port id equal to media pad index), and that an
 * OF endpoint node is equivalent to a media link.
 */
static int create_of_link(struct imx_media_dev *imxmd,
			  struct v4l2_subdev *sd,
			  struct v4l2_fwnode_link *link)
{
	struct v4l2_subdev *remote, *src, *sink;
	int src_pad, sink_pad;

	if (link->local_port >= sd->entity.num_pads)
		return -EINVAL;

	remote = imx_media_find_subdev_by_fwnode(imxmd, link->remote_node);
	if (!remote)
		return 0;

	if (sd->entity.pads[link->local_port].flags & MEDIA_PAD_FL_SINK) {
		src = remote;
		src_pad = link->remote_port;
		sink = sd;
		sink_pad = link->local_port;
	} else {
		src = sd;
		src_pad = link->local_port;
		sink = remote;
		sink_pad = link->remote_port;
	}

	/* make sure link doesn't already exist before creating */
	if (media_entity_find_link(&src->entity.pads[src_pad],
				   &sink->entity.pads[sink_pad]))
		return 0;

	v4l2_info(sd->v4l2_dev, "%s:%d -> %s:%d\n",
		  src->name, src_pad, sink->name, sink_pad);

	return media_create_pad_link(&src->entity, src_pad,
				     &sink->entity, sink_pad, 0);
}

/*
 * Create media links to/from sd using its device-tree endpoints.
 */
int imx_media_create_of_links(struct imx_media_dev *imxmd,
			      struct v4l2_subdev *sd)
{
	struct v4l2_fwnode_link link;
	struct device_node *ep;
	int ret;

	for_each_endpoint_of_node(sd->dev->of_node, ep) {
		ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
		if (ret)
			continue;

		ret = create_of_link(imxmd, sd, &link);
		v4l2_fwnode_put_link(&link);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_create_of_links);

/*
 * Create media links to the given CSI subdevice's sink pads,
 * using its device-tree endpoints.
 */
int imx_media_create_csi_of_links(struct imx_media_dev *imxmd,
				  struct v4l2_subdev *csi)
{
	struct device_node *csi_np = csi->dev->of_node;
	struct device_node *ep;

	for_each_child_of_node(csi_np, ep) {
		struct fwnode_handle *fwnode, *csi_ep;
		struct v4l2_fwnode_link link;
		int ret;

		memset(&link, 0, sizeof(link));

		link.local_node = of_fwnode_handle(csi_np);
		link.local_port = CSI_SINK_PAD;

		csi_ep = of_fwnode_handle(ep);

		fwnode = fwnode_graph_get_remote_endpoint(csi_ep);
		if (!fwnode)
			continue;

		fwnode = fwnode_get_parent(fwnode);
		fwnode_property_read_u32(fwnode, "reg", &link.remote_port);
		fwnode = fwnode_get_next_parent(fwnode);
		if (is_of_node(fwnode) &&
		    of_node_name_eq(to_of_node(fwnode), "ports"))
			fwnode = fwnode_get_next_parent(fwnode);
		link.remote_node = fwnode;

		ret = create_of_link(imxmd, csi, &link);
		fwnode_handle_put(link.remote_node);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_create_csi_of_links);
