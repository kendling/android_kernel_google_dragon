/*
 * Marvell Wireless LAN device driver: major functions
 *
 * Copyright (C) 2011-2014, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available by writing to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA or on the
 * worldwide web at http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#include "main.h"
#include "wmm.h"
#include "cfg80211.h"
#include "11n.h"

#define VERSION	"1.0"

static unsigned int debug_mask = MWIFIEX_DEFAULT_DEBUG_MASK;
module_param(debug_mask, uint, 0);
MODULE_PARM_DESC(debug_mask, "bitmap for debug flags");

const char driver_version[] = "mwifiex " VERSION " (%s) ";
static char *cal_data_cfg;
module_param(cal_data_cfg, charp, 0);

/*
 * This function registers the device and performs all the necessary
 * initializations.
 *
 * The following initialization operations are performed -
 *      - Allocate adapter structure
 *      - Save interface specific operations table in adapter
 *      - Call interface specific initialization routine
 *      - Allocate private structures
 *      - Set default adapter structure parameters
 *      - Initialize locks
 *
 * In case of any errors during inittialization, this function also ensures
 * proper cleanup before exiting.
 */
static int mwifiex_register(void *card, struct mwifiex_if_ops *if_ops,
			    void **padapter)
{
	struct mwifiex_adapter *adapter;
	int i;

	adapter = kzalloc(sizeof(struct mwifiex_adapter), GFP_KERNEL);
	if (!adapter)
		return -ENOMEM;

	*padapter = adapter;
	adapter->card = card;

	/* Save interface specific operations in adapter */
	memmove(&adapter->if_ops, if_ops, sizeof(struct mwifiex_if_ops));
	adapter->debug_mask = debug_mask;

	/* card specific initialization has been deferred until now .. */
	if (adapter->if_ops.init_if)
		if (adapter->if_ops.init_if(adapter))
			goto error;

	adapter->priv_num = 0;

	for (i = 0; i < MWIFIEX_MAX_BSS_NUM; i++) {
		/* Allocate memory for private structure */
		adapter->priv[i] =
			kzalloc(sizeof(struct mwifiex_private), GFP_KERNEL);
		if (!adapter->priv[i])
			goto error;

		adapter->priv[i]->adapter = adapter;
		adapter->priv_num++;
	}
	mwifiex_init_lock_list(adapter);

	init_timer(&adapter->cmd_timer);
	adapter->cmd_timer.function = mwifiex_cmd_timeout_func;
	adapter->cmd_timer.data = (unsigned long) adapter;

	return 0;

error:
	mwifiex_dbg(adapter, ERROR,
		    "info: leave mwifiex_register with error\n");

	for (i = 0; i < adapter->priv_num; i++)
		kfree(adapter->priv[i]);

	kfree(adapter);

	return -1;
}

/*
 * This function unregisters the device and performs all the necessary
 * cleanups.
 *
 * The following cleanup operations are performed -
 *      - Free the timers
 *      - Free beacon buffers
 *      - Free private structures
 *      - Free adapter structure
 */
static int mwifiex_unregister(struct mwifiex_adapter *adapter)
{
	s32 i;

	if (adapter->if_ops.cleanup_if)
		adapter->if_ops.cleanup_if(adapter);

	del_timer_sync(&adapter->cmd_timer);

	/* Free private structures */
	for (i = 0; i < adapter->priv_num; i++) {
		if (adapter->priv[i]) {
			mwifiex_free_curr_bcn(adapter->priv[i]);
			kfree(adapter->priv[i]);
		}
	}

	if (adapter->nd_info) {
		for (i = 0 ; i < adapter->nd_info->n_matches ; i++)
			kfree(adapter->nd_info->matches[i]);
		kfree(adapter->nd_info);
		adapter->nd_info = NULL;
	}

	kfree(adapter);
	return 0;
}

void mwifiex_queue_main_work(struct mwifiex_adapter *adapter)
{
	unsigned long flags;

	spin_lock_irqsave(&adapter->main_proc_lock, flags);
	if (adapter->mwifiex_processing) {
		adapter->more_task_flag = true;
		spin_unlock_irqrestore(&adapter->main_proc_lock, flags);
	} else {
		spin_unlock_irqrestore(&adapter->main_proc_lock, flags);
		queue_work(adapter->workqueue, &adapter->main_work);
	}
}
EXPORT_SYMBOL_GPL(mwifiex_queue_main_work);

static void mwifiex_queue_rx_work(struct mwifiex_adapter *adapter)
{
	unsigned long flags;

	spin_lock_irqsave(&adapter->rx_proc_lock, flags);
	if (adapter->rx_processing) {
		spin_unlock_irqrestore(&adapter->rx_proc_lock, flags);
	} else {
		spin_unlock_irqrestore(&adapter->rx_proc_lock, flags);
		queue_work(adapter->rx_workqueue, &adapter->rx_work);
	}
}

