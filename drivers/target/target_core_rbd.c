/*******************************************************************************
 * Filename:  target_core_rbd.c
 *
 * This file contains the Storage Engine  <-> Ceph RBD transport
 * specific functions.
 *
 * [Was based off of target_core_iblock.c from
 *  Nicholas A. Bellinger <nab@kernel.org>]
 *
 * (c) Copyright 2003-2013 Datera, Inc.
 * (c) Copyright 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ******************************************************************************/

#include <linux/string.h>
#include <linux/parser.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/genhd.h>
#include <linux/module.h>
#include <asm/unaligned.h>

#include <scsi/scsi.h>

#include <linux/ceph/libceph.h>
#include <linux/ceph/osd_client.h>
#include <linux/ceph/mon_client.h>
#include <linux/ceph/librbd.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_backend_configfs.h>
#include <target/target_core_fabric.h>

#include "target_core_rbd.h"

static inline struct tcm_rbd_dev *TCM_RBD_DEV(struct se_device *dev)
{
	return container_of(dev, struct tcm_rbd_dev, dev);
}

static int tcm_rbd_attach_hba(struct se_hba *hba, u32 host_id)
{
	pr_debug("CORE_HBA[%d] - TCM RBD HBA Driver %s on"
		" Generic Target Core Stack %s\n", hba->hba_id,
		TCM_RBD_VERSION, TARGET_CORE_VERSION);
	return 0;
}

static void tcm_rbd_detach_hba(struct se_hba *hba)
{
}

static struct se_device *tcm_rbd_alloc_device(struct se_hba *hba,
					      const char *name)
{
	struct tcm_rbd_dev *tcm_rbd_dev;

	tcm_rbd_dev = kzalloc(sizeof(struct tcm_rbd_dev), GFP_KERNEL);
	if (!tcm_rbd_dev) {
		pr_err("Unable to allocate struct tcm_rbd_dev\n");
		return NULL;
	}

	pr_debug( "TCM RBD: Allocated tcm_rbd_dev for %s\n", name);

	return &tcm_rbd_dev->dev;
}

static int tcm_rbd_configure_device(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct request_queue *q;
	struct block_device *bd = NULL;
	fmode_t mode;

	if (!(tcm_rbd_dev->bd_flags & TCM_RBD_HAS_UDEV_PATH)) {
		pr_err("Missing udev_path= parameters for TCM RBD\n");
		return -EINVAL;
	}

	pr_debug( "TCM RBD: Claiming struct block_device: %s\n",
		 tcm_rbd_dev->bd_udev_path);

	mode = FMODE_READ|FMODE_EXCL;
	if (!tcm_rbd_dev->bd_readonly)
		mode |= FMODE_WRITE;

	bd = blkdev_get_by_path(tcm_rbd_dev->bd_udev_path, mode, tcm_rbd_dev);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	tcm_rbd_dev->bd = bd;

	q = bdev_get_queue(bd);
	tcm_rbd_dev->rbd_dev = q->queuedata;

	dev->dev_attrib.hw_block_size = bdev_logical_block_size(bd);
	dev->dev_attrib.hw_max_sectors = queue_max_hw_sectors(q);
	dev->dev_attrib.hw_queue_depth = q->nr_requests;

	/*
	 * Check if the underlying struct block_device request_queue supports
	 * the QUEUE_FLAG_DISCARD bit for UNMAP/WRITE_SAME in SCSI + TRIM
	 * in ATA and we need to set TPE=1
	 */
	if (blk_queue_discard(q)) {
		dev->dev_attrib.max_unmap_lba_count =
				q->limits.max_discard_sectors;

		/*
		 * Currently hardcoded to 1 in Linux/SCSI code..
		 */
		dev->dev_attrib.max_unmap_block_desc_count = 1;
		dev->dev_attrib.unmap_granularity =
				q->limits.discard_granularity >> 9;
		dev->dev_attrib.unmap_granularity_alignment =
				q->limits.discard_alignment;

		pr_debug("TCM RBD: BLOCK Discard support available disabled by default\n");
	}
	/*
	 * Enable write same emulation for RBD and use 0xFFFF as
	 * the smaller WRITE_SAME(10) only has a two-byte block count.
	 */
	dev->dev_attrib.max_write_same_len = 0xFFFF;
	dev->dev_attrib.is_nonrot = 1;
	return 0;
}

