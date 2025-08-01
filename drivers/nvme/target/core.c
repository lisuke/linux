// SPDX-License-Identifier: GPL-2.0
/*
 * Common code for the NVMe target.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/pci-p2pdma.h>
#include <linux/scatterlist.h>

#include <generated/utsrelease.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

#include "nvmet.h"
#include "debugfs.h"

struct kmem_cache *nvmet_bvec_cache;
struct workqueue_struct *buffered_io_wq;
struct workqueue_struct *zbd_wq;
static const struct nvmet_fabrics_ops *nvmet_transports[NVMF_TRTYPE_MAX];
static DEFINE_IDA(cntlid_ida);

struct workqueue_struct *nvmet_wq;
EXPORT_SYMBOL_GPL(nvmet_wq);

/*
 * This read/write semaphore is used to synchronize access to configuration
 * information on a target system that will result in discovery log page
 * information change for at least one host.
 * The full list of resources to protected by this semaphore is:
 *
 *  - subsystems list
 *  - per-subsystem allowed hosts list
 *  - allow_any_host subsystem attribute
 *  - nvmet_genctr
 *  - the nvmet_transports array
 *
 * When updating any of those lists/structures write lock should be obtained,
 * while when reading (popolating discovery log page or checking host-subsystem
 * link) read lock is obtained to allow concurrent reads.
 */
DECLARE_RWSEM(nvmet_config_sem);

u32 nvmet_ana_group_enabled[NVMET_MAX_ANAGRPS + 1];
u64 nvmet_ana_chgcnt;
DECLARE_RWSEM(nvmet_ana_sem);

inline u16 errno_to_nvme_status(struct nvmet_req *req, int errno)
{
	switch (errno) {
	case 0:
		return NVME_SC_SUCCESS;
	case -ENOSPC:
		req->error_loc = offsetof(struct nvme_rw_command, length);
		return NVME_SC_CAP_EXCEEDED | NVME_STATUS_DNR;
	case -EREMOTEIO:
		req->error_loc = offsetof(struct nvme_rw_command, slba);
		return  NVME_SC_LBA_RANGE | NVME_STATUS_DNR;
	case -EOPNOTSUPP:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
	case -ENODATA:
		req->error_loc = offsetof(struct nvme_rw_command, nsid);
		return NVME_SC_ACCESS_DENIED;
	case -EIO:
		fallthrough;
	default:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	}
}

u16 nvmet_report_invalid_opcode(struct nvmet_req *req)
{
	pr_debug("unhandled cmd %d on qid %d\n", req->cmd->common.opcode,
		 req->sq->qid);

	req->error_loc = offsetof(struct nvme_common_command, opcode);
	return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
}

static struct nvmet_subsys *nvmet_find_get_subsys(struct nvmet_port *port,
		const char *subsysnqn);

u16 nvmet_copy_to_sgl(struct nvmet_req *req, off_t off, const void *buf,
		size_t len)
{
	if (sg_pcopy_from_buffer(req->sg, req->sg_cnt, buf, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_STATUS_DNR;
	}
	return 0;
}

u16 nvmet_copy_from_sgl(struct nvmet_req *req, off_t off, void *buf, size_t len)
{
	if (sg_pcopy_to_buffer(req->sg, req->sg_cnt, buf, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_STATUS_DNR;
	}
	return 0;
}

u16 nvmet_zero_sgl(struct nvmet_req *req, off_t off, size_t len)
{
	if (sg_zero_buffer(req->sg, req->sg_cnt, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_STATUS_DNR;
	}
	return 0;
}

static u32 nvmet_max_nsid(struct nvmet_subsys *subsys)
{
	struct nvmet_ns *cur;
	unsigned long idx;
	u32 nsid = 0;

	nvmet_for_each_enabled_ns(&subsys->namespaces, idx, cur)
		nsid = cur->nsid;

	return nsid;
}

static u32 nvmet_async_event_result(struct nvmet_async_event *aen)
{
	return aen->event_type | (aen->event_info << 8) | (aen->log_page << 16);
}

static void nvmet_async_events_failall(struct nvmet_ctrl *ctrl)
{
	struct nvmet_req *req;

	mutex_lock(&ctrl->lock);
	while (ctrl->nr_async_event_cmds) {
		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		mutex_unlock(&ctrl->lock);
		nvmet_req_complete(req, NVME_SC_INTERNAL | NVME_STATUS_DNR);
		mutex_lock(&ctrl->lock);
	}
	mutex_unlock(&ctrl->lock);
}

static void nvmet_async_events_process(struct nvmet_ctrl *ctrl)
{
	struct nvmet_async_event *aen;
	struct nvmet_req *req;

	mutex_lock(&ctrl->lock);
	while (ctrl->nr_async_event_cmds && !list_empty(&ctrl->async_events)) {
		aen = list_first_entry(&ctrl->async_events,
				       struct nvmet_async_event, entry);
		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		nvmet_set_result(req, nvmet_async_event_result(aen));

		list_del(&aen->entry);
		kfree(aen);

		mutex_unlock(&ctrl->lock);
		trace_nvmet_async_event(ctrl, req->cqe->result.u32);
		nvmet_req_complete(req, 0);
		mutex_lock(&ctrl->lock);
	}
	mutex_unlock(&ctrl->lock);
}

static void nvmet_async_events_free(struct nvmet_ctrl *ctrl)
{
	struct nvmet_async_event *aen, *tmp;

	mutex_lock(&ctrl->lock);
	list_for_each_entry_safe(aen, tmp, &ctrl->async_events, entry) {
		list_del(&aen->entry);
		kfree(aen);
	}
	mutex_unlock(&ctrl->lock);
}

static void nvmet_async_event_work(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl =
		container_of(work, struct nvmet_ctrl, async_event_work);

	nvmet_async_events_process(ctrl);
}

void nvmet_add_async_event(struct nvmet_ctrl *ctrl, u8 event_type,
		u8 event_info, u8 log_page)
{
	struct nvmet_async_event *aen;

	aen = kmalloc(sizeof(*aen), GFP_KERNEL);
	if (!aen)
		return;

	aen->event_type = event_type;
	aen->event_info = event_info;
	aen->log_page = log_page;

	mutex_lock(&ctrl->lock);
	list_add_tail(&aen->entry, &ctrl->async_events);
	mutex_unlock(&ctrl->lock);

	queue_work(nvmet_wq, &ctrl->async_event_work);
}

static void nvmet_add_to_changed_ns_log(struct nvmet_ctrl *ctrl, __le32 nsid)
{
	u32 i;

	mutex_lock(&ctrl->lock);
	if (ctrl->nr_changed_ns > NVME_MAX_CHANGED_NAMESPACES)
		goto out_unlock;

	for (i = 0; i < ctrl->nr_changed_ns; i++) {
		if (ctrl->changed_ns_list[i] == nsid)
			goto out_unlock;
	}

	if (ctrl->nr_changed_ns == NVME_MAX_CHANGED_NAMESPACES) {
		ctrl->changed_ns_list[0] = cpu_to_le32(0xffffffff);
		ctrl->nr_changed_ns = U32_MAX;
		goto out_unlock;
	}

	ctrl->changed_ns_list[ctrl->nr_changed_ns++] = nsid;
out_unlock:
	mutex_unlock(&ctrl->lock);
}

void nvmet_ns_changed(struct nvmet_subsys *subsys, u32 nsid)
{
	struct nvmet_ctrl *ctrl;

	lockdep_assert_held(&subsys->lock);

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		nvmet_add_to_changed_ns_log(ctrl, cpu_to_le32(nsid));
		if (nvmet_aen_bit_disabled(ctrl, NVME_AEN_BIT_NS_ATTR))
			continue;
		nvmet_add_async_event(ctrl, NVME_AER_NOTICE,
				NVME_AER_NOTICE_NS_CHANGED,
				NVME_LOG_CHANGED_NS);
	}
}

void nvmet_send_ana_event(struct nvmet_subsys *subsys,
		struct nvmet_port *port)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (port && ctrl->port != port)
			continue;
		if (nvmet_aen_bit_disabled(ctrl, NVME_AEN_BIT_ANA_CHANGE))
			continue;
		nvmet_add_async_event(ctrl, NVME_AER_NOTICE,
				NVME_AER_NOTICE_ANA, NVME_LOG_ANA);
	}
	mutex_unlock(&subsys->lock);
}