static int mwifiex_process_rx(struct mwifiex_adapter *adapter)
{
	unsigned long flags;
	struct sk_buff *skb;
	struct mwifiex_rxinfo *rx_info;

	spin_lock_irqsave(&adapter->rx_proc_lock, flags);
	if (adapter->rx_processing || adapter->rx_locked) {
		spin_unlock_irqrestore(&adapter->rx_proc_lock, flags);
		goto exit_rx_proc;
	} else {
		adapter->rx_processing = true;
		spin_unlock_irqrestore(&adapter->rx_proc_lock, flags);
	}

	/* Check for Rx data */
	while ((skb = skb_dequeue(&adapter->rx_data_q))) {
		atomic_dec(&adapter->rx_pending);
		if (adapter->delay_main_work &&
		    (atomic_read(&adapter->rx_pending) < LOW_RX_PENDING)) {
			adapter->delay_main_work = false;
			mwifiex_queue_main_work(adapter);
		}

		rx_info = MWIFIEX_SKB_RXCB(skb);
		if (rx_info->buf_type == MWIFIEX_TYPE_AGGR_DATA) {
			if (adapter->if_ops.deaggr_pkt)
				adapter->if_ops.deaggr_pkt(adapter, skb);
			dev_kfree_skb_any(skb);
		} else {
			mwifiex_handle_rx_packet(adapter, skb);
		}
	}
	spin_lock_irqsave(&adapter->rx_proc_lock, flags);
	adapter->rx_processing = false;
	spin_unlock_irqrestore(&adapter->rx_proc_lock, flags);

exit_rx_proc:
	return 0;
}

/*
 * The main process.
 *
 * This function is the main procedure of the driver and handles various driver
 * operations. It runs in a loop and provides the core functionalities.
 *
 * The main responsibilities of this function are -
 *      - Ensure concurrency control
 *      - Handle pending interrupts and call interrupt handlers
 *      - Wake up the card if required
 *      - Handle command responses and call response handlers
 *      - Handle events and call event handlers
 *      - Execute pending commands
 *      - Transmit pending data packets
 */
