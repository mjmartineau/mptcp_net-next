// SPDX-License-Identifier: GPL-2.0
/* Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */
#include <linux/netlink.h>
#include <net/devlink.h>
#include <linux/firmware.h>

#include "hinic_dev.h"
#include "hinic_port.h"
#include "hinic_devlink.h"

static bool check_image_valid(struct hinic_devlink_priv *priv, const u8 *buf,
			      u32 image_size, struct host_image_st *host_image)
{
	struct fw_image_st *fw_image = NULL;
	u32 len = 0;
	u32 i;

	fw_image = (struct fw_image_st *)buf;

	if (fw_image->fw_magic != HINIC_MAGIC_NUM) {
		dev_err(&priv->hwdev->hwif->pdev->dev, "Wrong fw_magic read from file, fw_magic: 0x%x\n",
			fw_image->fw_magic);
		return false;
	}

	if (fw_image->fw_info.fw_section_cnt > MAX_FW_TYPE_NUM) {
		dev_err(&priv->hwdev->hwif->pdev->dev, "Wrong fw_type_num read from file, fw_type_num: 0x%x\n",
			fw_image->fw_info.fw_section_cnt);
		return false;
	}

	for (i = 0; i < fw_image->fw_info.fw_section_cnt; i++) {
		len += fw_image->fw_section_info[i].fw_section_len;
		memcpy(&host_image->image_section_info[i],
		       &fw_image->fw_section_info[i],
		       sizeof(struct fw_section_info_st));
	}

	if (len != fw_image->fw_len ||
	    (fw_image->fw_len + UPDATEFW_IMAGE_HEAD_SIZE) != image_size) {
		dev_err(&priv->hwdev->hwif->pdev->dev, "Wrong data size read from file\n");
		return false;
	}

	host_image->image_info.up_total_len = fw_image->fw_len;
	host_image->image_info.fw_version = fw_image->fw_version;
	host_image->section_type_num = fw_image->fw_info.fw_section_cnt;
	host_image->device_id = fw_image->device_id;

	return true;
}

static bool check_image_integrity(struct hinic_devlink_priv *priv,
				  struct host_image_st *host_image,
				  u32 update_type)
{
	u32 collect_section_type = 0;
	u32 i, type;

	for (i = 0; i < host_image->section_type_num; i++) {
		type = host_image->image_section_info[i].fw_section_type;
		if (collect_section_type & (1U << type)) {
			dev_err(&priv->hwdev->hwif->pdev->dev, "Duplicate section type: %u\n",
				type);
			return false;
		}
		collect_section_type |= (1U << type);
	}

	if (update_type == FW_UPDATE_COLD &&
	    (((collect_section_type & _IMAGE_COLD_SUB_MODULES_MUST_IN) ==
	       _IMAGE_COLD_SUB_MODULES_MUST_IN) ||
	      collect_section_type == _IMAGE_CFG_SUB_MODULES_MUST_IN))
		return true;

	if (update_type == FW_UPDATE_HOT &&
	    (collect_section_type & _IMAGE_HOT_SUB_MODULES_MUST_IN) ==
	    _IMAGE_HOT_SUB_MODULES_MUST_IN)
		return true;

	if (update_type == FW_UPDATE_COLD)
		dev_err(&priv->hwdev->hwif->pdev->dev, "Check file integrity failed, valid: 0x%x or 0x%lx, current: 0x%x\n",
			_IMAGE_COLD_SUB_MODULES_MUST_IN,
			_IMAGE_CFG_SUB_MODULES_MUST_IN, collect_section_type);
	else
		dev_err(&priv->hwdev->hwif->pdev->dev, "Check file integrity failed, valid:0x%x, current: 0x%x\n",
			_IMAGE_HOT_SUB_MODULES_MUST_IN, collect_section_type);

	return false;
}