void nvmet_port_send_ana_event(struct nvmet_port *port)
{
	struct nvmet_subsys_link *p;

	down_read(&nvmet_config_sem);
	list_for_each_entry(p, &port->subsystems, entry)
		nvmet_send_ana_event(p->subsys, port);
	up_read(&nvmet_config_sem);
}

int nvmet_register_transport(const struct nvmet_fabrics_ops *ops)
{
	int ret = 0;

	down_write(&nvmet_config_sem);
	if (nvmet_transports[ops->type])
		ret = -EINVAL;
	else
		nvmet_transports[ops->type] = ops;
	up_write(&nvmet_config_sem);

	return ret;
}
EXPORT_SYMBOL_GPL(nvmet_register_transport);

void nvmet_unregister_transport(const struct nvmet_fabrics_ops *ops)
{
	down_write(&nvmet_config_sem);
	nvmet_transports[ops->type] = NULL;
	up_write(&nvmet_config_sem);
}
EXPORT_SYMBOL_GPL(nvmet_unregister_transport);

void nvmet_port_del_ctrls(struct nvmet_port *port, struct nvmet_subsys *subsys)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (ctrl->port == port)
			ctrl->ops->delete_ctrl(ctrl);
	}
	mutex_unlock(&subsys->lock);
}

int nvmet_enable_port(struct nvmet_port *port)
{
	const struct nvmet_fabrics_ops *ops;
	int ret;

	lockdep_assert_held(&nvmet_config_sem);

	if (port->disc_addr.trtype == NVMF_TRTYPE_MAX)
		return -EINVAL;

	ops = nvmet_transports[port->disc_addr.trtype];
	if (!ops) {
		up_write(&nvmet_config_sem);
		request_module("nvmet-transport-%d", port->disc_addr.trtype);
		down_write(&nvmet_config_sem);
		ops = nvmet_transports[port->disc_addr.trtype];
		if (!ops) {
			pr_err("transport type %d not supported\n",
				port->disc_addr.trtype);
			return -EINVAL;
		}
	}

	if (!try_module_get(ops->owner))
		return -EINVAL;

	/*
	 * If the user requested PI support and the transport isn't pi capable,
	 * don't enable the port.
	 */
	if (port->pi_enable && !(ops->flags & NVMF_METADATA_SUPPORTED)) {
		pr_err("T10-PI is not supported by transport type %d\n",
		       port->disc_addr.trtype);
		ret = -EINVAL;
		goto out_put;
	}

	ret = ops->add_port(port);
	if (ret)
		goto out_put;

	/* If the transport didn't set inline_data_size, then disable it. */
	if (port->inline_data_size < 0)
		port->inline_data_size = 0;

	/*
	 * If the transport didn't set the max_queue_size properly, then clamp
	 * it to the target limits. Also set default values in case the
	 * transport didn't set it at all.
	 */
	if (port->max_queue_size < 0)
		port->max_queue_size = NVMET_MAX_QUEUE_SIZE;
	else
		port->max_queue_size = clamp_t(int, port->max_queue_size,
					       NVMET_MIN_QUEUE_SIZE,
					       NVMET_MAX_QUEUE_SIZE);

	port->enabled = true;
	port->tr_ops = ops;
	return 0;

out_put:
	module_put(ops->owner);
	return ret;
}

void nvmet_disable_port(struct nvmet_port *port)
{
	const struct nvmet_fabrics_ops *ops;

	lockdep_assert_held(&nvmet_config_sem);

	port->enabled = false;
	port->tr_ops = NULL;

	ops = nvmet_transports[port->disc_addr.trtype];
	ops->remove_port(port);
	module_put(ops->owner);
}

static void nvmet_keep_alive_timer(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvmet_ctrl, ka_work);
	bool reset_tbkas = ctrl->reset_tbkas;

	ctrl->reset_tbkas = false;
	if (reset_tbkas) {
		pr_debug("ctrl %d reschedule traffic based keep-alive timer\n",
			ctrl->cntlid);
		queue_delayed_work(nvmet_wq, &ctrl->ka_work, ctrl->kato * HZ);
		return;
	}

	pr_err("ctrl %d keep-alive timer (%d seconds) expired!\n",
		ctrl->cntlid, ctrl->kato);

	nvmet_ctrl_fatal_error(ctrl);
}

void nvmet_start_keep_alive_timer(struct nvmet_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	pr_debug("ctrl %d start keep-alive timer for %d secs\n",
		ctrl->cntlid, ctrl->kato);

	queue_delayed_work(nvmet_wq, &ctrl->ka_work, ctrl->kato * HZ);
}

void nvmet_stop_keep_alive_timer(struct nvmet_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	pr_debug("ctrl %d stop keep-alive\n", ctrl->cntlid);

	cancel_delayed_work_sync(&ctrl->ka_work);
}

u16 nvmet_req_find_ns(struct nvmet_req *req)
{
	u32 nsid = le32_to_cpu(req->cmd->common.nsid);
	struct nvmet_subsys *subsys = nvmet_req_subsys(req);

	req->ns = xa_load(&subsys->namespaces, nsid);
	if (unlikely(!req->ns || !req->ns->enabled)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		if (!req->ns) /* ns doesn't exist! */
			return NVME_SC_INVALID_NS | NVME_STATUS_DNR;

		/* ns exists but it's disabled */
		req->ns = NULL;
		return NVME_SC_INTERNAL_PATH_ERROR;
	}

	percpu_ref_get(&req->ns->ref);
	return NVME_SC_SUCCESS;
}

static void nvmet_destroy_namespace(struct percpu_ref *ref)
{
	struct nvmet_ns *ns = container_of(ref, struct nvmet_ns, ref);

	complete(&ns->disable_done);
}

void nvmet_put_namespace(struct nvmet_ns *ns)
{
	percpu_ref_put(&ns->ref);
}

static void nvmet_ns_dev_disable(struct nvmet_ns *ns)
{
	nvmet_bdev_ns_disable(ns);
	nvmet_file_ns_disable(ns);
}