int mwifiex_main_process(struct mwifiex_adapter *adapter)
{
	int ret = 0;
	unsigned long flags;
	struct sk_buff *skb;

	spin_lock_irqsave(&adapter->main_proc_lock, flags);

	/* Check if already processing */
	if (adapter->mwifiex_processing) {
		adapter->more_task_flag = true;
		spin_unlock_irqrestore(&adapter->main_proc_lock, flags);
		goto exit_main_proc;
	} else {
		adapter->mwifiex_processing = true;
	}
process_start:
	do {
		adapter->more_task_flag = false;
		spin_unlock_irqrestore(&adapter->main_proc_lock, flags);
		if ((adapter->hw_status == MWIFIEX_HW_STATUS_CLOSING) ||
		    (adapter->hw_status == MWIFIEX_HW_STATUS_NOT_READY))
			break;

		/* If we process interrupts first, it would increase RX pending
		 * even further. Avoid this by checking if rx_pending has
		 * crossed high threshold and schedule rx work queue
		 * and then process interrupts
		 */
		if (atomic_read(&adapter->rx_pending) >= HIGH_RX_PENDING) {
			adapter->delay_main_work = true;
			mwifiex_queue_rx_work(adapter);
			break;
		}

		/* Handle pending interrupt if any */
		if (adapter->int_status) {
			if (adapter->hs_activated)
				mwifiex_process_hs_config(adapter);
			if (adapter->if_ops.process_int_status)
				adapter->if_ops.process_int_status(adapter);
		}

		if (adapter->rx_work_enabled && adapter->data_received)
			mwifiex_queue_rx_work(adapter);

		/* Need to wake up the card ? */
		if ((adapter->ps_state == PS_STATE_SLEEP) &&
		    (adapter->pm_wakeup_card_req &&
		     !adapter->pm_wakeup_fw_try) &&
		    (is_command_pending(adapter) ||
		     !skb_queue_empty(&adapter->tx_data_q) ||
		     !mwifiex_wmm_lists_empty(adapter))) {
			adapter->pm_wakeup_fw_try = true;
			mod_timer(&adapter->wakeup_timer, jiffies + (HZ*3));
			adapter->if_ops.wakeup(adapter);
			spin_lock_irqsave(&adapter->main_proc_lock, flags);
			continue;
		}

		if (IS_CARD_RX_RCVD(adapter)) {
			adapter->data_received = false;
			adapter->pm_wakeup_fw_try = false;
			del_timer(&adapter->wakeup_timer);
			if (adapter->ps_state == PS_STATE_SLEEP) {
				if (adapter->iface_type == MWIFIEX_PCIE &&
				    adapter->if_ops.enable_int)
					adapter->if_ops.enable_int(adapter);
				adapter->ps_state = PS_STATE_AWAKE;
			}
		} else {
			/* We have tried to wakeup the card already */
			if (adapter->pm_wakeup_fw_try)
				break;
			if (adapter->ps_state != PS_STATE_AWAKE ||
			    adapter->tx_lock_flag)
				break;

			if ((!adapter->scan_chan_gap_enabled &&
			     adapter->scan_processing) || adapter->data_sent ||
			    (mwifiex_wmm_lists_empty(adapter) &&
			     skb_queue_empty(&adapter->tx_data_q))) {
				if (adapter->cmd_sent || adapter->curr_cmd ||
				    (!is_command_pending(adapter)))
					break;
			}
		}

		/* Check Rx data for USB */
		if (adapter->iface_type == MWIFIEX_USB)
			while ((skb = skb_dequeue(&adapter->usb_rx_data_q)))
				mwifiex_handle_rx_packet(adapter, skb);

		/* Check for event */
		if (adapter->event_received) {
			adapter->event_received = false;
			mwifiex_process_event(adapter);
		}

		/* Check for Cmd Resp */
		if (adapter->cmd_resp_received) {
			adapter->cmd_resp_received = false;
			mwifiex_process_cmdresp(adapter);

			/* call mwifiex back when init_fw is done */
			if (adapter->hw_status == MWIFIEX_HW_STATUS_INIT_DONE) {
				adapter->hw_status = MWIFIEX_HW_STATUS_READY;
				mwifiex_init_fw_complete(adapter);
			}
		}

		/* Check if we need to confirm Sleep Request
		   received previously */
		if (adapter->ps_state == PS_STATE_PRE_SLEEP) {
			if (!adapter->cmd_sent && !adapter->curr_cmd)
				mwifiex_check_ps_cond(adapter);
		}

		/* * The ps_state may have been changed during processing of
		 * Sleep Request event.
		 */
		if ((adapter->ps_state == PS_STATE_SLEEP) ||
		    (adapter->ps_state == PS_STATE_PRE_SLEEP) ||
		    (adapter->ps_state == PS_STATE_SLEEP_CFM) ||
		    adapter->tx_lock_flag){
			spin_lock_irqsave(&adapter->main_proc_lock, flags);
			continue;
		}

		if (!adapter->cmd_sent && !adapter->curr_cmd) {
			if (mwifiex_exec_next_cmd(adapter) == -1) {
				ret = -1;
				break;
			}
		}

		if ((adapter->scan_chan_gap_enabled ||
		     !adapter->scan_processing) &&
		    !adapter->data_sent &&
		    !skb_queue_empty(&adapter->tx_data_q)) {
			mwifiex_process_tx_queue(adapter);
			if (adapter->hs_activated) {
				adapter->is_hs_configured = false;
				mwifiex_hs_activated_event
					(mwifiex_get_priv
					(adapter, MWIFIEX_BSS_ROLE_ANY),
					false);
			}
		}

		if ((adapter->scan_chan_gap_enabled ||
		     !adapter->scan_processing) &&
		    !adapter->data_sent && !mwifiex_wmm_lists_empty(adapter)) {
			mwifiex_wmm_process_tx(adapter);
			if (adapter->hs_activated) {
				adapter->is_hs_configured = false;
				mwifiex_hs_activated_event
					(mwifiex_get_priv
					 (adapter, MWIFIEX_BSS_ROLE_ANY),
					 false);
			}
		}

		if (adapter->delay_null_pkt && !adapter->cmd_sent &&
		    !adapter->curr_cmd && !is_command_pending(adapter) &&
		    (mwifiex_wmm_lists_empty(adapter) &&
		     skb_queue_empty(&adapter->tx_data_q))) {
			if (!mwifiex_send_null_packet
			    (mwifiex_get_priv(adapter, MWIFIEX_BSS_ROLE_STA),
			     MWIFIEX_TxPD_POWER_MGMT_NULL_PACKET |
			     MWIFIEX_TxPD_POWER_MGMT_LAST_PACKET)) {
				adapter->delay_null_pkt = false;
				adapter->ps_state = PS_STATE_SLEEP;
			}
			break;
		}
		spin_lock_irqsave(&adapter->main_proc_lock, flags);
	} while (true);

	spin_lock_irqsave(&adapter->main_proc_lock, flags);
	if (adapter->more_task_flag)
		goto process_start;
	adapter->mwifiex_processing = false;
	spin_unlock_irqrestore(&adapter->main_proc_lock, flags);

exit_main_proc:
	if (adapter->hw_status == MWIFIEX_HW_STATUS_CLOSING)
		mwifiex_shutdown_drv(adapter);
	return ret;
}
EXPORT_SYMBOL_GPL(mwifiex_main_process);

/*
 * This function frees the adapter structure.
 *
 * Additionally, this closes the netlink socket, frees the timers
 * and private structures.
 */
static void mwifiex_free_adapter(struct mwifiex_adapter *adapter)
{
	if (!adapter) {
		pr_err("%s: adapter is NULL\n", __func__);
		return;
	}

	mwifiex_unregister(adapter);
	pr_debug("info: %s: free adapter\n", __func__);
}

/*
 * This function cancels all works in the queue and destroys
 * the main workqueue.
 */
static void mwifiex_terminate_workqueue(struct mwifiex_adapter *adapter)
{
	flush_workqueue(adapter->workqueue);
	destroy_workqueue(adapter->workqueue);
	adapter->workqueue = NULL;

	if (adapter->rx_workqueue) {
		flush_workqueue(adapter->rx_workqueue);
		destroy_workqueue(adapter->rx_workqueue);
		adapter->rx_workqueue = NULL;
	}
}