static int check_image_device_type(struct hinic_devlink_priv *priv,
				   u32 image_device_type)
{
	struct hinic_comm_board_info board_info = {0};

	if (hinic_get_board_info(priv->hwdev, &board_info)) {
		dev_err(&priv->hwdev->hwif->pdev->dev, "Get board info failed\n");
		return false;
	}

	if (image_device_type == board_info.info.board_type)
		return true;

	dev_err(&priv->hwdev->hwif->pdev->dev, "The device type of upgrade file doesn't match the device type of current firmware, please check the upgrade file\n");
	dev_err(&priv->hwdev->hwif->pdev->dev, "The image device type: 0x%x, firmware device type: 0x%x\n",
		image_device_type, board_info.info.board_type);

	return false;
}

static int hinic_flash_fw(struct hinic_devlink_priv *priv, const u8 *data,
			  struct host_image_st *host_image)
{
	u32 section_remain_send_len, send_fragment_len, send_pos, up_total_len;
	struct hinic_cmd_update_fw *fw_update_msg = NULL;
	u32 section_type, section_crc, section_version;
	u32 i, len, section_len, section_offset;
	u16 out_size = sizeof(*fw_update_msg);
	int total_len_flag = 0;
	int err;

	fw_update_msg = kzalloc(sizeof(*fw_update_msg), GFP_KERNEL);
	if (!fw_update_msg)
		return -ENOMEM;

	up_total_len = host_image->image_info.up_total_len;

	for (i = 0; i < host_image->section_type_num; i++) {
		len = host_image->image_section_info[i].fw_section_len;
		if (host_image->image_section_info[i].fw_section_type ==
		    UP_FW_UPDATE_BOOT) {
			up_total_len = up_total_len - len;
			break;
		}
	}

	for (i = 0; i < host_image->section_type_num; i++) {
		section_len =
			host_image->image_section_info[i].fw_section_len;
		section_offset =
			host_image->image_section_info[i].fw_section_offset;
		section_remain_send_len = section_len;
		section_type =
			host_image->image_section_info[i].fw_section_type;
		section_crc = host_image->image_section_info[i].fw_section_crc;
		section_version =
			host_image->image_section_info[i].fw_section_version;

		if (section_type == UP_FW_UPDATE_BOOT)
			continue;

		send_fragment_len = 0;
		send_pos = 0;

		while (section_remain_send_len > 0) {
			if (!total_len_flag) {
				fw_update_msg->total_len = up_total_len;
				total_len_flag = 1;
			} else {
				fw_update_msg->total_len = 0;
			}

			memset(fw_update_msg->data, 0, MAX_FW_FRAGMENT_LEN);

			fw_update_msg->ctl_info.SF =
				(section_remain_send_len == section_len) ?
				true : false;
			fw_update_msg->section_info.FW_section_CRC = section_crc;
			fw_update_msg->fw_section_version = section_version;
			fw_update_msg->ctl_info.flag = UP_TYPE_A;

			if (section_type <= UP_FW_UPDATE_UP_DATA_B) {
				fw_update_msg->section_info.FW_section_type =
					(section_type % 2) ?
					UP_FW_UPDATE_UP_DATA :
					UP_FW_UPDATE_UP_TEXT;

				fw_update_msg->ctl_info.flag = UP_TYPE_B;
				if (section_type <= UP_FW_UPDATE_UP_DATA_A)
					fw_update_msg->ctl_info.flag = UP_TYPE_A;
			} else {
				fw_update_msg->section_info.FW_section_type =
					section_type - 0x2;
			}

			fw_update_msg->setion_total_len = section_len;
			fw_update_msg->section_offset = send_pos;

			if (section_remain_send_len <= MAX_FW_FRAGMENT_LEN) {
				fw_update_msg->ctl_info.SL = true;
				fw_update_msg->ctl_info.fragment_len =
					section_remain_send_len;
				send_fragment_len += section_remain_send_len;
			} else {
				fw_update_msg->ctl_info.SL = false;
				fw_update_msg->ctl_info.fragment_len =
					MAX_FW_FRAGMENT_LEN;
				send_fragment_len += MAX_FW_FRAGMENT_LEN;
			}

			memcpy(fw_update_msg->data,
			       data + UPDATEFW_IMAGE_HEAD_SIZE +
			       section_offset + send_pos,
			       fw_update_msg->ctl_info.fragment_len);

			err = hinic_port_msg_cmd(priv->hwdev,
						 HINIC_PORT_CMD_UPDATE_FW,
						 fw_update_msg,
						 sizeof(*fw_update_msg),
						 fw_update_msg, &out_size);
			if (err || !out_size || fw_update_msg->status) {
				dev_err(&priv->hwdev->hwif->pdev->dev, "Failed to update firmware, err: %d, status: 0x%x, out size: 0x%x\n",
					err, fw_update_msg->status, out_size);
				err = fw_update_msg->status ?
					fw_update_msg->status : -EIO;
				kfree(fw_update_msg);
				return err;
			}

			send_pos = send_fragment_len;
			section_remain_send_len = section_len -
						  send_fragment_len;
		}
	}