static int nvmet_p2pmem_ns_enable(struct nvmet_ns *ns)
{
	int ret;
	struct pci_dev *p2p_dev;

	if (!ns->use_p2pmem)
		return 0;

	if (!ns->bdev) {
		pr_err("peer-to-peer DMA is not supported by non-block device namespaces\n");
		return -EINVAL;
	}

	if (!blk_queue_pci_p2pdma(ns->bdev->bd_disk->queue)) {
		pr_err("peer-to-peer DMA is not supported by the driver of %s\n",
		       ns->device_path);
		return -EINVAL;
	}

	if (ns->p2p_dev) {
		ret = pci_p2pdma_distance(ns->p2p_dev, nvmet_ns_dev(ns), true);
		if (ret < 0)
			return -EINVAL;
	} else {
		/*
		 * Right now we just check that there is p2pmem available so
		 * we can report an error to the user right away if there
		 * is not. We'll find the actual device to use once we
		 * setup the controller when the port's device is available.
		 */

		p2p_dev = pci_p2pmem_find(nvmet_ns_dev(ns));
		if (!p2p_dev) {
			pr_err("no peer-to-peer memory is available for %s\n",
			       ns->device_path);
			return -EINVAL;
		}

		pci_dev_put(p2p_dev);
	}

	return 0;
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_p2pmem_ns_add_p2p(struct nvmet_ctrl *ctrl,
				    struct nvmet_ns *ns)
{
	struct device *clients[2];
	struct pci_dev *p2p_dev;
	int ret;

	if (!ctrl->p2p_client || !ns->use_p2pmem)
		return;

	if (ns->p2p_dev) {
		ret = pci_p2pdma_distance(ns->p2p_dev, ctrl->p2p_client, true);
		if (ret < 0)
			return;

		p2p_dev = pci_dev_get(ns->p2p_dev);
	} else {
		clients[0] = ctrl->p2p_client;
		clients[1] = nvmet_ns_dev(ns);

		p2p_dev = pci_p2pmem_find_many(clients, ARRAY_SIZE(clients));
		if (!p2p_dev) {
			pr_err("no peer-to-peer memory is available that's supported by %s and %s\n",
			       dev_name(ctrl->p2p_client), ns->device_path);
			return;
		}
	}

	ret = radix_tree_insert(&ctrl->p2p_ns_map, ns->nsid, p2p_dev);
	if (ret < 0)
		pci_dev_put(p2p_dev);

	pr_info("using p2pmem on %s for nsid %d\n", pci_name(p2p_dev),
		ns->nsid);
}

bool nvmet_ns_revalidate(struct nvmet_ns *ns)
{
	loff_t oldsize = ns->size;

	if (ns->bdev)
		nvmet_bdev_ns_revalidate(ns);
	else
		nvmet_file_ns_revalidate(ns);

	return oldsize != ns->size;
}

int nvmet_ns_enable(struct nvmet_ns *ns)
{
	struct nvmet_subsys *subsys = ns->subsys;
	struct nvmet_ctrl *ctrl;
	int ret;

	mutex_lock(&subsys->lock);
	ret = 0;

	if (nvmet_is_passthru_subsys(subsys)) {
		pr_info("cannot enable both passthru and regular namespaces for a single subsystem");
		goto out_unlock;
	}

	if (ns->enabled)
		goto out_unlock;

	ret = nvmet_bdev_ns_enable(ns);
	if (ret == -ENOTBLK)
		ret = nvmet_file_ns_enable(ns);
	if (ret)
		goto out_unlock;

	ret = nvmet_p2pmem_ns_enable(ns);
	if (ret)
		goto out_dev_disable;

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		nvmet_p2pmem_ns_add_p2p(ctrl, ns);

	if (ns->pr.enable) {
		ret = nvmet_pr_init_ns(ns);
		if (ret)
			goto out_dev_put;
	}

	if (percpu_ref_init(&ns->ref, nvmet_destroy_namespace, 0, GFP_KERNEL))
		goto out_pr_exit;

	nvmet_ns_changed(subsys, ns->nsid);
	ns->enabled = true;
	xa_set_mark(&subsys->namespaces, ns->nsid, NVMET_NS_ENABLED);
	ret = 0;
out_unlock:
	mutex_unlock(&subsys->lock);
	return ret;
out_pr_exit:
	if (ns->pr.enable)
		nvmet_pr_exit_ns(ns);
out_dev_put:
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		pci_dev_put(radix_tree_delete(&ctrl->p2p_ns_map, ns->nsid));
out_dev_disable:
	nvmet_ns_dev_disable(ns);
	goto out_unlock;
}

void nvmet_ns_disable(struct nvmet_ns *ns)
{
	struct nvmet_subsys *subsys = ns->subsys;
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	if (!ns->enabled)
		goto out_unlock;

	ns->enabled = false;
	xa_clear_mark(&subsys->namespaces, ns->nsid, NVMET_NS_ENABLED);

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		pci_dev_put(radix_tree_delete(&ctrl->p2p_ns_map, ns->nsid));

	mutex_unlock(&subsys->lock);

	/*
	 * Now that we removed the namespaces from the lookup list, we
	 * can kill the per_cpu ref and wait for any remaining references
	 * to be dropped, as well as a RCU grace period for anyone only
	 * using the namespace under rcu_read_lock().  Note that we can't
	 * use call_rcu here as we need to ensure the namespaces have
	 * been fully destroyed before unloading the module.
	 */
	percpu_ref_kill(&ns->ref);
	synchronize_rcu();
	wait_for_completion(&ns->disable_done);
	percpu_ref_exit(&ns->ref);

	if (ns->pr.enable)
		nvmet_pr_exit_ns(ns);

	mutex_lock(&subsys->lock);
	nvmet_ns_changed(subsys, ns->nsid);
	nvmet_ns_dev_disable(ns);
out_unlock:
	mutex_unlock(&subsys->lock);
}

void nvmet_ns_free(struct nvmet_ns *ns)
{
	struct nvmet_subsys *subsys = ns->subsys;

	nvmet_ns_disable(ns);

	mutex_lock(&subsys->lock);

	xa_erase(&subsys->namespaces, ns->nsid);
	if (ns->nsid == subsys->max_nsid)
		subsys->max_nsid = nvmet_max_nsid(subsys);

	subsys->nr_namespaces--;
	mutex_unlock(&subsys->lock);

	down_write(&nvmet_ana_sem);
	nvmet_ana_group_enabled[ns->anagrpid]--;
	up_write(&nvmet_ana_sem);

	kfree(ns->device_path);
	kfree(ns);
}

struct nvmet_ns *nvmet_ns_alloc(struct nvmet_subsys *subsys, u32 nsid)
{
	struct nvmet_ns *ns;

	mutex_lock(&subsys->lock);

	if (subsys->nr_namespaces == NVMET_MAX_NAMESPACES)
		goto out_unlock;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		goto out_unlock;

	init_completion(&ns->disable_done);

	ns->nsid = nsid;
	ns->subsys = subsys;

	if (ns->nsid > subsys->max_nsid)
		subsys->max_nsid = nsid;

	if (xa_insert(&subsys->namespaces, ns->nsid, ns, GFP_KERNEL))
		goto out_exit;

	subsys->nr_namespaces++;

	mutex_unlock(&subsys->lock);

	down_write(&nvmet_ana_sem);
	ns->anagrpid = NVMET_DEFAULT_ANA_GRPID;
	nvmet_ana_group_enabled[ns->anagrpid]++;
	up_write(&nvmet_ana_sem);

	uuid_gen(&ns->uuid);
	ns->buffered_io = false;
	ns->csi = NVME_CSI_NVM;

	return ns;
out_exit:
	subsys->max_nsid = nvmet_max_nsid(subsys);
	kfree(ns);
out_unlock:
	mutex_unlock(&subsys->lock);
	return NULL;
}

static void nvmet_update_sq_head(struct nvmet_req *req)
{
	if (req->sq->size) {
		u32 old_sqhd, new_sqhd;

		old_sqhd = READ_ONCE(req->sq->sqhd);
		do {
			new_sqhd = (old_sqhd + 1) % req->sq->size;
		} while (!try_cmpxchg(&req->sq->sqhd, &old_sqhd, new_sqhd));
	}
	req->cqe->sq_head = cpu_to_le16(req->sq->sqhd & 0x0000FFFF);
}

static void nvmet_set_error(struct nvmet_req *req, u16 status)
{
	struct nvmet_ctrl *ctrl = req->sq->ctrl;
	struct nvme_error_slot *new_error_slot;
	unsigned long flags;

	req->cqe->status = cpu_to_le16(status << 1);

	if (!ctrl || req->error_loc == NVMET_NO_ERROR_LOC)
		return;

	spin_lock_irqsave(&ctrl->error_lock, flags);
	ctrl->err_counter++;
	new_error_slot =
		&ctrl->slots[ctrl->err_counter % NVMET_ERROR_LOG_SLOTS];

	new_error_slot->error_count = cpu_to_le64(ctrl->err_counter);
	new_error_slot->sqid = cpu_to_le16(req->sq->qid);
	new_error_slot->cmdid = cpu_to_le16(req->cmd->common.command_id);
	new_error_slot->status_field = cpu_to_le16(status << 1);
	new_error_slot->param_error_location = cpu_to_le16(req->error_loc);
	new_error_slot->lba = cpu_to_le64(req->error_slba);
	new_error_slot->nsid = req->cmd->common.nsid;
	spin_unlock_irqrestore(&ctrl->error_lock, flags);

	/* set the more bit for this request */
	req->cqe->status |= cpu_to_le16(1 << 14);
}

static void __nvmet_req_complete(struct nvmet_req *req, u16 status)
{
	struct nvmet_ns *ns = req->ns;
	struct nvmet_pr_per_ctrl_ref *pc_ref = req->pc_ref;

	if (!req->sq->sqhd_disabled)
		nvmet_update_sq_head(req);
	req->cqe->sq_id = cpu_to_le16(req->sq->qid);
	req->cqe->command_id = req->cmd->common.command_id;

	if (unlikely(status))
		nvmet_set_error(req, status);

	trace_nvmet_req_complete(req);

	req->ops->queue_response(req);

	if (pc_ref)
		nvmet_pr_put_ns_pc_ref(pc_ref);
	if (ns)
		nvmet_put_namespace(ns);
}

void nvmet_req_complete(struct nvmet_req *req, u16 status)
{
	struct nvmet_sq *sq = req->sq;

	__nvmet_req_complete(req, status);
	percpu_ref_put(&sq->ref);
}
EXPORT_SYMBOL_GPL(nvmet_req_complete);

void nvmet_cq_init(struct nvmet_cq *cq)
{
	refcount_set(&cq->ref, 1);
}
EXPORT_SYMBOL_GPL(nvmet_cq_init);

bool nvmet_cq_get(struct nvmet_cq *cq)
{
	return refcount_inc_not_zero(&cq->ref);
}
EXPORT_SYMBOL_GPL(nvmet_cq_get);

void nvmet_cq_put(struct nvmet_cq *cq)
{
	if (refcount_dec_and_test(&cq->ref))
		nvmet_cq_destroy(cq);
}
EXPORT_SYMBOL_GPL(nvmet_cq_put);

void nvmet_cq_setup(struct nvmet_ctrl *ctrl, struct nvmet_cq *cq,
		u16 qid, u16 size)
{
	cq->qid = qid;
	cq->size = size;

	ctrl->cqs[qid] = cq;
}

void nvmet_cq_destroy(struct nvmet_cq *cq)
{
	struct nvmet_ctrl *ctrl = cq->ctrl;

	if (ctrl) {
		ctrl->cqs[cq->qid] = NULL;
		nvmet_ctrl_put(cq->ctrl);
		cq->ctrl = NULL;
	}
}

void nvmet_sq_setup(struct nvmet_ctrl *ctrl, struct nvmet_sq *sq,
		u16 qid, u16 size)
{
	sq->sqhd = 0;
	sq->qid = qid;
	sq->size = size;

	ctrl->sqs[qid] = sq;
}

static void nvmet_confirm_sq(struct percpu_ref *ref)
{
	struct nvmet_sq *sq = container_of(ref, struct nvmet_sq, ref);

	complete(&sq->confirm_done);
}

u16 nvmet_check_cqid(struct nvmet_ctrl *ctrl, u16 cqid, bool create)
{
	if (!ctrl->cqs)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;

	if (cqid > ctrl->subsys->max_qid)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	if ((create && ctrl->cqs[cqid]) || (!create && !ctrl->cqs[cqid]))
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	return NVME_SC_SUCCESS;
}

u16 nvmet_check_io_cqid(struct nvmet_ctrl *ctrl, u16 cqid, bool create)
{
	if (!cqid)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	return nvmet_check_cqid(ctrl, cqid, create);
}

bool nvmet_cq_in_use(struct nvmet_cq *cq)
{
	return refcount_read(&cq->ref) > 1;
}
EXPORT_SYMBOL_GPL(nvmet_cq_in_use);

u16 nvmet_cq_create(struct nvmet_ctrl *ctrl, struct nvmet_cq *cq,
		    u16 qid, u16 size)
{
	u16 status;

	status = nvmet_check_cqid(ctrl, qid, true);
	if (status != NVME_SC_SUCCESS)
		return status;

	if (!kref_get_unless_zero(&ctrl->ref))
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	cq->ctrl = ctrl;

	nvmet_cq_init(cq);
	nvmet_cq_setup(ctrl, cq, qid, size);

	return NVME_SC_SUCCESS;
}
EXPORT_SYMBOL_GPL(nvmet_cq_create);

u16 nvmet_check_sqid(struct nvmet_ctrl *ctrl, u16 sqid,
		     bool create)
{
	if (!ctrl->sqs)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;

	if (sqid > ctrl->subsys->max_qid)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	if ((create && ctrl->sqs[sqid]) ||
	    (!create && !ctrl->sqs[sqid]))
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	return NVME_SC_SUCCESS;
}

u16 nvmet_sq_create(struct nvmet_ctrl *ctrl, struct nvmet_sq *sq,
		    struct nvmet_cq *cq, u16 sqid, u16 size)
{
	u16 status;
	int ret;

	if (!kref_get_unless_zero(&ctrl->ref))
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;

	status = nvmet_check_sqid(ctrl, sqid, true);
	if (status != NVME_SC_SUCCESS)
		return status;

	ret = nvmet_sq_init(sq, cq);
	if (ret) {
		status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
		goto ctrl_put;
	}

	nvmet_sq_setup(ctrl, sq, sqid, size);
	sq->ctrl = ctrl;

	return NVME_SC_SUCCESS;

ctrl_put:
	nvmet_ctrl_put(ctrl);
	return status;
}
EXPORT_SYMBOL_GPL(nvmet_sq_create);

void nvmet_sq_destroy(struct nvmet_sq *sq)
{
	struct nvmet_ctrl *ctrl = sq->ctrl;

	/*
	 * If this is the admin queue, complete all AERs so that our
	 * queue doesn't have outstanding requests on it.
	 */
	if (ctrl && ctrl->sqs && ctrl->sqs[0] == sq)
		nvmet_async_events_failall(ctrl);
	percpu_ref_kill_and_confirm(&sq->ref, nvmet_confirm_sq);
	wait_for_completion(&sq->confirm_done);
	wait_for_completion(&sq->free_done);
	percpu_ref_exit(&sq->ref);
	nvmet_auth_sq_free(sq);
	nvmet_cq_put(sq->cq);

	/*
	 * we must reference the ctrl again after waiting for inflight IO
	 * to complete. Because admin connect may have sneaked in after we
	 * store sq->ctrl locally, but before we killed the percpu_ref. the
	 * admin connect allocates and assigns sq->ctrl, which now needs a
	 * final ref put, as this ctrl is going away.
	 */
	ctrl = sq->ctrl;

	if (ctrl) {
		/*
		 * The teardown flow may take some time, and the host may not
		 * send us keep-alive during this period, hence reset the
		 * traffic based keep-alive timer so we don't trigger a
		 * controller teardown as a result of a keep-alive expiration.
		 */
		ctrl->reset_tbkas = true;
		sq->ctrl->sqs[sq->qid] = NULL;
		nvmet_ctrl_put(ctrl);
		sq->ctrl = NULL; /* allows reusing the queue later */
	}
}
EXPORT_SYMBOL_GPL(nvmet_sq_destroy);

static void nvmet_sq_free(struct percpu_ref *ref)
{
	struct nvmet_sq *sq = container_of(ref, struct nvmet_sq, ref);

	complete(&sq->free_done);
}

int nvmet_sq_init(struct nvmet_sq *sq, struct nvmet_cq *cq)
{
	int ret;

	if (!nvmet_cq_get(cq))
		return -EINVAL;

	ret = percpu_ref_init(&sq->ref, nvmet_sq_free, 0, GFP_KERNEL);
	if (ret) {
		pr_err("percpu_ref init failed!\n");
		nvmet_cq_put(cq);
		return ret;
	}
	init_completion(&sq->free_done);
	init_completion(&sq->confirm_done);
	nvmet_auth_sq_init(sq);
	sq->cq = cq;

	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_sq_init);

static inline u16 nvmet_check_ana_state(struct nvmet_port *port,
		struct nvmet_ns *ns)
{
	enum nvme_ana_state state = port->ana_state[ns->anagrpid];

	if (unlikely(state == NVME_ANA_INACCESSIBLE))
		return NVME_SC_ANA_INACCESSIBLE;
	if (unlikely(state == NVME_ANA_PERSISTENT_LOSS))
		return NVME_SC_ANA_PERSISTENT_LOSS;
	if (unlikely(state == NVME_ANA_CHANGE))
		return NVME_SC_ANA_TRANSITION;
	return 0;
}

static inline u16 nvmet_io_cmd_check_access(struct nvmet_req *req)
{
	if (unlikely(req->ns->readonly)) {
		switch (req->cmd->common.opcode) {
		case nvme_cmd_read:
		case nvme_cmd_flush:
			break;
		default:
			return NVME_SC_NS_WRITE_PROTECTED;
		}
	}

	return 0;
}

static u32 nvmet_io_cmd_transfer_len(struct nvmet_req *req)
{
	struct nvme_command *cmd = req->cmd;
	u32 metadata_len = 0;

	if (nvme_is_fabrics(cmd))
		return nvmet_fabrics_io_cmd_data_len(req);

	if (!req->ns)
		return 0;

	switch (req->cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
	case nvme_cmd_zone_append:
		if (req->sq->ctrl->pi_support && nvmet_ns_has_pi(req->ns))
			metadata_len = nvmet_rw_metadata_len(req);
		return nvmet_rw_data_len(req) + metadata_len;
	case nvme_cmd_dsm:
		return nvmet_dsm_len(req);
	case nvme_cmd_zone_mgmt_recv:
		return (le32_to_cpu(req->cmd->zmr.numd) + 1) << 2;
	default:
		return 0;
	}
}

static u16 nvmet_parse_io_cmd(struct nvmet_req *req)
{
	struct nvme_command *cmd = req->cmd;
	u16 ret;

	if (nvme_is_fabrics(cmd))
		return nvmet_parse_fabrics_io_cmd(req);

	if (unlikely(!nvmet_check_auth_status(req)))
		return NVME_SC_AUTH_REQUIRED | NVME_STATUS_DNR;

	ret = nvmet_check_ctrl_status(req);
	if (unlikely(ret))
		return ret;

	if (nvmet_is_passthru_req(req))
		return nvmet_parse_passthru_io_cmd(req);

	ret = nvmet_req_find_ns(req);
	if (unlikely(ret))
		return ret;

	ret = nvmet_check_ana_state(req->port, req->ns);
	if (unlikely(ret)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		return ret;
	}
	ret = nvmet_io_cmd_check_access(req);
	if (unlikely(ret)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		return ret;
	}

	if (req->ns->pr.enable) {
		ret = nvmet_parse_pr_cmd(req);
		if (!ret)
			return ret;
	}

	switch (req->ns->csi) {
	case NVME_CSI_NVM:
		if (req->ns->file)
			ret = nvmet_file_parse_io_cmd(req);
		else
			ret = nvmet_bdev_parse_io_cmd(req);
		break;
	case NVME_CSI_ZNS:
		if (IS_ENABLED(CONFIG_BLK_DEV_ZONED))
			ret = nvmet_bdev_zns_parse_io_cmd(req);
		else
			ret = NVME_SC_INVALID_IO_CMD_SET;
		break;
	default:
		ret = NVME_SC_INVALID_IO_CMD_SET;
	}
	if (ret)
		return ret;

	if (req->ns->pr.enable) {
		ret = nvmet_pr_check_cmd_access(req);
		if (ret)
			return ret;

		ret = nvmet_pr_get_ns_pc_ref(req);
	}
	return ret;
}

bool nvmet_req_init(struct nvmet_req *req, struct nvmet_sq *sq,
		const struct nvmet_fabrics_ops *ops)
{
	u8 flags = req->cmd->common.flags;
	u16 status;

	req->cq = sq->cq;
	req->sq = sq;
	req->ops = ops;
	req->sg = NULL;
	req->metadata_sg = NULL;
	req->sg_cnt = 0;
	req->metadata_sg_cnt = 0;
	req->transfer_len = 0;
	req->metadata_len = 0;
	req->cqe->result.u64 = 0;
	req->cqe->status = 0;
	req->cqe->sq_head = 0;
	req->ns = NULL;
	req->error_loc = NVMET_NO_ERROR_LOC;
	req->error_slba = 0;
	req->pc_ref = NULL;

	/* no support for fused commands yet */
	if (unlikely(flags & (NVME_CMD_FUSE_FIRST | NVME_CMD_FUSE_SECOND))) {
		req->error_loc = offsetof(struct nvme_common_command, flags);
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto fail;
	}

	/*
	 * For fabrics, PSDT field shall describe metadata pointer (MPTR) that
	 * contains an address of a single contiguous physical buffer that is
	 * byte aligned. For PCI controllers, this is optional so not enforced.
	 */
	if (unlikely((flags & NVME_CMD_SGL_ALL) != NVME_CMD_SGL_METABUF)) {
		if (!req->sq->ctrl || !nvmet_is_pci_ctrl(req->sq->ctrl)) {
			req->error_loc =
				offsetof(struct nvme_common_command, flags);
			status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
			goto fail;
		}
	}

	if (unlikely(!req->sq->ctrl))
		/* will return an error for any non-connect command: */
		status = nvmet_parse_connect_cmd(req);
	else if (likely(req->sq->qid != 0))
		status = nvmet_parse_io_cmd(req);
	else
		status = nvmet_parse_admin_cmd(req);

	if (status)
		goto fail;

	trace_nvmet_req_init(req, req->cmd);

	if (unlikely(!percpu_ref_tryget_live(&sq->ref))) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto fail;
	}

	if (sq->ctrl)
		sq->ctrl->reset_tbkas = true;

	return true;

fail:
	__nvmet_req_complete(req, status);
	return false;
}
EXPORT_SYMBOL_GPL(nvmet_req_init);