/*
 * This function gets firmware and initializes it.
 *
 * The main initialization steps followed are -
 *      - Download the correct firmware to card
 *      - Issue the init commands to firmware
 */
static void mwifiex_fw_dpc(const struct firmware *firmware, void *context)
{
	int ret;
	char fmt[64];
	struct mwifiex_private *priv;
	struct mwifiex_adapter *adapter = context;
	struct mwifiex_fw_image fw;
	struct semaphore *sem = adapter->card_sem;
	bool init_failed = false;
	struct wireless_dev *wdev;

	if (!firmware) {
		mwifiex_dbg(adapter, ERROR,
			    "Failed to get firmware %s\n",
			    adapter->fw_name);
		goto err_dnld_fw;
	}

	memset(&fw, 0, sizeof(struct mwifiex_fw_image));
	adapter->firmware = firmware;
	fw.fw_buf = (u8 *) adapter->firmware->data;
	fw.fw_len = adapter->firmware->size;

	if (adapter->if_ops.dnld_fw)
		ret = adapter->if_ops.dnld_fw(adapter, &fw);
	else
		ret = mwifiex_dnld_fw(adapter, &fw);
	if (ret == -1)
		goto err_dnld_fw;

	mwifiex_dbg(adapter, MSG, "WLAN FW is active\n");

	if (cal_data_cfg) {
		if ((request_firmware(&adapter->cal_data, cal_data_cfg,
				      adapter->dev)) < 0)
			mwifiex_dbg(adapter, ERROR,
				    "Cal data request_firmware() failed\n");
	}

	/* enable host interrupt after fw dnld is successful */
	if (adapter->if_ops.enable_int) {
		if (adapter->if_ops.enable_int(adapter))
			goto err_dnld_fw;
	}

	adapter->init_wait_q_woken = false;
	ret = mwifiex_init_fw(adapter);
	if (ret == -1) {
		goto err_init_fw;
	} else if (!ret) {
		adapter->hw_status = MWIFIEX_HW_STATUS_READY;
		goto done;
	}
	/* Wait for mwifiex_init to complete */
	if (!adapter->mfg_mode) {
		wait_event_interruptible(adapter->init_wait_q,
					 adapter->init_wait_q_woken);
		if (adapter->hw_status != MWIFIEX_HW_STATUS_READY)
			goto err_init_fw;
	}

	priv = adapter->priv[MWIFIEX_BSS_ROLE_STA];
	if (mwifiex_register_cfg80211(adapter)) {
		mwifiex_dbg(adapter, ERROR,
			    "cannot register with cfg80211\n");
		goto err_init_fw;
	}

	rtnl_lock();
	/* Create station interface by default */
	wdev = mwifiex_add_virtual_intf(adapter->wiphy, "mlan%d",
					NL80211_IFTYPE_STATION, NULL, NULL);
	if (IS_ERR(wdev)) {
		mwifiex_dbg(adapter, ERROR,
			    "cannot create default STA interface\n");
		rtnl_unlock();
		goto err_add_intf;
	}
	rtnl_unlock();

	mwifiex_drv_get_driver_version(adapter, fmt, sizeof(fmt) - 1);
	mwifiex_dbg(adapter, MSG, "driver_version = %s\n", fmt);
	goto done;

err_add_intf:
	wiphy_unregister(adapter->wiphy);
	wiphy_free(adapter->wiphy);
err_init_fw:
	if (adapter->if_ops.disable_int)
		adapter->if_ops.disable_int(adapter);
err_dnld_fw:
	mwifiex_dbg(adapter, ERROR,
		    "info: %s: unregister device\n", __func__);
	if (adapter->if_ops.unregister_dev)
		adapter->if_ops.unregister_dev(adapter);

	if ((adapter->hw_status == MWIFIEX_HW_STATUS_FW_READY) ||
	    (adapter->hw_status == MWIFIEX_HW_STATUS_READY)) {
		mwifiex_dbg(adapter, INFO,
			    "info: %s: shutdown mwifiex\n", __func__);
		adapter->init_wait_q_woken = false;

		if (mwifiex_shutdown_drv(adapter) == -EINPROGRESS)
			wait_event_interruptible(adapter->init_wait_q,
						 adapter->init_wait_q_woken);
	}
	adapter->surprise_removed = true;
	mwifiex_terminate_workqueue(adapter);
	init_failed = true;
done:
	if (adapter->cal_data) {
		release_firmware(adapter->cal_data);
		adapter->cal_data = NULL;
	}
	if (adapter->firmware) {
		release_firmware(adapter->firmware);
		adapter->firmware = NULL;
	}
	if (init_failed)
		mwifiex_free_adapter(adapter);
	up(sem);
	return;
}

/*
 * This function initializes the hardware and gets firmware.
 */