static void tcm_rbd_free_device(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);

	if (tcm_rbd_dev->bd != NULL)
		blkdev_put(tcm_rbd_dev->bd, FMODE_WRITE|FMODE_READ|FMODE_EXCL);

	kfree(tcm_rbd_dev);
}

static sector_t tcm_rbd_get_blocks(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	sector_t blocks_long = tcm_rbd_dev->rbd_dev->mapping.size >>
								SECTOR_SHIFT;

	if (SECTOR_SIZE == dev->dev_attrib.block_size)
		return blocks_long;

	switch (SECTOR_SIZE) {
	case 4096:
		switch (dev->dev_attrib.block_size) {
		case 2048:
			blocks_long <<= 1;
			break;
		case 1024:
			blocks_long <<= 2;
			break;
		case 512:
			blocks_long <<= 3;
		default:
			break;
		}
		break;
	case 2048:
		switch (dev->dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 1;
			break;
		case 1024:
			blocks_long <<= 1;
			break;
		case 512:
			blocks_long <<= 2;
			break;
		default:
			break;
		}
		break;
	case 1024:
		switch (dev->dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 2;
			break;
		case 2048:
			blocks_long >>= 1;
			break;
		case 512:
			blocks_long <<= 1;
			break;
		default:
			break;
		}
		break;
	case 512:
		switch (dev->dev_attrib.block_size) {
		case 4096:
			blocks_long >>= 3;
			break;
		case 2048:
			blocks_long >>= 2;
			break;
		case 1024:
			blocks_long >>= 1;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return blocks_long;
}

static void rbd_complete_cmd(struct se_cmd *cmd)
{
	struct rbd_img_request *img_request = cmd->priv;
	u8 status = SAM_STAT_GOOD;

	if (img_request && img_request->result)
		status = SAM_STAT_CHECK_CONDITION;
	else
		status = SAM_STAT_GOOD;

	target_complete_cmd(cmd, status);
	if (img_request)
		rbd_img_request_put(img_request);
}

static sense_reason_t tcm_rbd_execute_sync_cache(struct se_cmd *cmd)
{
	/* Ceph/Rados supports flush, but kRBD does not yet */
	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}

/*
 * Convert the blocksize advertised to the initiator to the RBD offset.
 */
static u64 rbd_lba_shift(struct se_device *dev, unsigned long long task_lba)
{
	sector_t block_lba;

	/* convert to linux block which uses 512 byte sectors */
	if (dev->dev_attrib.block_size == 4096)
		block_lba = task_lba << 3;
	else if (dev->dev_attrib.block_size == 2048)
		block_lba = task_lba << 2;
	else if (dev->dev_attrib.block_size == 1024)
		block_lba = task_lba << 1;
	else
		block_lba = task_lba;

	/* convert to RBD offset */
	return block_lba << SECTOR_SHIFT;
}

static void tcm_rbd_async_callback(struct rbd_img_request *img_request)
{
	rbd_complete_cmd(img_request->lio_cmd_data);
}

static void tcm_rbd_sync_callback(struct rbd_img_request *img_request)
{
	struct completion *waiting = img_request->lio_cmd_data;

	complete(waiting);
}

static sense_reason_t
tcm_rbd_execute_cmd(struct se_cmd *cmd, struct rbd_device *rbd_dev,
		    struct scatterlist *sgl, enum obj_operation_type op_type,
		    u64 offset, u64 length, bool sync)
{
	struct rbd_img_request *img_request;
	struct ceph_snap_context *snapc = NULL;
	DECLARE_COMPLETION_ONSTACK(wait);
	sense_reason_t sense = TCM_NO_SENSE;
	int ret;

	if (op_type == OBJ_OP_WRITE || op_type == OBJ_OP_WRITESAME) {
		down_read(&rbd_dev->header_rwsem);
		snapc = rbd_dev->header.snapc;
		ceph_get_snap_context(snapc);
		up_read(&rbd_dev->header_rwsem);
	}

	img_request = rbd_img_request_create(rbd_dev, offset, length,
					     op_type, snapc);
	if (!img_request) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_snapc;
	}

	ret = rbd_img_request_fill(img_request,
				   sgl ? OBJ_REQUEST_SG : OBJ_REQUEST_NODATA,
				   sgl);
	if (ret) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_req;
	}

	if (sync) {
		img_request->lio_cmd_data = &wait;
		img_request->callback = tcm_rbd_sync_callback;
	} else {
		img_request->lio_cmd_data = cmd;
		img_request->callback = tcm_rbd_async_callback;
	}
	cmd->priv = img_request;

	ret = rbd_img_request_submit(img_request);
	if (ret == -ENOMEM) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_req;
	} else if (ret) {
		sense = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto free_req;
	}

	if (sync) {
		wait_for_completion(&wait);
		if (img_request->result)
			sense = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		rbd_img_request_put(img_request);
	}

	return sense;