void nvmet_req_uninit(struct nvmet_req *req)
{
	percpu_ref_put(&req->sq->ref);
	if (req->pc_ref)
		nvmet_pr_put_ns_pc_ref(req->pc_ref);
	if (req->ns)
		nvmet_put_namespace(req->ns);
}
EXPORT_SYMBOL_GPL(nvmet_req_uninit);

size_t nvmet_req_transfer_len(struct nvmet_req *req)
{
	if (likely(req->sq->qid != 0))
		return nvmet_io_cmd_transfer_len(req);
	if (unlikely(!req->sq->ctrl))
		return nvmet_connect_cmd_data_len(req);
	return nvmet_admin_cmd_data_len(req);
}
EXPORT_SYMBOL_GPL(nvmet_req_transfer_len);

bool nvmet_check_transfer_len(struct nvmet_req *req, size_t len)
{
	if (unlikely(len != req->transfer_len)) {
		u16 status;

		req->error_loc = offsetof(struct nvme_common_command, dptr);
		if (req->cmd->common.flags & NVME_CMD_SGL_ALL)
			status = NVME_SC_SGL_INVALID_DATA;
		else
			status = NVME_SC_INVALID_FIELD;
		nvmet_req_complete(req, status | NVME_STATUS_DNR);
		return false;
	}

	return true;
}
EXPORT_SYMBOL_GPL(nvmet_check_transfer_len);