static int mwifiex_init_hw_fw(struct mwifiex_adapter *adapter)
{
	int ret;

	if (mfg_mode && !strlen(mfg_firmware)) {
		pr_err("%s: mfg firmware missing. Ignoring manufacturing mode\n",
		       __func__);
		mfg_mode = false;
	}

	/* Override default firmware with manufacturing one if
	 * manufacturing mode is enabled
	 */
	if (mfg_mode) {
		if (strlcpy(adapter->fw_name, mfg_firmware,
			    sizeof(adapter->fw_name)) >=
			    sizeof(adapter->fw_name)) {
			pr_err("%s: fw_name too long!\n", __func__);
			return -1;
		}
	}

	ret = request_firmware_nowait(THIS_MODULE, 1, adapter->fw_name,
				      adapter->dev, GFP_KERNEL, adapter,
				      mwifiex_fw_dpc);
	if (ret < 0)
		mwifiex_dbg(adapter, ERROR,
			    "request_firmware_nowait() returned error %d\n",
			    ret);
	return ret;
}

/*
 * CFG802.11 network device handler for open.
 *
 * Starts the data queue.
 */
static int
mwifiex_open(struct net_device *dev)
{
	netif_carrier_off(dev);

	return 0;
}

/*
 * CFG802.11 network device handler for close.
 */
static int
mwifiex_close(struct net_device *dev)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);

	if (priv->scan_request) {
		mwifiex_dbg(priv->adapter, INFO,
			    "aborting scan on ndo_stop\n");
		cfg80211_scan_done(priv->scan_request, 1);
		priv->scan_request = NULL;
		priv->scan_aborting = true;
	}

	if (priv->sched_scanning) {
		mwifiex_dbg(priv->adapter, INFO,
			    "aborting bgscan on ndo_stop\n");
		mwifiex_stop_bg_scan(priv);
		cfg80211_sched_scan_stopped(priv->wdev.wiphy);
	}

	netif_tx_stop_all_queues(dev);
	return 0;
}

/*
 * Add buffer into wmm tx queue and queue work to transmit it.
 */
int mwifiex_queue_tx_pkt(struct mwifiex_private *priv, struct sk_buff *skb)
{
	struct netdev_queue *txq;
	int index = mwifiex_1d_to_wmm_queue[skb->priority];

	if (atomic_inc_return(&priv->wmm_tx_pending[index]) >= MAX_TX_PENDING) {
		txq = netdev_get_tx_queue(priv->netdev, index);
		if (!netif_tx_queue_stopped(txq)) {
			netif_tx_stop_queue(txq);
			mwifiex_dbg(priv->adapter, INFO,
				    "stop queue: %d\n", index);
		}
	}

	atomic_inc(&priv->adapter->tx_pending);
	mwifiex_wmm_add_buf_txqueue(priv, skb);

	mwifiex_queue_main_work(priv->adapter);

	return 0;
}

/*
 * CFG802.11 network device handler for data transmission.
 */
static int
mwifiex_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);
	struct sk_buff *new_skb;
	struct mwifiex_txinfo *tx_info;

	mwifiex_dbg(priv->adapter, DATA,
		    "data: %lu BSS(%d-%d): Data <= kernel\n",
		    jiffies, priv->bss_type, priv->bss_num);

	if (priv->adapter->surprise_removed) {
		kfree_skb(skb);
		priv->stats.tx_dropped++;
		return 0;
	}
	if (!skb->len || (skb->len > ETH_FRAME_LEN)) {
		mwifiex_dbg(priv->adapter, ERROR,
			    "Tx: bad skb len %d\n", skb->len);
		kfree_skb(skb);
		priv->stats.tx_dropped++;
		return 0;
	}
	if (skb_headroom(skb) < MWIFIEX_MIN_DATA_HEADER_LEN) {
		mwifiex_dbg(priv->adapter, DATA,
			    "data: Tx: insufficient skb headroom %d\n",
			    skb_headroom(skb));
		/* Insufficient skb headroom - allocate a new skb */
		new_skb =
			skb_realloc_headroom(skb, MWIFIEX_MIN_DATA_HEADER_LEN);
		if (unlikely(!new_skb)) {
			mwifiex_dbg(priv->adapter, ERROR,
				    "Tx: cannot alloca new_skb\n");
			kfree_skb(skb);
			priv->stats.tx_dropped++;
			return 0;
		}
		kfree_skb(skb);
		skb = new_skb;
		mwifiex_dbg(priv->adapter, INFO,
			    "info: new skb headroomd %d\n",
			    skb_headroom(skb));
	}

	tx_info = MWIFIEX_SKB_TXCB(skb);
	memset(tx_info, 0, sizeof(*tx_info));
	tx_info->bss_num = priv->bss_num;
	tx_info->bss_type = priv->bss_type;
	tx_info->pkt_len = skb->len;

	/* Record the current time the packet was queued; used to
	 * determine the amount of time the packet was queued in
	 * the driver before it was sent to the firmware.
	 * The delay is then sent along with the packet to the
	 * firmware for aggregate delay calculation for stats and
	 * MSDU lifetime expiry.
	 */
	__net_timestamp(skb);

	mwifiex_queue_tx_pkt(priv, skb);

	return 0;
}

/*
 * CFG802.11 network device handler for setting MAC address.
 */