free_req:
	rbd_img_request_put(img_request);
free_snapc:
	ceph_put_snap_context(snapc);
	return sense;
}

static sense_reason_t tcm_rbd_do_unmap(struct se_cmd *cmd, void *priv,
					    sector_t lba, sector_t nolb)
{
	struct tcm_rbd_dev *tcm_rbd_dev = priv;
	struct rbd_device *rbd_dev = tcm_rbd_dev->rbd_dev;

	return tcm_rbd_execute_cmd(cmd, rbd_dev, NULL, OBJ_OP_DISCARD,
				   lba << SECTOR_SHIFT, nolb << SECTOR_SHIFT,
				   true);
}

static sense_reason_t tcm_rbd_execute_unmap(struct se_cmd *cmd)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(cmd->se_dev);

	return sbc_execute_unmap(cmd, tcm_rbd_do_unmap, tcm_rbd_dev);
}

static sense_reason_t tcm_rbd_execute_write_same_unmap(struct se_cmd *cmd)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(cmd->se_dev);
	sector_t lba = cmd->t_task_lba;
	sector_t nolb = sbc_get_write_same_sectors(cmd);
	int ret;

	ret = tcm_rbd_do_unmap(cmd, tcm_rbd_dev, lba, nolb);
	if (ret)
		return ret;

	target_complete_cmd(cmd, GOOD);
	return 0;
}

