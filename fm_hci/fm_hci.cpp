/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *        * Redistributions of source code must retain the above copyright
 *            notice, this list of conditions and the following disclaimer.
 *        * Redistributions in binary form must reproduce the above
 *            copyright notice, this list of conditions and the following
 *            disclaimer in the documentation and/or other materials provided
 *            with the distribution.
 *        * Neither the name of The Linux Foundation nor the names of its
 *            contributors may be used to endorse or promote products derived
 *            from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************
 *
 *  This file contains main functions to support FM HCI interface to send
 *  commands and  recieved events.
 *
 *****************************************************************************/

#define LOG_TAG "fm_hci"

#include <queue>          // std::queue
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <cstdlib>
#include <thread>

#include <utils/Log.h>
#include <unistd.h>

#include <vendor/qti/hardware/fm/1.0/IFmHci.h>
#include <vendor/qti/hardware/fm/1.0/IFmHciCallbacks.h>
#include <vendor/qti/hardware/fm/1.0/types.h>
#include "fm_hci.h"

#include <hwbinder/ProcessState.h>

using vendor::qti::hardware::fm::V1_0::IFmHci;
using vendor::qti::hardware::fm::V1_0::IFmHciCallbacks;
using vendor::qti::hardware::fm::V1_0::HciPacket;
using vendor::qti::hardware::fm::V1_0::Status;
using android::hardware::ProcessState;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;

static struct fm_hci_t hci;

typedef std::unique_lock<std::mutex> Lock;
android::sp<IFmHci> fmHci;

static int enqueue_fm_rx_event(struct fm_event_header_t *hdr);
static void dequeue_fm_rx_event();
static int enqueue_fm_tx_cmd(struct fm_command_header_t *hdr);
static void dequeue_fm_tx_cmd();
static void  hci_tx_thread();
static void hci_rx_thread();
static int start_tx_thread();
static void stop_tx_thread();
static int start_rx_thread();
static void stop_rx_thread();
static void cleanup_threads();
static bool hci_initialize();
static void hci_transmit(struct fm_command_header_t *hdr);
static void hci_close();