bool nvmet_check_data_len_lte(struct nvmet_req *req, size_t data_len)
{
	if (unlikely(data_len > req->transfer_len)) {
		u16 status;

		req->error_loc = offsetof(struct nvme_common_command, dptr);
		if (req->cmd->common.flags & NVME_CMD_SGL_ALL)
			status = NVME_SC_SGL_INVALID_DATA;
		else
			status = NVME_SC_INVALID_FIELD;
		nvmet_req_complete(req, status | NVME_STATUS_DNR);
		return false;
	}

	return true;
}

static unsigned int nvmet_data_transfer_len(struct nvmet_req *req)
{
	return req->transfer_len - req->metadata_len;
}

static int nvmet_req_alloc_p2pmem_sgls(struct pci_dev *p2p_dev,
		struct nvmet_req *req)
{
	req->sg = pci_p2pmem_alloc_sgl(p2p_dev, &req->sg_cnt,
			nvmet_data_transfer_len(req));
	if (!req->sg)
		goto out_err;

	if (req->metadata_len) {
		req->metadata_sg = pci_p2pmem_alloc_sgl(p2p_dev,
				&req->metadata_sg_cnt, req->metadata_len);
		if (!req->metadata_sg)
			goto out_free_sg;
	}

	req->p2p_dev = p2p_dev;

	return 0;
out_free_sg:
	pci_p2pmem_free_sgl(req->p2p_dev, req->sg);
out_err:
	return -ENOMEM;
}

static struct pci_dev *nvmet_req_find_p2p_dev(struct nvmet_req *req)
{
	if (!IS_ENABLED(CONFIG_PCI_P2PDMA) ||
	    !req->sq->ctrl || !req->sq->qid || !req->ns)
		return NULL;
	return radix_tree_lookup(&req->sq->ctrl->p2p_ns_map, req->ns->nsid);
}