static sense_reason_t tcm_rbd_execute_write_same(struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct rbd_device *rbd_dev = tcm_rbd_dev->rbd_dev;
	sector_t sectors = sbc_get_write_same_sectors(cmd);
	u64 length = rbd_lba_shift(dev, sectors);
	struct scatterlist *sg;

	if (cmd->prot_op) {
		pr_err("WRITE_SAME: Protection information with IBLOCK"
		       " backends not supported\n");
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
	sg = &cmd->t_data_sg[0];

	if (cmd->t_data_nents > 1 ||
	    sg->length != cmd->se_dev->dev_attrib.block_size) {
		pr_err("WRITE_SAME: Illegal SGL t_data_nents: %u length: %u"
			" block_size: %u\n", cmd->t_data_nents, sg->length,
			cmd->se_dev->dev_attrib.block_size);
		return TCM_INVALID_CDB_FIELD;
	}

	return tcm_rbd_execute_cmd(cmd, rbd_dev, sg, OBJ_OP_WRITESAME,
				   rbd_lba_shift(dev, cmd->t_task_lba), length,
				   false);
}

static void tcm_rbd_cmp_and_write_callback(struct rbd_img_request *img_request)
{
	struct se_cmd *cmd = img_request->lio_cmd_data;
	struct se_device *dev = cmd->se_dev;
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	sense_reason_t sense_reason = TCM_NO_SENSE;
	u64 miscompare_off;

	if (img_request->result == -EILSEQ) {
		ceph_copy_from_page_vector(tcm_rbd_dev->cmp_and_write_pages,
					   &miscompare_off, 0,
					   sizeof(miscompare_off));
		cmd->sense_info = (u32)le64_to_cpu(miscompare_off);
		pr_err("COMPARE_AND_WRITE: miscompare at offset %llu\n",
		       (unsigned long long)cmd->bad_sector);
		sense_reason = TCM_MISCOMPARE_VERIFY;
	}
	kfree(tcm_rbd_dev->cmp_and_write_sg);
	tcm_rbd_dev->cmp_and_write_sg = NULL;
	up(&dev->caw_sem);

	if (sense_reason != TCM_NO_SENSE) {
		/* TODO pass miscompare offset */
		target_complete_cmd_with_sense(cmd, sense_reason);
	} else if (img_request->result) {
		target_complete_cmd(cmd, SAM_STAT_CHECK_CONDITION);
	} else {
		target_complete_cmd(cmd, SAM_STAT_GOOD);
	}
	rbd_img_request_put(img_request);
}

static sense_reason_t tcm_rbd_execute_cmp_and_write(struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct rbd_device *rbd_dev = tcm_rbd_dev->rbd_dev;
	struct rbd_img_request *img_request;
	struct ceph_snap_context *snapc;
	sense_reason_t sense = TCM_NO_SENSE;
	unsigned int len = cmd->t_task_nolb * dev->dev_attrib.block_size;
	u64 response_len = len + sizeof(u64);
	struct page **pages;
	u32 page_count;
	int ret;

	down_read(&rbd_dev->header_rwsem);
	snapc = rbd_dev->header.snapc;
	ceph_get_snap_context(snapc);
	up_read(&rbd_dev->header_rwsem);

	img_request = rbd_img_request_create(rbd_dev,
					     rbd_lba_shift(dev, cmd->t_task_lba),
					     len, OBJ_OP_CMP_AND_WRITE, snapc);
	if (!img_request) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_snapc;
	}

	ret = down_interruptible(&dev->caw_sem);
	if (ret != 0 || signal_pending(current)) {
		sense = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto free_req;
	}

	tcm_rbd_dev->cmp_and_write_sg = sbc_create_compare_and_write_sg(cmd);
	if (!tcm_rbd_dev->cmp_and_write_sg)
		goto rel_caw_sem;

	page_count = (u32)calc_pages_for(0, len + sizeof(u64));
	pages = ceph_alloc_page_vector(page_count, GFP_KERNEL);
	if (IS_ERR(pages))
		goto free_write_sg;
	tcm_rbd_dev->cmp_and_write_pages = pages;

	ret = rbd_img_cmp_and_write_request_fill(img_request, cmd->t_data_sg,
						 len,
						 tcm_rbd_dev->cmp_and_write_sg,
						 len, pages, response_len);
	if (ret == -EOPNOTSUPP) {
		sense = TCM_INVALID_CDB_FIELD;
		goto free_pages;
	} else if (ret) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_pages;
	}

	cmd->priv = img_request;
	img_request->lio_cmd_data = cmd;
	img_request->callback = tcm_rbd_cmp_and_write_callback;

	ret = rbd_img_request_submit(img_request);

	if (ret == -ENOMEM) {
		sense = TCM_OUT_OF_RESOURCES;
		goto free_write_sg;
	} else if (ret) {
		sense = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto free_write_sg;
	}
	return 0;

free_pages:
	ceph_release_page_vector(pages, page_count);
free_write_sg:
	kfree(tcm_rbd_dev->cmp_and_write_sg);
	tcm_rbd_dev->cmp_and_write_sg = NULL;
rel_caw_sem:
	up(&dev->caw_sem);
free_req:
	rbd_img_request_put(img_request);
free_snapc:
	ceph_put_snap_context(snapc);
	return sense;
}

enum {
	Opt_udev_path, Opt_readonly, Opt_force, Opt_err
};

static match_table_t tokens = {
	{Opt_udev_path, "udev_path=%s"},
	{Opt_readonly, "readonly=%d"},
	{Opt_force, "force=%d"},
	{Opt_err, NULL}
};

static ssize_t
tcm_rbd_set_configfs_dev_params(struct se_device *dev, const char *page,
				ssize_t count)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	char *orig, *ptr, *arg_p, *opts;
	substring_t args[MAX_OPT_ARGS];
	int ret = 0, token;
	unsigned long tmp_readonly;

	opts = kstrdup(page, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	orig = opts;

	while ((ptr = strsep(&opts, ",\n")) != NULL) {
		if (!*ptr)
			continue;

		token = match_token(ptr, tokens, args);
		switch (token) {
		case Opt_udev_path:
			if (tcm_rbd_dev->bd) {
				pr_err("Unable to set udev_path= while"
					" tcm_rbd_dev->bd exists\n");
				ret = -EEXIST;
				goto out;
			}
			if (match_strlcpy(tcm_rbd_dev->bd_udev_path, &args[0],
				SE_UDEV_PATH_LEN) == 0) {
				ret = -EINVAL;
				break;
			}
			pr_debug("TCM RBD: Referencing UDEV path: %s\n",
				 tcm_rbd_dev->bd_udev_path);
			tcm_rbd_dev->bd_flags |= TCM_RBD_HAS_UDEV_PATH;
			break;
		case Opt_readonly:
			arg_p = match_strdup(&args[0]);
			if (!arg_p) {
				ret = -ENOMEM;
				break;
			}
			ret = kstrtoul(arg_p, 0, &tmp_readonly);
			kfree(arg_p);
			if (ret < 0) {
				pr_err("kstrtoul() failed for readonly=\n");
				goto out;
			}
			tcm_rbd_dev->bd_readonly = tmp_readonly;
			pr_debug("TCM RBD: readonly: %d\n",
				 tcm_rbd_dev->bd_readonly);
			break;
		case Opt_force:
			break;
		default:
			break;
		}
	}

out:
	kfree(orig);
	return (!ret) ? count : ret;
}