static int
mwifiex_set_mac_address(struct net_device *dev, void *addr)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);
	struct sockaddr *hw_addr = addr;
	int ret;

	memcpy(priv->curr_addr, hw_addr->sa_data, ETH_ALEN);

	/* Send request to firmware */
	ret = mwifiex_send_cmd(priv, HostCmd_CMD_802_11_MAC_ADDRESS,
			       HostCmd_ACT_GEN_SET, 0, NULL, true);

	if (!ret)
		memcpy(priv->netdev->dev_addr, priv->curr_addr, ETH_ALEN);
	else
		mwifiex_dbg(priv->adapter, ERROR,
			    "set mac address failed: ret=%d\n", ret);

	memcpy(dev->dev_addr, priv->curr_addr, ETH_ALEN);

	return ret;
}

/*
 * CFG802.11 network device handler for setting multicast list.
 */
static void mwifiex_set_multicast_list(struct net_device *dev)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);
	struct mwifiex_multicast_list mcast_list;

	if (dev->flags & IFF_PROMISC) {
		mcast_list.mode = MWIFIEX_PROMISC_MODE;
	} else if (dev->flags & IFF_ALLMULTI ||
		   netdev_mc_count(dev) > MWIFIEX_MAX_MULTICAST_LIST_SIZE) {
		mcast_list.mode = MWIFIEX_ALL_MULTI_MODE;
	} else {
		mcast_list.mode = MWIFIEX_MULTICAST_MODE;
		mcast_list.num_multicast_addr =
			mwifiex_copy_mcast_addr(&mcast_list, dev);
	}
	mwifiex_request_set_multicast_list(priv, &mcast_list);
}

/*
 * CFG802.11 network device handler for transmission timeout.
 */
static void
mwifiex_tx_timeout(struct net_device *dev)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);

	priv->num_tx_timeout++;
	priv->tx_timeout_cnt++;
	mwifiex_dbg(priv->adapter, ERROR,
		    "%lu : Tx timeout(#%d), bss_type-num = %d-%d\n",
		    jiffies, priv->tx_timeout_cnt, priv->bss_type,
		    priv->bss_num);
	mwifiex_set_trans_start(dev);

	if (priv->adapter->if_ops.read_regs)
		priv->adapter->if_ops.read_regs(priv->adapter);

	if (priv->tx_timeout_cnt > TX_TIMEOUT_THRESHOLD &&
	    priv->adapter->if_ops.card_reset) {
		mwifiex_dbg(priv->adapter, ERROR,
			    "tx_timeout_cnt exceeds threshold.\t"
			    "Triggering card reset!\n");
		priv->adapter->if_ops.card_reset(priv->adapter);
	}
}

/*
 * CFG802.11 network device handler for statistics retrieval.
 */
static struct net_device_stats *mwifiex_get_stats(struct net_device *dev)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);

	return &priv->stats;
}

static u16
mwifiex_netdev_select_wmm_queue(struct net_device *dev, struct sk_buff *skb,
				void *accel_priv, select_queue_fallback_t fallback)
{
	skb->priority = cfg80211_classify8021d(skb, NULL);
	return mwifiex_1d_to_wmm_queue[skb->priority];
}