int nvmet_req_alloc_sgls(struct nvmet_req *req)
{
	struct pci_dev *p2p_dev = nvmet_req_find_p2p_dev(req);

	if (p2p_dev && !nvmet_req_alloc_p2pmem_sgls(p2p_dev, req))
		return 0;

	req->sg = sgl_alloc(nvmet_data_transfer_len(req), GFP_KERNEL,
			    &req->sg_cnt);
	if (unlikely(!req->sg))
		goto out;

	if (req->metadata_len) {
		req->metadata_sg = sgl_alloc(req->metadata_len, GFP_KERNEL,
					     &req->metadata_sg_cnt);
		if (unlikely(!req->metadata_sg))
			goto out_free;
	}

	return 0;
out_free:
	sgl_free(req->sg);
out:
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(nvmet_req_alloc_sgls);

void nvmet_req_free_sgls(struct nvmet_req *req)
{
	if (req->p2p_dev) {
		pci_p2pmem_free_sgl(req->p2p_dev, req->sg);
		if (req->metadata_sg)
			pci_p2pmem_free_sgl(req->p2p_dev, req->metadata_sg);
		req->p2p_dev = NULL;
	} else {
		sgl_free(req->sg);
		if (req->metadata_sg)
			sgl_free(req->metadata_sg);
	}

	req->sg = NULL;
	req->metadata_sg = NULL;
	req->sg_cnt = 0;
	req->metadata_sg_cnt = 0;
}
EXPORT_SYMBOL_GPL(nvmet_req_free_sgls);

static inline bool nvmet_css_supported(u8 cc_css)
{
	switch (cc_css << NVME_CC_CSS_SHIFT) {
	case NVME_CC_CSS_NVM:
	case NVME_CC_CSS_CSI:
		return true;
	default:
		return false;
	}
}

static void nvmet_start_ctrl(struct nvmet_ctrl *ctrl)
{
	lockdep_assert_held(&ctrl->lock);

	/*
	 * Only I/O controllers should verify iosqes,iocqes.
	 * Strictly speaking, the spec says a discovery controller
	 * should verify iosqes,iocqes are zeroed, however that
	 * would break backwards compatibility, so don't enforce it.
	 */
	if (!nvmet_is_disc_subsys(ctrl->subsys) &&
	    (nvmet_cc_iosqes(ctrl->cc) != NVME_NVM_IOSQES ||
	     nvmet_cc_iocqes(ctrl->cc) != NVME_NVM_IOCQES)) {
		ctrl->csts = NVME_CSTS_CFS;
		return;
	}

	if (nvmet_cc_mps(ctrl->cc) != 0 ||
	    nvmet_cc_ams(ctrl->cc) != 0 ||
	    !nvmet_css_supported(nvmet_cc_css(ctrl->cc))) {
		ctrl->csts = NVME_CSTS_CFS;
		return;
	}

	ctrl->csts = NVME_CSTS_RDY;

	/*
	 * Controllers that are not yet enabled should not really enforce the
	 * keep alive timeout, but we still want to track a timeout and cleanup
	 * in case a host died before it enabled the controller.  Hence, simply
	 * reset the keep alive timer when the controller is enabled.
	 */
	if (ctrl->kato)
		mod_delayed_work(nvmet_wq, &ctrl->ka_work, ctrl->kato * HZ);
}

static void nvmet_clear_ctrl(struct nvmet_ctrl *ctrl)
{
	lockdep_assert_held(&ctrl->lock);

	/* XXX: tear down queues? */
	ctrl->csts &= ~NVME_CSTS_RDY;
	ctrl->cc = 0;
}

void nvmet_update_cc(struct nvmet_ctrl *ctrl, u32 new)
{
	u32 old;

	mutex_lock(&ctrl->lock);
	old = ctrl->cc;
	ctrl->cc = new;

	if (nvmet_cc_en(new) && !nvmet_cc_en(old))
		nvmet_start_ctrl(ctrl);
	if (!nvmet_cc_en(new) && nvmet_cc_en(old))
		nvmet_clear_ctrl(ctrl);
	if (nvmet_cc_shn(new) && !nvmet_cc_shn(old)) {
		nvmet_clear_ctrl(ctrl);
		ctrl->csts |= NVME_CSTS_SHST_CMPLT;
	}
	if (!nvmet_cc_shn(new) && nvmet_cc_shn(old))
		ctrl->csts &= ~NVME_CSTS_SHST_CMPLT;
	mutex_unlock(&ctrl->lock);
}
EXPORT_SYMBOL_GPL(nvmet_update_cc);

static void nvmet_init_cap(struct nvmet_ctrl *ctrl)
{
	/* command sets supported: NVMe command set: */
	ctrl->cap = (1ULL << 37);
	/* Controller supports one or more I/O Command Sets */
	ctrl->cap |= (1ULL << 43);
	/* CC.EN timeout in 500msec units: */
	ctrl->cap |= (15ULL << 24);
	/* maximum queue entries supported: */
	if (ctrl->ops->get_max_queue_size)
		ctrl->cap |= min_t(u16, ctrl->ops->get_max_queue_size(ctrl),
				   ctrl->port->max_queue_size) - 1;
	else
		ctrl->cap |= ctrl->port->max_queue_size - 1;

	if (nvmet_is_passthru_subsys(ctrl->subsys))
		nvmet_passthrough_override_cap(ctrl);
}

struct nvmet_ctrl *nvmet_ctrl_find_get(const char *subsysnqn,
				       const char *hostnqn, u16 cntlid,
				       struct nvmet_req *req)
{
	struct nvmet_ctrl *ctrl = NULL;
	struct nvmet_subsys *subsys;

	subsys = nvmet_find_get_subsys(req->port, subsysnqn);
	if (!subsys) {
		pr_warn("connect request for invalid subsystem %s!\n",
			subsysnqn);
		req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(subsysnqn);
		goto out;
	}

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (ctrl->cntlid == cntlid) {
			if (strncmp(hostnqn, ctrl->hostnqn, NVMF_NQN_SIZE)) {
				pr_warn("hostnqn mismatch.\n");
				continue;
			}
			if (!kref_get_unless_zero(&ctrl->ref))
				continue;

			/* ctrl found */
			goto found;
		}
	}

	ctrl = NULL; /* ctrl not found */
	pr_warn("could not find controller %d for subsys %s / host %s\n",
		cntlid, subsysnqn, hostnqn);
	req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(cntlid);

found:
	mutex_unlock(&subsys->lock);
	nvmet_subsys_put(subsys);
out:
	return ctrl;
}

u16 nvmet_check_ctrl_status(struct nvmet_req *req)
{
	if (unlikely(!(req->sq->ctrl->cc & NVME_CC_ENABLE))) {
		pr_err("got cmd %d while CC.EN == 0 on qid = %d\n",
		       req->cmd->common.opcode, req->sq->qid);
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	}

	if (unlikely(!(req->sq->ctrl->csts & NVME_CSTS_RDY))) {
		pr_err("got cmd %d while CSTS.RDY == 0 on qid = %d\n",
		       req->cmd->common.opcode, req->sq->qid);
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	}

	if (unlikely(!nvmet_check_auth_status(req))) {
		pr_warn("qid %d not authenticated\n", req->sq->qid);
		return NVME_SC_AUTH_REQUIRED | NVME_STATUS_DNR;
	}
	return 0;
}