static ssize_t tcm_rbd_show_configfs_dev_params(struct se_device *dev, char *b)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct block_device *bd = tcm_rbd_dev->bd;
	char buf[BDEVNAME_SIZE];
	ssize_t bl = 0;

	if (bd)
		bl += sprintf(b + bl, "rbd device: %s", bdevname(bd, buf));
	if (tcm_rbd_dev->bd_flags & TCM_RBD_HAS_UDEV_PATH)
		bl += sprintf(b + bl, "  UDEV PATH: %s",
			      tcm_rbd_dev->bd_udev_path);
	bl += sprintf(b + bl, "  readonly: %d\n", tcm_rbd_dev->bd_readonly);

	bl += sprintf(b + bl, "        ");
	if (bd) {
		bl += sprintf(b + bl, "Major: %d Minor: %d  %s\n",
			      MAJOR(bd->bd_dev), MINOR(bd->bd_dev),
			      (!bd->bd_contains) ?
			      "" : (bd->bd_holder == tcm_rbd_dev) ?
			      "CLAIMED: RBD" : "CLAIMED: OS");
	} else {
		bl += sprintf(b + bl, "Major: 0 Minor: 0\n");
	}

	return bl;
}

static sense_reason_t
tcm_rbd_execute_rw(struct se_cmd *cmd, struct scatterlist *sgl, u32 sgl_nents,
		   enum dma_data_direction data_direction)
{
	struct se_device *dev = cmd->se_dev;
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct rbd_device *rbd_dev = tcm_rbd_dev->rbd_dev;
	enum obj_operation_type op_type;

	if (!sgl_nents) {
		rbd_complete_cmd(cmd);
		return 0;
	}

	if (data_direction == DMA_FROM_DEVICE) {
		op_type = OBJ_OP_READ;
	} else {
		op_type = OBJ_OP_WRITE;
	}

	return tcm_rbd_execute_cmd(cmd, rbd_dev, sgl, op_type,
				   rbd_lba_shift(dev, cmd->t_task_lba),
				   cmd->data_length, false);
}

static sector_t tcm_rbd_get_alignment_offset_lbas(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct block_device *bd = tcm_rbd_dev->bd;
	int ret;

	ret = bdev_alignment_offset(bd);
	if (ret == -1)
		return 0;

	/* convert offset-bytes to offset-lbas */
	return ret / bdev_logical_block_size(bd);
}

static unsigned int tcm_rbd_get_lbppbe(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct block_device *bd = tcm_rbd_dev->bd;
	int logs_per_phys = bdev_physical_block_size(bd) / bdev_logical_block_size(bd);

	return ilog2(logs_per_phys);
}

static unsigned int tcm_rbd_get_io_min(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct block_device *bd = tcm_rbd_dev->bd;

	return bdev_io_min(bd);
}

static unsigned int tcm_rbd_get_io_opt(struct se_device *dev)
{
	struct tcm_rbd_dev *tcm_rbd_dev = TCM_RBD_DEV(dev);
	struct block_device *bd = tcm_rbd_dev->bd;

	return bdev_io_opt(bd);
}

static struct sbc_ops tcm_rbd_sbc_ops = {
	.execute_rw		= tcm_rbd_execute_rw,
	.execute_sync_cache	= tcm_rbd_execute_sync_cache,
	.execute_write_same	= tcm_rbd_execute_write_same,
	.execute_write_same_unmap = tcm_rbd_execute_write_same_unmap,
	.execute_unmap		= tcm_rbd_execute_unmap,
	.execute_compare_and_write = tcm_rbd_execute_cmp_and_write,
};