	kfree(fw_update_msg);

	return 0;
}

static int hinic_firmware_update(struct hinic_devlink_priv *priv,
				 const struct firmware *fw,
				 struct netlink_ext_ack *extack)
{
	struct host_image_st host_image;
	int err;

	memset(&host_image, 0, sizeof(struct host_image_st));

	if (!check_image_valid(priv, fw->data, fw->size, &host_image) ||
	    !check_image_integrity(priv, &host_image, FW_UPDATE_COLD) ||
	    !check_image_device_type(priv, host_image.device_id)) {
		NL_SET_ERR_MSG_MOD(extack, "Check image failed");
		return -EINVAL;
	}

	dev_info(&priv->hwdev->hwif->pdev->dev, "Flash firmware begin\n");

	err = hinic_flash_fw(priv, fw->data, &host_image);
	if (err) {
		if (err == HINIC_FW_DISMATCH_ERROR) {
			dev_err(&priv->hwdev->hwif->pdev->dev, "Firmware image doesn't match this card, please use newer image, err: %d\n",
				err);
			NL_SET_ERR_MSG_MOD(extack,
					   "Firmware image doesn't match this card, please use newer image");
		} else {
			dev_err(&priv->hwdev->hwif->pdev->dev, "Send firmware image data failed, err: %d\n",
				err);
			NL_SET_ERR_MSG_MOD(extack, "Send firmware image data failed");
		}

		return err;
	}

	dev_info(&priv->hwdev->hwif->pdev->dev, "Flash firmware end\n");

	return 0;
}

static int hinic_devlink_flash_update(struct devlink *devlink,
				      const char *file_name,
				      const char *component,
				      struct netlink_ext_ack *extack)
{
	struct hinic_devlink_priv *priv = devlink_priv(devlink);
	const struct firmware *fw;
	int err;

	if (component)
		return -EOPNOTSUPP;

	err = request_firmware_direct(&fw, file_name,
				      &priv->hwdev->hwif->pdev->dev);
	if (err)
		return err;

	err = hinic_firmware_update(priv, fw, extack);
	release_firmware(fw);

	return err;
}

static const struct devlink_ops hinic_devlink_ops = {
	.flash_update = hinic_devlink_flash_update,
};

struct devlink *hinic_devlink_alloc(void)
{
	return devlink_alloc(&hinic_devlink_ops, sizeof(struct hinic_dev));
}

void hinic_devlink_free(struct devlink *devlink)
{
	devlink_free(devlink);
}

int hinic_devlink_register(struct devlink *devlink, struct device *dev)
{
	return devlink_register(devlink, dev);
}

void hinic_devlink_unregister(struct devlink *devlink)
{
	devlink_unregister(devlink);
}