bool nvmet_host_allowed(struct nvmet_subsys *subsys, const char *hostnqn)
{
	struct nvmet_host_link *p;

	lockdep_assert_held(&nvmet_config_sem);

	if (subsys->allow_any_host)
		return true;

	if (nvmet_is_disc_subsys(subsys)) /* allow all access to disc subsys */
		return true;

	list_for_each_entry(p, &subsys->hosts, entry) {
		if (!strcmp(nvmet_host_name(p->host), hostnqn))
			return true;
	}

	return false;
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_setup_p2p_ns_map(struct nvmet_ctrl *ctrl,
		struct device *p2p_client)
{
	struct nvmet_ns *ns;
	unsigned long idx;

	if (!p2p_client)
		return;

	ctrl->p2p_client = get_device(p2p_client);

	nvmet_for_each_enabled_ns(&ctrl->subsys->namespaces, idx, ns)
		nvmet_p2pmem_ns_add_p2p(ctrl, ns);
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_release_p2p_ns_map(struct nvmet_ctrl *ctrl)
{
	struct radix_tree_iter iter;
	void __rcu **slot;

	radix_tree_for_each_slot(slot, &ctrl->p2p_ns_map, &iter, 0)
		pci_dev_put(radix_tree_deref_slot(slot));

	put_device(ctrl->p2p_client);
}

static void nvmet_fatal_error_handler(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl =
			container_of(work, struct nvmet_ctrl, fatal_err_work);

	pr_err("ctrl %d fatal error occurred!\n", ctrl->cntlid);
	ctrl->ops->delete_ctrl(ctrl);
}

struct nvmet_ctrl *nvmet_alloc_ctrl(struct nvmet_alloc_ctrl_args *args)
{
	struct nvmet_subsys *subsys;
	struct nvmet_ctrl *ctrl;
	u32 kato = args->kato;
	u8 dhchap_status;
	int ret;

	args->status = NVME_SC_CONNECT_INVALID_PARAM | NVME_STATUS_DNR;
	subsys = nvmet_find_get_subsys(args->port, args->subsysnqn);
	if (!subsys) {
		pr_warn("connect request for invalid subsystem %s!\n",
			args->subsysnqn);
		args->result = IPO_IATTR_CONNECT_DATA(subsysnqn);
		args->error_loc = offsetof(struct nvme_common_command, dptr);
		return NULL;
	}

	down_read(&nvmet_config_sem);
	if (!nvmet_host_allowed(subsys, args->hostnqn)) {
		pr_info("connect by host %s for subsystem %s not allowed\n",
			args->hostnqn, args->subsysnqn);
		args->result = IPO_IATTR_CONNECT_DATA(hostnqn);
		up_read(&nvmet_config_sem);
		args->status = NVME_SC_CONNECT_INVALID_HOST | NVME_STATUS_DNR;
		args->error_loc = offsetof(struct nvme_common_command, dptr);
		goto out_put_subsystem;
	}
	up_read(&nvmet_config_sem);

	args->status = NVME_SC_INTERNAL;
	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		goto out_put_subsystem;
	mutex_init(&ctrl->lock);

	ctrl->port = args->port;
	ctrl->ops = args->ops;

#ifdef CONFIG_NVME_TARGET_PASSTHRU
	/* By default, set loop targets to clear IDS by default */
	if (ctrl->port->disc_addr.trtype == NVMF_TRTYPE_LOOP)
		subsys->clear_ids = 1;
#endif

	INIT_WORK(&ctrl->async_event_work, nvmet_async_event_work);
	INIT_LIST_HEAD(&ctrl->async_events);
	INIT_RADIX_TREE(&ctrl->p2p_ns_map, GFP_KERNEL);
	INIT_WORK(&ctrl->fatal_err_work, nvmet_fatal_error_handler);
	INIT_DELAYED_WORK(&ctrl->ka_work, nvmet_keep_alive_timer);

	memcpy(ctrl->subsysnqn, args->subsysnqn, NVMF_NQN_SIZE);
	memcpy(ctrl->hostnqn, args->hostnqn, NVMF_NQN_SIZE);

	kref_init(&ctrl->ref);
	ctrl->subsys = subsys;
	ctrl->pi_support = ctrl->port->pi_enable && ctrl->subsys->pi_support;
	nvmet_init_cap(ctrl);
	WRITE_ONCE(ctrl->aen_enabled, NVMET_AEN_CFG_OPTIONAL);

	ctrl->changed_ns_list = kmalloc_array(NVME_MAX_CHANGED_NAMESPACES,
			sizeof(__le32), GFP_KERNEL);
	if (!ctrl->changed_ns_list)
		goto out_free_ctrl;

	ctrl->sqs = kcalloc(subsys->max_qid + 1,
			sizeof(struct nvmet_sq *),
			GFP_KERNEL);
	if (!ctrl->sqs)
		goto out_free_changed_ns_list;

	ctrl->cqs = kcalloc(subsys->max_qid + 1, sizeof(struct nvmet_cq *),
			   GFP_KERNEL);
	if (!ctrl->cqs)
		goto out_free_sqs;

	ret = ida_alloc_range(&cntlid_ida,
			     subsys->cntlid_min, subsys->cntlid_max,
			     GFP_KERNEL);
	if (ret < 0) {
		args->status = NVME_SC_CONNECT_CTRL_BUSY | NVME_STATUS_DNR;
		goto out_free_cqs;
	}
	ctrl->cntlid = ret;

	/*
	 * Discovery controllers may use some arbitrary high value
	 * in order to cleanup stale discovery sessions
	 */
	if (nvmet_is_disc_subsys(ctrl->subsys) && !kato)
		kato = NVMET_DISC_KATO_MS;

	/* keep-alive timeout in seconds */
	ctrl->kato = DIV_ROUND_UP(kato, 1000);

	ctrl->err_counter = 0;
	spin_lock_init(&ctrl->error_lock);

	nvmet_start_keep_alive_timer(ctrl);

	mutex_lock(&subsys->lock);
	ret = nvmet_ctrl_init_pr(ctrl);
	if (ret)
		goto init_pr_fail;
	list_add_tail(&ctrl->subsys_entry, &subsys->ctrls);
	nvmet_setup_p2p_ns_map(ctrl, args->p2p_client);
	nvmet_debugfs_ctrl_setup(ctrl);
	mutex_unlock(&subsys->lock);

	if (args->hostid)
		uuid_copy(&ctrl->hostid, args->hostid);

	dhchap_status = nvmet_setup_auth(ctrl, args->sq);
	if (dhchap_status) {
		pr_err("Failed to setup authentication, dhchap status %u\n",
		       dhchap_status);
		nvmet_ctrl_put(ctrl);
		if (dhchap_status == NVME_AUTH_DHCHAP_FAILURE_FAILED)
			args->status =
				NVME_SC_CONNECT_INVALID_HOST | NVME_STATUS_DNR;
		else
			args->status = NVME_SC_INTERNAL;
		return NULL;
	}

	args->status = NVME_SC_SUCCESS;

	pr_info("Created %s controller %d for subsystem %s for NQN %s%s%s%s.\n",
		nvmet_is_disc_subsys(ctrl->subsys) ? "discovery" : "nvm",
		ctrl->cntlid, ctrl->subsys->subsysnqn, ctrl->hostnqn,
		ctrl->pi_support ? " T10-PI is enabled" : "",
		nvmet_has_auth(ctrl, args->sq) ? " with DH-HMAC-CHAP" : "",
		nvmet_queue_tls_keyid(args->sq) ? ", TLS" : "");

	return ctrl;

init_pr_fail:
	mutex_unlock(&subsys->lock);
	nvmet_stop_keep_alive_timer(ctrl);
	ida_free(&cntlid_ida, ctrl->cntlid);
out_free_cqs:
	kfree(ctrl->cqs);
out_free_sqs:
	kfree(ctrl->sqs);
out_free_changed_ns_list:
	kfree(ctrl->changed_ns_list);
out_free_ctrl:
	kfree(ctrl);
out_put_subsystem:
	nvmet_subsys_put(subsys);
	return NULL;
}
EXPORT_SYMBOL_GPL(nvmet_alloc_ctrl);

static void nvmet_ctrl_free(struct kref *ref)
{
	struct nvmet_ctrl *ctrl = container_of(ref, struct nvmet_ctrl, ref);
	struct nvmet_subsys *subsys = ctrl->subsys;

	mutex_lock(&subsys->lock);
	nvmet_ctrl_destroy_pr(ctrl);
	nvmet_release_p2p_ns_map(ctrl);
	list_del(&ctrl->subsys_entry);
	mutex_unlock(&subsys->lock);

	nvmet_stop_keep_alive_timer(ctrl);

	flush_work(&ctrl->async_event_work);
	cancel_work_sync(&ctrl->fatal_err_work);

	nvmet_destroy_auth(ctrl);

	nvmet_debugfs_ctrl_free(ctrl);

	ida_free(&cntlid_ida, ctrl->cntlid);

	nvmet_async_events_free(ctrl);
	kfree(ctrl->sqs);
	kfree(ctrl->cqs);
	kfree(ctrl->changed_ns_list);
	kfree(ctrl);

	nvmet_subsys_put(subsys);
}

void nvmet_ctrl_put(struct nvmet_ctrl *ctrl)
{
	kref_put(&ctrl->ref, nvmet_ctrl_free);
}
EXPORT_SYMBOL_GPL(nvmet_ctrl_put);

void nvmet_ctrl_fatal_error(struct nvmet_ctrl *ctrl)
{
	mutex_lock(&ctrl->lock);
	if (!(ctrl->csts & NVME_CSTS_CFS)) {
		ctrl->csts |= NVME_CSTS_CFS;
		queue_work(nvmet_wq, &ctrl->fatal_err_work);
	}
	mutex_unlock(&ctrl->lock);
}
EXPORT_SYMBOL_GPL(nvmet_ctrl_fatal_error);

ssize_t nvmet_ctrl_host_traddr(struct nvmet_ctrl *ctrl,
		char *traddr, size_t traddr_len)
{
	if (!ctrl->ops->host_traddr)
		return -EOPNOTSUPP;
	return ctrl->ops->host_traddr(ctrl, traddr, traddr_len);
}

static struct nvmet_subsys *nvmet_find_get_subsys(struct nvmet_port *port,
		const char *subsysnqn)
{
	struct nvmet_subsys_link *p;

	if (!port)
		return NULL;

	if (!strcmp(NVME_DISC_SUBSYS_NAME, subsysnqn)) {
		if (!kref_get_unless_zero(&nvmet_disc_subsys->ref))
			return NULL;
		return nvmet_disc_subsys;
	}

	down_read(&nvmet_config_sem);
	if (!strncmp(nvmet_disc_subsys->subsysnqn, subsysnqn,
				NVMF_NQN_SIZE)) {
		if (kref_get_unless_zero(&nvmet_disc_subsys->ref)) {
			up_read(&nvmet_config_sem);
			return nvmet_disc_subsys;
		}
	}
	list_for_each_entry(p, &port->subsystems, entry) {
		if (!strncmp(p->subsys->subsysnqn, subsysnqn,
				NVMF_NQN_SIZE)) {
			if (!kref_get_unless_zero(&p->subsys->ref))
				break;
			up_read(&nvmet_config_sem);
			return p->subsys;
		}
	}
	up_read(&nvmet_config_sem);
	return NULL;
}

struct nvmet_subsys *nvmet_subsys_alloc(const char *subsysnqn,
		enum nvme_subsys_type type)
{
	struct nvmet_subsys *subsys;
	char serial[NVMET_SN_MAX_SIZE / 2];
	int ret;

	subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	if (!subsys)
		return ERR_PTR(-ENOMEM);

	subsys->ver = NVMET_DEFAULT_VS;
	/* generate a random serial number as our controllers are ephemeral: */
	get_random_bytes(&serial, sizeof(serial));
	bin2hex(subsys->serial, &serial, sizeof(serial));

	subsys->model_number = kstrdup(NVMET_DEFAULT_CTRL_MODEL, GFP_KERNEL);
	if (!subsys->model_number) {
		ret = -ENOMEM;
		goto free_subsys;
	}

	subsys->ieee_oui = 0;

	subsys->firmware_rev = kstrndup(UTS_RELEASE, NVMET_FR_MAX_SIZE, GFP_KERNEL);
	if (!subsys->firmware_rev) {
		ret = -ENOMEM;
		goto free_mn;
	}

	switch (type) {
	case NVME_NQN_NVME:
		subsys->max_qid = NVMET_NR_QUEUES;
		break;
	case NVME_NQN_DISC:
	case NVME_NQN_CURR:
		subsys->max_qid = 0;
		break;
	default:
		pr_err("%s: Unknown Subsystem type - %d\n", __func__, type);
		ret = -EINVAL;
		goto free_fr;
	}
	subsys->type = type;
	subsys->subsysnqn = kstrndup(subsysnqn, NVMF_NQN_SIZE,
			GFP_KERNEL);
	if (!subsys->subsysnqn) {
		ret = -ENOMEM;
		goto free_fr;
	}
	subsys->cntlid_min = NVME_CNTLID_MIN;
	subsys->cntlid_max = NVME_CNTLID_MAX;
	kref_init(&subsys->ref);

	mutex_init(&subsys->lock);
	xa_init(&subsys->namespaces);
	INIT_LIST_HEAD(&subsys->ctrls);
	INIT_LIST_HEAD(&subsys->hosts);

	ret = nvmet_debugfs_subsys_setup(subsys);
	if (ret)
		goto free_subsysnqn;

	return subsys;

free_subsysnqn:
	kfree(subsys->subsysnqn);
free_fr:
	kfree(subsys->firmware_rev);
free_mn:
	kfree(subsys->model_number);
free_subsys:
	kfree(subsys);
	return ERR_PTR(ret);
}

static void nvmet_subsys_free(struct kref *ref)
{
	struct nvmet_subsys *subsys =
		container_of(ref, struct nvmet_subsys, ref);

	WARN_ON_ONCE(!xa_empty(&subsys->namespaces));

	nvmet_debugfs_subsys_free(subsys);

	xa_destroy(&subsys->namespaces);
	nvmet_passthru_subsys_free(subsys);

	kfree(subsys->subsysnqn);
	kfree(subsys->model_number);
	kfree(subsys->firmware_rev);
	kfree(subsys);
}

void nvmet_subsys_del_ctrls(struct nvmet_subsys *subsys)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		ctrl->ops->delete_ctrl(ctrl);
	mutex_unlock(&subsys->lock);
}

void nvmet_subsys_put(struct nvmet_subsys *subsys)
{
	kref_put(&subsys->ref, nvmet_subsys_free);
}

static int __init nvmet_init(void)
{
	int error = -ENOMEM;

	nvmet_ana_group_enabled[NVMET_DEFAULT_ANA_GRPID] = 1;

	nvmet_bvec_cache = kmem_cache_create("nvmet-bvec",
			NVMET_MAX_MPOOL_BVEC * sizeof(struct bio_vec), 0,
			SLAB_HWCACHE_ALIGN, NULL);
	if (!nvmet_bvec_cache)
		return -ENOMEM;

	zbd_wq = alloc_workqueue("nvmet-zbd-wq", WQ_MEM_RECLAIM, 0);
	if (!zbd_wq)
		goto out_destroy_bvec_cache;

	buffered_io_wq = alloc_workqueue("nvmet-buffered-io-wq",
			WQ_MEM_RECLAIM, 0);
	if (!buffered_io_wq)
		goto out_free_zbd_work_queue;

	nvmet_wq = alloc_workqueue("nvmet-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND | WQ_SYSFS, 0);
	if (!nvmet_wq)
		goto out_free_buffered_work_queue;

	error = nvmet_init_discovery();
	if (error)
		goto out_free_nvmet_work_queue;

	error = nvmet_init_debugfs();
	if (error)
		goto out_exit_discovery;

	error = nvmet_init_configfs();
	if (error)
		goto out_exit_debugfs;

	return 0;

out_exit_debugfs:
	nvmet_exit_debugfs();
out_exit_discovery:
	nvmet_exit_discovery();
out_free_nvmet_work_queue:
	destroy_workqueue(nvmet_wq);
out_free_buffered_work_queue:
	destroy_workqueue(buffered_io_wq);
out_free_zbd_work_queue:
	destroy_workqueue(zbd_wq);
out_destroy_bvec_cache:
	kmem_cache_destroy(nvmet_bvec_cache);
	return error;
}

static void __exit nvmet_exit(void)
{
	nvmet_exit_configfs();
	nvmet_exit_debugfs();
	nvmet_exit_discovery();
	ida_destroy(&cntlid_ida);
	destroy_workqueue(nvmet_wq);
	destroy_workqueue(buffered_io_wq);
	destroy_workqueue(zbd_wq);
	kmem_cache_destroy(nvmet_bvec_cache);

	BUILD_BUG_ON(sizeof(struct nvmf_disc_rsp_page_entry) != 1024);
	BUILD_BUG_ON(sizeof(struct nvmf_disc_rsp_page_hdr) != 1024);
}

module_init(nvmet_init);
module_exit(nvmet_exit);

MODULE_DESCRIPTION("NVMe target core framework");
MODULE_LICENSE("GPL v2");