/*******************************************************************************
**
** Function         enqueue_fm_rx_event
**
** Description      This function is called in the hal daemon context to queue
**                  FM events in RX queue.
**
** Parameters:      hdr - contains the fm event header pointer
**
**
** Returns          int
**
*******************************************************************************/
static int enqueue_fm_rx_event(struct fm_event_header_t *hdr)
{

    hci.rx_queue_mtx.lock();
    hci.rx_event_queue.push(hdr);
    hci.rx_queue_mtx.unlock();

    if (hci.is_rx_processing == false) {
        hci.rx_cond.notify_all();
    }

    ALOGI("%s: FM-Event ENQUEUED SUCCESSFULLY", __func__);

    return FM_HC_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         dequeue_fm_rx_event
**
** Description      This function is called in the rx thread context to dequeue
**                  FM events from RX queue & processing the FM event.
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void dequeue_fm_rx_event()
{
    fm_event_header_t *evt_buf;

    ALOGI("%s", __func__);
    while (1) {
        hci.rx_queue_mtx.lock();
        if (hci.rx_event_queue.empty()) {
            ALOGI("No more FM Events are available in the RX Queue");
            hci.is_rx_processing = false;
            hci.rx_queue_mtx.unlock();
            return;
        } else {
            hci.is_rx_processing = true;
        }

        evt_buf = hci.rx_event_queue.front();
        hci.rx_event_queue.pop();
        hci.rx_queue_mtx.unlock();

        hci.credit_mtx.lock();
        if (evt_buf->evt_code == FM_CMD_COMPLETE) {
            ALOGI("%s: %d Credits got from the SOC", __func__, evt_buf->params[0]);
            hci.command_credits += evt_buf->params[0];
            hci.cmd_credits_cond.notify_all();
        } else if (evt_buf->evt_code == FM_CMD_STATUS) {
            ALOGI("%s: %d Credits got from the SOC", __func__, evt_buf->params[1]);
            hci.command_credits += evt_buf->params[1];
            hci.cmd_credits_cond.notify_all();
        } else if (evt_buf->evt_code == FM_HW_ERR_EVENT) {
            ALOGI("%s: FM H/w Err Event Recvd. Event Code: 0x%x", __func__, evt_buf->evt_code);
        } else {
            ALOGE("%s: Not CS/CC Event: Recvd. Event Code: 0x%x", __func__, evt_buf->evt_code);
        }

        hci.credit_mtx.unlock();
        if (hci.cb && hci.cb->process_event) {
            ALOGI("%s: processing the event", __func__);
            hci.cb->process_event(NULL, (uint8_t *)evt_buf);
        }

        free(evt_buf);
        evt_buf = NULL;
    }

}

/*******************************************************************************
**
** Function         enqueue_fm_tx_cmd
**
** Description      This function is called in the application JNI context to
**                  queue FM commands in TX queue.
**
** Parameters:      hdr - contains the fm command header pointer
**
**
** Returns          int
**
*******************************************************************************/
static int enqueue_fm_tx_cmd(struct fm_command_header_t *hdr)
{
    ALOGI("%s:  opcode 0x%x len:%d", __func__,  hdr->opcode, hdr->len);

    hci.tx_queue_mtx.lock();
    hci.tx_cmd_queue.push(hdr);
    hci.tx_queue_mtx.unlock();

    if (hci.is_tx_processing == false) {
        hci.tx_cond.notify_all();
    }

    ALOGI("%s: FM-CMD ENQUEUED SUCCESSFULLY", __func__);

    return FM_HC_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         dequeue_fm_tx_cmd
**
** Description      This function is called in the tx thread context to dequeue
**                  & transmitting FM command to to HAL daemon.
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void dequeue_fm_tx_cmd()
{
    fm_command_header_t *hdr;

    ALOGI("%s", __func__);

    while (1) {
        hci.tx_queue_mtx.lock();
        if(hci.tx_cmd_queue.empty()){
            ALOGI("No more FM CMDs are available in the Queue");
            hci.is_tx_processing = false;
            hci.tx_queue_mtx.unlock();
            return;
        } else {
            hci.is_tx_processing = true;
        }

        hdr = hci.tx_cmd_queue.front();
        hci.tx_cmd_queue.pop();
        hci.tx_queue_mtx.unlock();

        Lock lk(hci.credit_mtx);
        while (hci.command_credits == 0) {
            ALOGI("%s: waiting for credits", __func__);
            hci.cmd_credits_cond.wait(lk);
            ALOGI("%s: %d Credits Remaining", __func__, hci.command_credits);
            if (hci.command_credits) {
                 break;
            }
        }
        hci.command_credits--;
        hci_transmit(hdr);
    }
}


/*******************************************************************************
**
** Function         hci_tx_thread
**
** Description      This function is main function of tx worker thread.
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void  hci_tx_thread()
{
    ALOGI("%s: ##### starting hci_tx_thread Worker thread!!! #####", __func__);
    hci.is_tx_thread_running = true;

    while (hci.state != FM_RADIO_DISABLING && hci.state != FM_RADIO_DISABLED) {
        //wait  for tx cmd
        Lock lk(hci.tx_cond_mtx);
        hci.tx_cond.wait(lk);
        ALOGV("%s: dequeueing the tx cmd!!!" , __func__);
        dequeue_fm_tx_cmd();
    }

    hci.is_tx_thread_running =false;
    ALOGI("%s: ##### Exiting hci_tx_thread Worker thread!!! #####", __func__);
}

/*******************************************************************************
**
** Function         hci_rx_thread
**
** Description      This function is main function of tx worker thread.
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void hci_rx_thread()
{

    ALOGI("%s: ##### starting hci_rx_thread Worker thread!!! #####", __func__);
    hci.is_rx_thread_running = true;

    while (hci.state != FM_RADIO_DISABLING && hci.state != FM_RADIO_DISABLED) {
        //wait for rx event
        Lock lk(hci.rx_cond_mtx);
        hci.rx_cond.wait(lk);
        dequeue_fm_rx_event();
    }

    hci.is_rx_thread_running = false;
    ALOGI("%s: ##### Exiting hci_rx_thread Worker thread!!! #####", __func__);
}

/*******************************************************************************
**
** Function         start_tx_thread
**
** Description      This function is called to start tx worker thread.
**
** Parameters:      void
**
**
** Returns          int
**
*******************************************************************************/
static int start_tx_thread()
{

    ALOGI("FM-HCI: Creating the FM-HCI  TX TASK...");
    hci.tx_thread_ = std::thread(hci_tx_thread);
    if (!hci.tx_thread_.joinable()) {
        ALOGE("tx thread is not joinable");
        return FM_HC_STATUS_FAIL;
    }

    return FM_HC_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         stop_tx_thread
**
** Description      This function is called to stop tx worker thread.
**
** Parameters:      void
**
**
** Returns          int
**
*******************************************************************************/
static void stop_tx_thread()
{
    int ret;

    ALOGI("%s:stop_tx_thread ++", __func__);
    if (hci.is_tx_processing == false) {
        hci.tx_cond.notify_all();
    }

    hci.tx_thread_.join();
    ALOGI("%s:stop_tx_thread --", __func__);
}

/*******************************************************************************
**
** Function         start_rx_thread
**
** Description      This function is called to start rx worker thread.
**
** Parameters:      void
**
**
** Returns          int
**
*******************************************************************************/
static int start_rx_thread()
{
    int ret = FM_HC_STATUS_SUCCESS;
    ALOGI("FM-HCI: Creating the FM-HCI RX TASK...");

    hci.rx_thread_ = std::thread(hci_rx_thread);
    if (!hci.rx_thread_.joinable()) {
        ALOGE("rx thread is not joinable");
        return FM_HC_STATUS_FAIL;
    }

    return ret;
}

/*******************************************************************************
**
** Function         stop_rx_thread
**
** Description      This function is called to stop rx worker thread.
**
** Parameters:      void
**
**
** Returns          int
**
*******************************************************************************/
static void stop_rx_thread()
{
    ALOGI("%s:stop_rx_thread ++", __func__);
    if (hci.is_rx_processing == false) {
        hci.rx_cond.notify_all();
    }

    hci.rx_thread_.join();
    ALOGI("%s:stop_rx_thread --", __func__);
}

/*******************************************************************************
**
** Function         cleanup_threads
**
** Description      This function is called to cleanup rx & tx worker thread.
**
** Parameters:      void
**
**
** Returns          int
**
*******************************************************************************/
static void cleanup_threads()
{
    stop_rx_thread();
    stop_tx_thread();
}

/*******************************************************************************
**
** Function         initialization_complete
**
** Description      This function is called, when initialization complete
**                  callback is called by hal daemon.
**
** Parameters:      hdr - contains the fm event header pointer
**
**
** Returns          int
**
*******************************************************************************/
static void initialization_complete(bool is_hci_initialize)
{
    int ret;
    ALOGI("++%s: is_hci_initialize: %d", __func__, is_hci_initialize);

    while (is_hci_initialize) {
        ret = start_tx_thread();
        if (ret)
        {
            cleanup_threads();
            hci.state = FM_RADIO_DISABLING;
            break;
        }

        ret = start_rx_thread();
        if (ret)
        {
            cleanup_threads();
            hci.state = FM_RADIO_DISABLING;
            break;
        }

        hci.state = FM_RADIO_ENABLED;
        break;
    }

    hci.on_cond.notify_all();
    ALOGI("--%s: is_hci_initialize: %d", __func__, is_hci_initialize);

}

/*******************************************************************************
**
** Class            FmHciCallbacks
**
** Description      This is main class, which has the implemention for FM HCI
**                  callback functions.
**
** Member callback Functions:      initializationComplete, hciEventReceived
**
**
** Returns          int
**
*******************************************************************************/
class FmHciCallbacks : public IFmHciCallbacks {
    public:
        FmHciCallbacks() {
        };
        virtual ~FmHciCallbacks() = default;

        Return<void> initializationComplete(Status status) {
            if(status == Status::SUCCESS)
            {
                initialization_complete(true);
            } else {
                initialization_complete(false);
            }

            return Void();
        }

        Return<void> hciEventReceived(const hidl_vec<uint8_t>& event) {
            struct fm_event_header_t *temp = (struct fm_event_header_t *) malloc(event.size());
            if(temp) {
                memcpy(temp, event.data(), event.size());
                ALOGI("%s: evt_code:  0x%x", __func__, temp->evt_code);
                enqueue_fm_rx_event(temp);
            }
            else {
                ALOGE("%s: Memory Allocation failed for event buffer ",__func__);
            }
            return Void();
        }
};

/*******************************************************************************
**
** Function         hci_initialize
**
** Description      This function is used to initialize fm hci hidl transport.
**                  It makes a binder call to hal daemon
**
** Parameters:      void
**
**
** Returns          bool
**
*******************************************************************************/
static bool hci_initialize()
{
    ALOGI("%s", __func__);

    fmHci = IFmHci::getService();

    if (fmHci != nullptr) {
        hci.state = FM_RADIO_ENABLING;
        android::sp<IFmHciCallbacks> callbacks = new FmHciCallbacks();
        fmHci->initialize(callbacks);
        return true;
    } else {
        return false;
    }
}

/*******************************************************************************
**
** Function         hci_transmit
**
** Description      This function is used to send fm command to fm hci hidl transport.
**                  It makes a binder call to hal daemon.
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void hci_transmit(struct fm_command_header_t *hdr) {
    HciPacket data;

    ALOGI("%s: opcode 0x%x len:%d", __func__, hdr->opcode, hdr->len);

    if (fmHci != nullptr) {
        data.setToExternal((uint8_t *)hdr, 3 + hdr->len);
        fmHci->sendHciCommand(data);
    } else {
        ALOGI("%s: fmHci is NULL", __func__);
    }

    free(hdr);
}

/*******************************************************************************
**
** Function         hci_close
**
** Description      This function is used to close fm hci hidl transport.
**                  It makes a binder call to hal daemon
**
** Parameters:      void
**
**
** Returns          void
**
*******************************************************************************/
static void hci_close()
{
    ALOGI("%s", __func__);

    if (fmHci != nullptr) {
        fmHci->close();
        fmHci = nullptr;
    }
}

/*******************************************************************************
**
** Function         fm_hci_init
**
** Description      This function is used to intialize fm hci
**
** Parameters:     hci_hal - contains the fm helium hal hci pointer
**
**
** Returns          void
**
*******************************************************************************/
int fm_hci_init(fm_hci_hal_t *hci_hal)
{
    int ret = FM_HC_STATUS_FAIL;

    ALOGD("++%s", __func__);

    if (!hci_hal || !hci_hal->hal) {
        ALOGE("NULL input argument");
        return FM_HC_STATUS_NULL_POINTER;
    }

    memset(&hci, 0, sizeof(struct fm_hci_t));

    hci.cb = hci_hal->cb;
    hci.command_credits = 1;
    hci.is_tx_processing = false;
    hci.is_rx_processing = false;
    hci.is_tx_thread_running = false;
    hci.is_rx_thread_running = false;
    hci.state = FM_RADIO_DISABLED;
    hci_hal->hci = &hci;

    if (hci_initialize()) {
        //wait for iniialization complete
        ALOGD("--%s waiting for iniialization complete hci state: %d ",
                __func__, hci.state);
        if(hci.state == FM_RADIO_ENABLING){
            Lock lk(hci.on_mtx);
            hci.on_cond.wait(lk);
        }
    }

    if (hci.state == FM_RADIO_ENABLED) {
        while (hci.is_tx_thread_running == false
            || hci.is_rx_thread_running == false)
        {
            /* checking tx & rx thread running status after every
               5ms before notifying on to upper layer */
            usleep(5000);
        }
        ALOGD("--%s success", __func__);
        ret = FM_HC_STATUS_SUCCESS;
    } else {
       ALOGD("--%s failed", __func__);
       hci_close();
       hci.state = FM_RADIO_DISABLED;
    }
    return ret;
}

/*******************************************************************************
**
** Function         fm_hci_transmit
**
** Description      This function is called by helium hal & is used enqueue the
**                  tx commands in tx queue.
**
** Parameters:      p_hci - contains the fm helium hal hci pointer
**                  hdr - contains the fm command header pointer
**
** Returns          void
**
*******************************************************************************/
int fm_hci_transmit(void *p_hci, struct fm_command_header_t *hdr)
{
    if (!hdr) {
        ALOGE("NULL input arguments");
        return FM_HC_STATUS_NULL_POINTER;
    }

    return enqueue_fm_tx_cmd(hdr);
}

/*******************************************************************************
**
** Function         fm_hci_close
**
** Description      This function is used to close & cleanup hci
**
** Parameters:      p_hci - contains the fm hci pointer
**
**
** Returns          void
**
*******************************************************************************/
void fm_hci_close(void *p_hci)
{
    ALOGI("%s", __func__);
    hci.state = FM_RADIO_DISABLING;

    hci_close();
    stop_tx_thread();

    if (hci.cb && hci.cb->fm_hci_close_done) {
        ALOGI("%s:Notify FM OFF to hal", __func__);
        hci.cb->fm_hci_close_done();
    }

    hci.state = FM_RADIO_DISABLED;
}