static int mwifiex_do_ioctl(struct net_device *dev, struct ifreq *req, int cmd)
{
	struct mwifiex_private *priv = mwifiex_netdev_get_priv(dev);
	struct iwreq *wrq = (struct iwreq *)req;
	int ret;

	if (!priv->adapter->mfg_mode)
		return -EINVAL;

	dev_dbg(priv->adapter->dev, "ioctl cmd = 0x%x\n", cmd);

	switch (cmd) {
	case MWIFIEX_HOSTCMD_IOCTL:
		ret = mwifiex_process_host_command(priv, wrq);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* Network device handlers */
static const struct net_device_ops mwifiex_netdev_ops = {
	.ndo_open = mwifiex_open,
	.ndo_stop = mwifiex_close,
	.ndo_start_xmit = mwifiex_hard_start_xmit,
	.ndo_set_mac_address = mwifiex_set_mac_address,
	.ndo_do_ioctl = mwifiex_do_ioctl,
	.ndo_tx_timeout = mwifiex_tx_timeout,
	.ndo_get_stats = mwifiex_get_stats,
	.ndo_set_rx_mode = mwifiex_set_multicast_list,
	.ndo_select_queue = mwifiex_netdev_select_wmm_queue,
};

static const struct iw_priv_args mwifiex_iwpriv_args[] = {
	{	MWIFIEX_HOSTCMD_IOCTL,
		IW_PRIV_TYPE_BYTE | 2047,
		IW_PRIV_TYPE_BYTE | 2047,
		"hostcmd"
	},
};

static struct iw_handler_def mwifiex_iwpriv_handler_def = {
	.num_private_args = ARRAY_SIZE(mwifiex_iwpriv_args),
	.private_args = (struct iw_priv_args *)mwifiex_iwpriv_args,
};

/*
 * This function initializes the private structure parameters.
 *
 * The following wait queues are initialized -
 *      - IOCTL wait queue
 *      - Command wait queue
 *      - Statistics wait queue
 *
 * ...and the following default parameters are set -
 *      - Current key index     : Set to 0
 *      - Rate index            : Set to auto
 *      - Media connected       : Set to disconnected
 *      - Adhoc link sensed     : Set to false
 *      - Nick name             : Set to null
 *      - Number of Tx timeout  : Set to 0
 *      - Device address        : Set to current address
 *
 * In addition, the CFG80211 work queue is also created.
 */
void mwifiex_init_priv_params(struct mwifiex_private *priv,
						struct net_device *dev)
{
	dev->netdev_ops = &mwifiex_netdev_ops;
	dev->destructor = free_netdev;
	dev->wireless_handlers = &mwifiex_iwpriv_handler_def;
	/* Initialize private structure */
	priv->current_key_index = 0;
	priv->media_connected = false;
	memset(&priv->nick_name, 0, sizeof(priv->nick_name));
	memset(priv->mgmt_ie, 0,
	       sizeof(struct mwifiex_ie) * MAX_MGMT_IE_INDEX);
	priv->beacon_idx = MWIFIEX_AUTO_IDX_MASK;
	priv->proberesp_idx = MWIFIEX_AUTO_IDX_MASK;
	priv->assocresp_idx = MWIFIEX_AUTO_IDX_MASK;
	priv->rsn_idx = MWIFIEX_AUTO_IDX_MASK;
	priv->num_tx_timeout = 0;
	memcpy(dev->dev_addr, priv->curr_addr, ETH_ALEN);
}

/*
 * This function check if command is pending.
 */
int is_command_pending(struct mwifiex_adapter *adapter)
{
	unsigned long flags;
	int is_cmd_pend_q_empty;

	spin_lock_irqsave(&adapter->cmd_pending_q_lock, flags);
	is_cmd_pend_q_empty = list_empty(&adapter->cmd_pending_q);
	spin_unlock_irqrestore(&adapter->cmd_pending_q_lock, flags);

	return !is_cmd_pend_q_empty;
}

/*
 * This is the RX work queue function.
 *
 * It handles the RX operations.
 */
static void mwifiex_rx_work_queue(struct work_struct *work)
{
	struct mwifiex_adapter *adapter =
		container_of(work, struct mwifiex_adapter, rx_work);

	if (adapter->surprise_removed)
		return;
	mwifiex_process_rx(adapter);
}

/*
 * This is the main work queue function.
 *
 * It handles the main process, which in turn handles the complete
 * driver operations.
 */
static void mwifiex_main_work_queue(struct work_struct *work)
{
	struct mwifiex_adapter *adapter =
		container_of(work, struct mwifiex_adapter, main_work);

	if (adapter->surprise_removed)
		return;
	mwifiex_main_process(adapter);
}

/*
 * This function adds the card.
 *
 * This function follows the following major steps to set up the device -
 *      - Initialize software. This includes probing the card, registering
 *        the interface operations table, and allocating/initializing the
 *        adapter structure
 *      - Set up the netlink socket
 *      - Create and start the main work queue
 *      - Register the device
 *      - Initialize firmware and hardware
 *      - Add logical interfaces
 */
int
mwifiex_add_card(void *card, struct semaphore *sem,
		 struct mwifiex_if_ops *if_ops, u8 iface_type)
{
	struct mwifiex_adapter *adapter;

	if (down_interruptible(sem))
		goto exit_sem_err;

	if (mwifiex_register(card, if_ops, (void **)&adapter)) {
		pr_err("%s: software init failed\n", __func__);
		goto err_init_sw;
	}

	adapter->iface_type = iface_type;
	adapter->card_sem = sem;

	adapter->hw_status = MWIFIEX_HW_STATUS_INITIALIZING;
	adapter->surprise_removed = false;
	init_waitqueue_head(&adapter->init_wait_q);
	adapter->is_suspended = false;
	adapter->hs_activated = false;
	init_waitqueue_head(&adapter->hs_activate_wait_q);
	init_waitqueue_head(&adapter->cmd_wait_q.wait);
	adapter->cmd_wait_q.status = 0;
	adapter->scan_wait_q_woken = false;

	if (num_possible_cpus() > 1) {
		adapter->rx_work_enabled = true;
		pr_notice("rx work enabled, cpus %d\n", num_possible_cpus());
	}

	adapter->workqueue =
		alloc_workqueue("MWIFIEX_WORK_QUEUE",
				WQ_HIGHPRI | WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!adapter->workqueue)
		goto err_kmalloc;

	INIT_WORK(&adapter->main_work, mwifiex_main_work_queue);

	if (adapter->rx_work_enabled) {
		adapter->rx_workqueue = alloc_workqueue("MWIFIEX_RX_WORK_QUEUE",
							WQ_HIGHPRI |
							WQ_MEM_RECLAIM |
							WQ_UNBOUND, 1);
		if (!adapter->rx_workqueue)
			goto err_kmalloc;

		INIT_WORK(&adapter->rx_work, mwifiex_rx_work_queue);
	}

	/* Register the device. Fill up the private data structure with relevant
	   information from the card. */
	if (adapter->if_ops.register_dev(adapter)) {
		pr_err("%s: failed to register mwifiex device\n", __func__);
		goto err_registerdev;
	}

	if (mwifiex_init_hw_fw(adapter)) {
		mwifiex_dbg(adapter, ERROR,
			    "%s: firmware init failed\n", __func__);
		goto err_init_fw;
	}

	return 0;

err_init_fw:
	mwifiex_dbg(adapter, INFO,
		    "info: unregister device\n");
	if (adapter->if_ops.unregister_dev)
		adapter->if_ops.unregister_dev(adapter);
	if ((adapter->hw_status == MWIFIEX_HW_STATUS_FW_READY) ||
	    (adapter->hw_status == MWIFIEX_HW_STATUS_READY)) {
		pr_debug("info: %s: shutdown mwifiex\n", __func__);
		adapter->init_wait_q_woken = false;

		if (mwifiex_shutdown_drv(adapter) == -EINPROGRESS)
			wait_event_interruptible(adapter->init_wait_q,
						 adapter->init_wait_q_woken);
	}
err_registerdev:
	adapter->surprise_removed = true;
	mwifiex_terminate_workqueue(adapter);
err_kmalloc:
	mwifiex_free_adapter(adapter);

err_init_sw:
	up(sem);

exit_sem_err:
	return -1;
}
EXPORT_SYMBOL_GPL(mwifiex_add_card);

/*
 * This function removes the card.
 *
 * This function follows the following major steps to remove the device -
 *      - Stop data traffic
 *      - Shutdown firmware
 *      - Remove the logical interfaces
 *      - Terminate the work queue
 *      - Unregister the device
 *      - Free the adapter structure
 */
int mwifiex_remove_card(struct mwifiex_adapter *adapter, struct semaphore *sem)
{
	struct mwifiex_private *priv = NULL;
	int i;

	if (down_interruptible(sem))
		goto exit_sem_err;

	if (!adapter)
		goto exit_remove;

	/* We can no longer handle interrupts once we start doing the teardown
	 * below. */
	if (adapter->if_ops.disable_int)
		adapter->if_ops.disable_int(adapter);

	adapter->surprise_removed = true;

	mwifiex_terminate_workqueue(adapter);

	/* Stop data */
	for (i = 0; i < adapter->priv_num; i++) {
		priv = adapter->priv[i];
		if (priv && priv->netdev) {
			mwifiex_stop_net_dev_queue(priv->netdev, adapter);
			if (netif_carrier_ok(priv->netdev))
				netif_carrier_off(priv->netdev);
		}
	}

	mwifiex_dbg(adapter, CMD,
		    "cmd: calling mwifiex_shutdown_drv...\n");
	adapter->init_wait_q_woken = false;

	if (mwifiex_shutdown_drv(adapter) == -EINPROGRESS)
		wait_event_interruptible(adapter->init_wait_q,
					 adapter->init_wait_q_woken);
	mwifiex_dbg(adapter, CMD,
		    "cmd: mwifiex_shutdown_drv done\n");
	if (atomic_read(&adapter->rx_pending) ||
	    atomic_read(&adapter->tx_pending) ||
	    atomic_read(&adapter->cmd_pending)) {
		mwifiex_dbg(adapter, ERROR,
			    "rx_pending=%d, tx_pending=%d,\t"
			    "cmd_pending=%d\n",
			    atomic_read(&adapter->rx_pending),
			    atomic_read(&adapter->tx_pending),
			    atomic_read(&adapter->cmd_pending));
	}

	for (i = 0; i < adapter->priv_num; i++) {
		priv = adapter->priv[i];

		if (!priv)
			continue;

		rtnl_lock();
		if (priv->netdev &&
		    priv->wdev.iftype != NL80211_IFTYPE_UNSPECIFIED)
			mwifiex_del_virtual_intf(adapter->wiphy, &priv->wdev);
		rtnl_unlock();
	}

	wiphy_unregister(adapter->wiphy);
	wiphy_free(adapter->wiphy);

	/* Unregister device */
	dev_dbg(adapter->dev, "info: unregister device\n");
	if (adapter->if_ops.unregister_dev)
		adapter->if_ops.unregister_dev(adapter);
	/* Free adapter structure */
	mwifiex_dbg(adapter, INFO,
		    "info: free adapter\n");
	mwifiex_free_adapter(adapter);

exit_remove:
	up(sem);
exit_sem_err:
	return 0;
}
EXPORT_SYMBOL_GPL(mwifiex_remove_card);

/*
 * This function initializes the module.
 *
 * The debug FS is also initialized if configured.
 */
static int
mwifiex_init_module(void)
{
#ifdef CONFIG_DEBUG_FS
	mwifiex_debugfs_init();
#endif
	return 0;
}

/*
 * This function cleans up the module.
 *
 * The debug FS is removed if available.
 */
static void
mwifiex_cleanup_module(void)
{
#ifdef CONFIG_DEBUG_FS
	mwifiex_debugfs_remove();
#endif
}

module_init(mwifiex_init_module);
module_exit(mwifiex_cleanup_module);

MODULE_AUTHOR("Marvell International Ltd.");
MODULE_DESCRIPTION("Marvell WiFi-Ex Driver version " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL v2");