static sense_reason_t tcm_rbd_parse_cdb(struct se_cmd *cmd)
{
	return sbc_parse_cdb(cmd, &tcm_rbd_sbc_ops);
}

static bool tcm_rbd_get_write_cache(struct se_device *dev)
{
	return false;
}

DEF_TB_DEFAULT_ATTRIBS(rbd);

static struct configfs_attribute *rbd_backend_dev_attrs[] = {
	&rbd_dev_attrib_emulate_model_alias.attr,
	&rbd_dev_attrib_emulate_dpo.attr,
	&rbd_dev_attrib_emulate_fua_write.attr,
	&rbd_dev_attrib_emulate_fua_read.attr,
	&rbd_dev_attrib_emulate_write_cache.attr,
	&rbd_dev_attrib_emulate_ua_intlck_ctrl.attr,
	&rbd_dev_attrib_emulate_tas.attr,
	&rbd_dev_attrib_emulate_tpu.attr,
	&rbd_dev_attrib_emulate_tpws.attr,
	&rbd_dev_attrib_emulate_caw.attr,
	&rbd_dev_attrib_emulate_3pc.attr,
	&rbd_dev_attrib_pi_prot_type.attr,
	&rbd_dev_attrib_hw_pi_prot_type.attr,
	&rbd_dev_attrib_pi_prot_format.attr,
	&rbd_dev_attrib_enforce_pr_isids.attr,
	&rbd_dev_attrib_is_nonrot.attr,
	&rbd_dev_attrib_emulate_rest_reord.attr,
	&rbd_dev_attrib_force_pr_aptpl.attr,
	&rbd_dev_attrib_hw_block_size.attr,
	&rbd_dev_attrib_block_size.attr,
	&rbd_dev_attrib_hw_max_sectors.attr,
	&rbd_dev_attrib_optimal_sectors.attr,
	&rbd_dev_attrib_hw_queue_depth.attr,
	&rbd_dev_attrib_queue_depth.attr,
	&rbd_dev_attrib_max_unmap_lba_count.attr,
	&rbd_dev_attrib_max_unmap_block_desc_count.attr,
	&rbd_dev_attrib_unmap_granularity.attr,
	&rbd_dev_attrib_unmap_granularity_alignment.attr,
	&rbd_dev_attrib_max_write_same_len.attr,
	NULL,
};

static struct se_subsystem_api tcm_rbd_ops = {
	.name				= "rbd",
	.inquiry_prod			= "RBD",
	.inquiry_rev			= TCM_RBD_VERSION,
	.owner				= THIS_MODULE,
	.attach_hba			= tcm_rbd_attach_hba,
	.detach_hba			= tcm_rbd_detach_hba,
	.alloc_device			= tcm_rbd_alloc_device,
	.configure_device		= tcm_rbd_configure_device,
	.free_device			= tcm_rbd_free_device,
	.parse_cdb			= tcm_rbd_parse_cdb,
	.set_configfs_dev_params	= tcm_rbd_set_configfs_dev_params,
	.show_configfs_dev_params	= tcm_rbd_show_configfs_dev_params,
	.get_device_type		= sbc_get_device_type,
	.get_blocks			= tcm_rbd_get_blocks,
	.get_alignment_offset_lbas	= tcm_rbd_get_alignment_offset_lbas,
	.get_lbppbe			= tcm_rbd_get_lbppbe,
	.get_io_min			= tcm_rbd_get_io_min,
	.get_io_opt			= tcm_rbd_get_io_opt,
	.get_write_cache		= tcm_rbd_get_write_cache,
};

static int __init tcm_rbd_module_init(void)
{
	struct target_backend_cits *tbc = &tcm_rbd_ops.tb_cits;

	target_core_setup_sub_cits(&tcm_rbd_ops);
	tbc->tb_dev_attrib_cit.ct_attrs = rbd_backend_dev_attrs;

	return transport_subsystem_register(&tcm_rbd_ops);
}

static void __exit tcm_rbd_module_exit(void)
{
	transport_subsystem_release(&tcm_rbd_ops);
}

MODULE_DESCRIPTION("TCM Ceph RBD subsystem plugin");
MODULE_AUTHOR("Mike Christie");
MODULE_LICENSE("GPL");

module_init(tcm_rbd_module_init);
module_exit(tcm_rbd_module_exit);
