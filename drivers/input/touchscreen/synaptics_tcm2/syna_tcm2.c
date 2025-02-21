// SPDX-License-Identifier: GPL-2.0
/*
 * Synaptics TouchCom touchscreen driver
 *
 * Copyright (C) 2017-2020 Synaptics Incorporated. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * INFORMATION CONTAINED IN THIS DOCUMENT IS PROVIDED "AS-IS," AND SYNAPTICS
 * EXPRESSLY DISCLAIMS ALL EXPRESS AND IMPLIED WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
 * AND ANY WARRANTIES OF NON-INFRINGEMENT OF ANY INTELLECTUAL PROPERTY RIGHTS.
 * IN NO EVENT SHALL SYNAPTICS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, PUNITIVE, OR CONSEQUENTIAL DAMAGES ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OF THE INFORMATION CONTAINED IN THIS DOCUMENT, HOWEVER CAUSED
 * AND BASED ON ANY THEORY OF LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, AND EVEN IF SYNAPTICS WAS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE. IF A TRIBUNAL OF COMPETENT JURISDICTION DOES
 * NOT PERMIT THE DISCLAIMER OF DIRECT DAMAGES OR ANY OTHER DAMAGES, SYNAPTICS'
 * TOTAL CUMULATIVE LIABILITY TO ANY PARTY SHALL NOT EXCEED ONE HUNDRED U.S.
 * DOLLARS.
 */

/**
 * @file syna_tcm2.c
 *
 * This file implements the Synaptics device driver running under Linux kernel
 * input device subsystem, and also communicate with Synaptics touch controller
 * through TouchComm command-response protocol.
 */

#include "syna_tcm2.h"
#include "syna_tcm2_platform.h"
#include "synaptics_touchcom_core_dev.h"
#include "synaptics_touchcom_func_base.h"
#include "synaptics_touchcom_func_touch.h"
#ifdef STARTUP_REFLASH
#ifdef HAS_ROMBOOT_REFLASH_FEATURE
#include "synaptics_touchcom_func_romboot.h"
#else
#include "synaptics_touchcom_func_reflash.h"
#endif
#endif

/* prize add fod function start */
#ifdef ENABLE_WAKEUP_GESTURE
extern bool panel_fod_is_enabled(void);
static unsigned int last_gesture_id;
#endif
/* prize add fod function end */

//#include "../../panel/include/panel-nt37700c-vdo-120hz-vfp-6382-new.h"	//drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-end
/* prize add by kangdong 20210910 start */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
#include "../../../misc/prize/hardware_info/hardware_info.h"
extern struct hardware_info current_tp_info;
#endif
/* prize add by kangdong 20210910 end */
#if IS_ENABLED(ENABLE_WAKEUP_GESTURE)
#include "../../../misc/prize/prize_common_node/prize_common_node.h"
#endif
/**
 * @section: USE_CUSTOM_TOUCH_REPORT_CONFIG
 *           Open if willing to set up the format of touch report.
 *           The custom_touch_format[] array can be used to describe the
 *           customized report format.
 */
#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
static unsigned char custom_touch_format[] = {
	/* entity code */                    /* bits */
#ifdef ENABLE_WAKEUP_GESTURE
	TOUCH_REPORT_GESTURE_ID,                8,
#endif
	TOUCH_REPORT_NUM_OF_ACTIVE_OBJECTS,     8,
	TOUCH_REPORT_FOREACH_ACTIVE_OBJECT,
	TOUCH_REPORT_OBJECT_N_INDEX,            8,
	TOUCH_REPORT_OBJECT_N_CLASSIFICATION,   8,
	TOUCH_REPORT_OBJECT_N_X_POSITION,       16,
	TOUCH_REPORT_OBJECT_N_Y_POSITION,       16,
	TOUCH_REPORT_FOREACH_END,
	TOUCH_REPORT_END
};
#endif

/**
 * @section: STARTUP_REFLASH_DELAY_TIME_MS
 *           The delayed time to start fw update during the startup time.
 *           This configuration depends on STARTUP_REFLASH.
 */
#ifdef STARTUP_REFLASH
#define STARTUP_REFLASH_DELAY_TIME_MS (5000)

#define FW_IMAGE_NAME "PR6777215_S3910P_bf085_boe_spi_15_20230703_240hz.img"
#endif

/**
 * @section: RESET_ON_RESUME_DELAY_MS
 *           The delayed time to issue a reset on resume state.
 *           This configuration depends on RESET_ON_RESUME.
 */
#ifdef RESET_ON_RESUME
#define RESET_ON_RESUME_DELAY_MS (100)
#endif


/**
 * @section: POWER_ALIVE_AT_SUSPEND
 *           indicate that the power is still alive even at
 *           system suspend.
 *           otherwise, there is no power supplied when system
 *           is going to suspend stage.
 */
#define POWER_ALIVE_AT_SUSPEND

/**
 * @section: global variables for an active drm panel
 *           in order to register display notifier
 */
#ifdef USE_DRM_PANEL_NOTIFIER
	struct drm_panel *active_panel;
#endif


#if defined(ENABLE_HELPER)
/**
 * syna_dev_reset_detected_cb()
 *
 * Callback to assign a task to helper thread.
 *
 * Please be noted that this function will be invoked in ISR so don't
 * issue another touchcomm command here.
 *
 * @param
 *    [ in] callback_data: pointer to caller data
 *
 * @return
 *    on success, 0 or positive value; otherwise, negative value on error.
 */
static void syna_dev_reset_detected_cb(void *callback_data)
{
	struct syna_tcm *tcm = (struct syna_tcm *)callback_data;

	if (!tcm->helper.workqueue) {
		LOGW("No helper thread created\n");
		return;
	}

#ifdef RESET_ON_RESUME
	if (tcm->pwr_state != PWR_ON)
		return;
#endif

	if (ATOMIC_GET(tcm->helper.task) == HELP_NONE) {
		ATOMIC_SET(tcm->helper.task, HELP_RESET_DETECTED);

		queue_work(tcm->helper.workqueue, &tcm->helper.work);
	}
}
/**
 * syna_dev_helper_work()
 *
 * According to the given task, perform the delayed work
 *
 * @param
 *    [ in] work: data for work used
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static void syna_dev_helper_work(struct work_struct *work)
{
	unsigned char task;
	struct syna_tcm_helper *helper =
			container_of(work, struct syna_tcm_helper, work);
	struct syna_tcm *tcm =
			container_of(helper, struct syna_tcm, helper);

	task = ATOMIC_GET(helper->task);

	switch (task) {
	case HELP_RESET_DETECTED:
		LOGD("Reset caught (device mode:0x%x)\n", tcm->tcm_dev->dev_mode);
		break;
	default:
		break;
	}

	ATOMIC_SET(helper->task, HELP_NONE);
}
#endif
/**
 * syna_dev_enable_lowpwr_gesture()
 *
 * Enable or disable the low power gesture mode.
 * Furthermore, set up the wake-up irq.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *    [ in] en:  '1' to enable low power gesture mode; '0' to disable
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enable_lowpwr_gesture(struct syna_tcm *tcm, bool en)
{
	int retval = 0;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;

	if (!tcm->lpwg_enabled)
		return 0;

	if (attn->irq_id == 0)
		return 0;

	if (en) {
		if (!tcm->irq_wake) {
			enable_irq_wake(attn->irq_id);
			tcm->irq_wake = true;
		}

		/* enable wakeup gesture mode
		 *
		 * the wakeup gesture control may result from by finger event;
		 * therefore, it's better to use ATTN driven mode here
		 */
		retval = syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				1,
				RESP_IN_ATTN);
		if (retval < 0) {
			LOGE("Fail to enable wakeup gesture via DC command\n");
			return retval;
		}
	} else {
		if (tcm->irq_wake) {
			disable_irq_wake(attn->irq_id);
			tcm->irq_wake = false;
		}

		/* disable wakeup gesture mode
		 *
		 * the wakeup gesture control may result from by finger event;
		 * therefore, it's better to use ATTN driven mode here
		 */
		retval = syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				0,
				RESP_IN_ATTN);
		if (retval < 0) {
			LOGE("Fail to disable wakeup gesture via DC command\n");
			return retval;
		}
	}

	return retval;
}

#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
/**
 * syna_dev_parse_custom_touch_data_cb()
 *
 * Callback to parse the custom or non-standard touch entity from the
 * touch report.
 *
 * Please be noted that this function will be invoked in ISR so don't
 * issue another touchcomm command here.
 * If really needed, please assign a task to helper thread.
 *
 * @param
 *    [ in]    code:          the code of current touch entity
 *    [ in]    config:        the report configuration stored
 *    [in/out] config_offset: offset of current position in report config,
 *                            and then return the updated position.
 *    [ in]    report:        touch report given
 *    [in/out] report_offset: offset of current position in touch report,
 *                            the updated position should be returned.
 *    [ in]    report_size:   size of given touch report
 *    [ in]    callback_data: pointer to caller data passed to callback function
 *
 * @return
 *    on success, 0 or positive value; otherwise, negative value on error.
 */
static int syna_dev_parse_custom_touch_data_cb(const unsigned char code,
		const unsigned char *config, unsigned int *config_offset,
		const unsigned char *report, unsigned int *report_offset,
		unsigned int report_size, void *callback_data)
{
	/**
	 * sample code to demonstrate how to parse the custom touch entity
	 * from the touch report, additional modifications will be needed.
	 *
	 * struct syna_tcm *tcm = (struct syna_tcm *)callback_data;
	 * unsigned int data;
	 * unsigned int bits;
	 *
	 * switch (code) {
	 * case CUSTOM_ENTITY_CODE:
	 *		bits = config[(*config_offset)++];
	 *		syna_tcm_get_touch_data(report, report_size,
	 *				*report_offset, bits, &data);
	 *
	 *		*report_offset += bits;
	 *		return bits;
	 *	default:
	 *		LOGW("Unknown touch config code (idx:%d 0x%02x)\n",
	 *			*config_offset, code);
	 *		return (-EINVAL);
	 *	}
	 *
	 */

	return (-EINVAL);
}
#endif

/**
 * syna_tcm_free_input_events()
 *
 * Clear all relevant touched events.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_free_input_events(struct syna_tcm *tcm)
{
	struct input_dev *input_dev = tcm->input_dev;
#ifdef TYPE_B_PROTOCOL
	unsigned int idx;
#endif

	if (input_dev == NULL)
		return;

	syna_pal_mutex_lock(&tcm->tp_event_mutex);

#ifdef TYPE_B_PROTOCOL
	for (idx = 0; idx < MAX_NUM_OBJECTS; idx++) {
		input_mt_slot(input_dev, idx);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
	}
#endif
	input_report_key(input_dev, BTN_TOUCH, 0);
	input_report_key(input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
	input_mt_sync(input_dev);
#endif
	input_sync(input_dev);

	syna_pal_mutex_unlock(&tcm->tp_event_mutex);

}

/**
 * syna_dev_report_input_events()
 *
 * Report touched events to the input subsystem.
 *
 * After syna_tcm_get_event_data() function and the touched data is ready,
 * this function can be called to report an input event.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_report_input_events(struct syna_tcm *tcm)
{
	unsigned int idx;
	unsigned int x;
	unsigned int y;
	int wx;
	int wy;
	unsigned int status;
	unsigned int touch_count;
	struct input_dev *input_dev = tcm->input_dev;
	unsigned int max_objects = tcm->tcm_dev->max_objects;
	struct tcm_touch_data_blob *touch_data;
	struct tcm_objects_data_blob *object_data;

	if (input_dev == NULL)
		return;

	syna_pal_mutex_lock(&tcm->tp_event_mutex);

	touch_data = &tcm->tp_data;
	object_data = &tcm->tp_data.object_data[0];

#ifdef ENABLE_WAKEUP_GESTURE
	LOGI("%d,%d,pwr_state:%d, irq_wake:%d,gesture_id=%d,last_gesture_id=%d\n",tcm->wakeup_click_enabled,tcm->finger_fod_enabled,tcm->pwr_state,tcm->irq_wake,touch_data->gesture_id,last_gesture_id);
	if(panel_fod_is_enabled()||(last_gesture_id == 128)) {
	if (((tcm->pwr_state == LOW_PWR)||(tcm->pwr_state == PWR_ON)) && tcm->irq_wake) {
		switch (touch_data->gesture_id) {
		case 1:

			if(tcm->wakeup_click_enabled){
			input_report_key(input_dev, KEY_WAKEUP, 1);
			input_sync(input_dev);
			input_report_key(input_dev, KEY_WAKEUP, 0);
			input_sync(input_dev);	
			LOGI("gesture_id double tap \n");
			}
			break;
		case 128:     /* 0x80 */
			if(tcm->finger_fod_enabled)
				atomic_set(&tcm->fod_figer_state, 1);
			
			if((tcm->finger_fod_enabled) && (last_gesture_id != touch_data->gesture_id)) {
			input_report_key(input_dev, KEY_GESTURE, 1);
			input_sync(input_dev);
			input_report_key(input_dev, KEY_GESTURE, 0);
			input_sync(input_dev);
			LOGI("gesture_id fod down \n");
			}
			break;
		case 129:     /* 0x81 */
			if(tcm->finger_fod_enabled) {
				if(atomic_read(&tcm->now_system_status)) {
				LOGI("now_system_status 1\n");
				atomic_set(&tcm->fod_figer_state, 0);
			input_report_key(input_dev, KEY_GESTURE_UP, 1);
			input_sync(input_dev);
			input_report_key(input_dev, KEY_GESTURE_UP, 0);
			input_sync(input_dev);
				tcm->hw_if->ops_hw_reset(tcm->hw_if);
				} else if(!atomic_read(&tcm->now_system_status)) {
				LOGI("now_system_status 0\n");
				atomic_set(&tcm->fod_figer_state, 0);
				input_report_key(input_dev, KEY_GESTURE_UP, 1);
				input_sync(input_dev);
				input_report_key(input_dev, KEY_GESTURE_UP, 0);
				input_sync(input_dev);	
				}
			LOGI("gesture_id fod up \n");
			}
			break;
		default:
			LOGD("unknown gesture_id\n");
			atomic_set(&tcm->fod_figer_state, 0);
			break;
		}
		last_gesture_id = touch_data->gesture_id;
	}
	}
#endif

	if (tcm->pwr_state == LOW_PWR)
		goto exit;

	touch_count = 0;

	for (idx = 0; idx < max_objects; idx++) {
		if (tcm->prev_obj_status[idx] == LIFT &&
				object_data[idx].status == LIFT)
			status = NOP;
		else
			status = object_data[idx].status;

		switch (status) {
		case LIFT:
#ifdef TYPE_B_PROTOCOL
			input_mt_slot(input_dev, idx);
			input_mt_report_slot_state(input_dev,
					MT_TOOL_FINGER, 0);
#endif
			break;
		case FINGER:
		case GLOVED_OBJECT:
			x = object_data[idx].x_pos;
			y = object_data[idx].y_pos;
			wx = object_data[idx].x_width;
			wy = object_data[idx].y_width;
#ifdef REPORT_SWAP_XY
			x = x ^ y;
			y = x ^ y;
			x = x ^ y;
#endif
#ifdef REPORT_FLIP_X
			x = tcm->input_dev_params.max_x - x;
#endif
#ifdef REPORT_FLIP_Y
			y = tcm->input_dev_params.max_y - y;
#endif
#ifdef TYPE_B_PROTOCOL
			input_mt_slot(input_dev, idx);
			input_mt_report_slot_state(input_dev,
					MT_TOOL_FINGER, 1);
#endif
			input_report_key(input_dev, BTN_TOUCH, 1);
			input_report_key(input_dev, BTN_TOOL_FINGER, 1);
			input_report_abs(input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
#ifdef REPORT_TOUCH_WIDTH
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MAJOR, MAX(wx, wy));
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MINOR, MIN(wx, wy));
#endif
#ifndef TYPE_B_PROTOCOL
			input_mt_sync(input_dev);
#endif
			LOGD("Finger %d: x = %d, y = %d\n", idx, x, y);
			touch_count++;
			break;
		default:
			break;
		}

		tcm->prev_obj_status[idx] = object_data[idx].status;
	}

	if (touch_count == 0) {
		input_report_key(input_dev, BTN_TOUCH, 0);
		input_report_key(input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
		input_mt_sync(input_dev);
#endif
	}

	input_sync(input_dev);

exit:
	syna_pal_mutex_unlock(&tcm->tp_event_mutex);

}

/**
 * syna_dev_create_input_device()
 *
 * Allocate an input device and set up relevant parameters to the
 * input subsystem.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_create_input_device(struct syna_tcm *tcm)
{
	int retval = 0;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;
	struct input_dev *input_dev = NULL;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		return -EINVAL;
	}

	input_dev = devm_input_allocate_device(dev);
#else /* Legacy API */
	input_dev = input_allocate_device();
#endif
	if (input_dev == NULL) {
		LOGE("Fail to allocate input device\n");
		return -ENODEV;
	}

	input_dev->name = TOUCH_INPUT_NAME;
	input_dev->phys = TOUCH_INPUT_PHYS_PATH;
	input_dev->id.product = SYNAPTICS_TCM_DRIVER_ID;
	input_dev->id.version = SYNAPTICS_TCM_DRIVER_VERSION;
	input_dev->dev.parent = tcm->pdev->dev.parent;
	input_set_drvdata(input_dev, tcm);

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);
#ifdef INPUT_PROP_DIRECT
	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#endif

#ifdef ENABLE_WAKEUP_GESTURE
	set_bit(KEY_WAKEUP, input_dev->keybit);
	input_set_capability(input_dev, EV_KEY, KEY_WAKEUP);
	set_bit(KEY_GESTURE, input_dev->keybit);
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE);
	set_bit(KEY_GESTURE_UP, input_dev->keybit);	
	input_set_capability(input_dev, EV_KEY, KEY_GESTURE_UP);
#endif

	input_set_abs_params(input_dev,
			ABS_MT_POSITION_X, 0, tcm_dev->max_x, 0, 0);
	input_set_abs_params(input_dev,
			ABS_MT_POSITION_Y, 0, tcm_dev->max_y, 0, 0);

	input_mt_init_slots(input_dev, tcm_dev->max_objects,
			INPUT_MT_DIRECT);

#ifdef REPORT_TOUCH_WIDTH
	input_set_abs_params(input_dev,
			ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev,
			ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
#endif

	tcm->input_dev_params.max_x = tcm_dev->max_x;
	tcm->input_dev_params.max_y = tcm_dev->max_y;
	tcm->input_dev_params.max_objects = tcm_dev->max_objects;

	retval = input_register_device(input_dev);
	if (retval < 0) {
		LOGE("Fail to register input device\n");
		input_free_device(input_dev);
		input_dev = NULL;
		return retval;
	}

	tcm->input_dev = input_dev;

	return 0;
}

/**
 * syna_dev_release_input_device()
 *
 * Release an input device allocated previously.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_release_input_device(struct syna_tcm *tcm)
{
	if (!tcm->input_dev)
		return;

	input_unregister_device(tcm->input_dev);
	input_free_device(tcm->input_dev);

	tcm->input_dev = NULL;
}

/**
 * syna_dev_check_input_params()
 *
 * Check if any of the input parameters registered to the input subsystem
 * has changed.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    positive value to indicate mismatching parameters; otherwise, return 0.
 */
static int syna_dev_check_input_params(struct syna_tcm *tcm)
{
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (tcm_dev->max_x == 0 && tcm_dev->max_y == 0)
		return 0;

	if (tcm->input_dev_params.max_x != tcm_dev->max_x)
		return 1;

	if (tcm->input_dev_params.max_y != tcm_dev->max_y)
		return 1;

	if (tcm->input_dev_params.max_objects != tcm_dev->max_objects)
		return 1;

	if (tcm_dev->max_objects > MAX_NUM_OBJECTS) {
		LOGW("Out of max num objects defined, in app_info: %d\n",
			tcm_dev->max_objects);
		return 0;
	}

	LOGN("Input parameters unchanged\n");

	return 0;
}

/**
 * syna_dev_set_up_input_device()
 *
 * Set up input device to the input subsystem by confirming the supported
 * parameters and creating the device.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_set_up_input_device(struct syna_tcm *tcm)
{
	int retval = 0;

	if (IS_NOT_APP_FW_MODE(tcm->tcm_dev->dev_mode)) {
		LOGN("Application firmware not running, current mode: %02x\n",
			tcm->tcm_dev->dev_mode);
		return 0;
	}

	syna_dev_free_input_events(tcm);

	syna_pal_mutex_lock(&tcm->tp_event_mutex);

	retval = syna_dev_check_input_params(tcm);
	if (retval == 0)
		goto exit;

	if (tcm->input_dev != NULL)
		syna_dev_release_input_device(tcm);

	retval = syna_dev_create_input_device(tcm);
	if (retval < 0) {
		LOGE("Fail to create input device\n");
		goto exit;
	}

exit:
	syna_pal_mutex_unlock(&tcm->tp_event_mutex);

	return retval;
}

/**
 * syna_dev_isr()
 *
 * This is the function to be called when the interrupt is asserted.
 * The purposes of this handler is to read events generated by device and
 * retrieve all enqueued messages until ATTN is no longer asserted.
 *
 * @param
 *    [ in] irq:  interrupt line
 *    [ in] data: private data being passed to the handler function
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static irqreturn_t syna_dev_isr(int irq, void *data)
{
	int retval;
	unsigned char code = 0;
	struct syna_tcm *tcm = data;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;

	if (unlikely(gpio_get_value(attn->irq_gpio) != attn->irq_on_state))
		goto exit;

//drv Report TP events during sleep resolution-pengzhipeng-20230406-start
	//if (tcm->pwr_state == LOW_PWR)
	//	goto exit;
//drv Report TP events during sleep resolution-pengzhipeng-20230406-end
	tcm->isr_pid = current->pid;

#ifdef HAS_SYSFS_INTERFACE
	if (tcm->is_attn_redirecting) {
		syna_cdev_redirect_attn(tcm);
		goto exit;
	}
#endif
	/* retrieve the original report date generated by firmware */
	retval = syna_tcm_get_event_data(tcm->tcm_dev,
			&code,
			&tcm->event_data);
	if (retval < 0) {
		LOGE("Fail to get event data\n");
		goto exit;
	}

#ifdef ENABLE_EXTERNAL_FRAME_PROCESS
	if (tcm->report_to_queue[code] == EFP_ENABLE) {
		syna_tcm_buf_lock(&tcm->tcm_dev->external_buf);
		syna_cdev_update_report_queue(tcm, code,
		    &tcm->tcm_dev->external_buf);
		syna_tcm_buf_unlock(&tcm->tcm_dev->external_buf);
#ifndef REPORT_CONCURRENTLY
		goto exit;
#endif
	}
#endif
	/* report input event only when receiving a touch report */
	if (code == REPORT_TOUCH) {
		/* parse touch report once received */
		retval = syna_tcm_parse_touch_report(tcm->tcm_dev,
				tcm->event_data.buf,
				tcm->event_data.data_length,
				&tcm->tp_data);
		if (retval < 0) {
			LOGE("Fail to parse touch report\n");
			goto exit;
		}
		/* forward the touch event to system */
		syna_dev_report_input_events(tcm);
	}

exit:
	return IRQ_HANDLED;
}


/**
 * syna_dev_request_irq()
 *
 * Allocate an interrupt line and register the ISR handler
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_request_irq(struct syna_tcm *tcm)
{
	int retval;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		retval = -EINVAL;
		goto exit;
	}
#endif

	if (attn->irq_gpio < 0) {
		LOGE("Invalid IRQ GPIO\n");
		retval = -EINVAL;
		goto exit;
	}

	attn->irq_id = gpio_to_irq(attn->irq_gpio);

#ifdef DEV_MANAGED_API
	retval = devm_request_threaded_irq(dev,
			attn->irq_id,
			NULL,
			syna_dev_isr,
			attn->irq_flags,
			PLATFORM_DRIVER_NAME,
			tcm);
#else /* Legacy API */
	retval = request_threaded_irq(attn->irq_id,
			NULL,
			syna_dev_isr,
			attn->irq_flags,
			PLATFORM_DRIVER_NAME,
			tcm);
#endif
	if (retval < 0) {
		LOGE("Fail to request threaded irq\n");
		goto exit;
	}

	attn->irq_enabled = true;

	LOGI("Interrupt handler registered\n");

exit:
	return retval;
}

/**
 * syna_dev_release_irq()
 *
 * Release an interrupt line allocated previously
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_release_irq(struct syna_tcm *tcm)
{
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		return;
	}
#endif

	if (attn->irq_id <= 0)
		return;

	if (tcm->hw_if->ops_enable_irq)
		tcm->hw_if->ops_enable_irq(tcm->hw_if, false);

#ifdef DEV_MANAGED_API
	devm_free_irq(dev, attn->irq_id, tcm);
#else
	free_irq(attn->irq_id, tcm);
#endif

	attn->irq_id = 0;
	attn->irq_enabled = false;

	LOGI("Interrupt handler released\n");
}

/**
 * syna_dev_set_up_app_fw()
 *
 * Implement the essential steps for the initialization including the
 * preparation of app info and the configuration of touch report.
 *
 * This function should be called whenever the device initially powers
 * up, resets, or firmware update.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_set_up_app_fw(struct syna_tcm *tcm)
{
	int retval = 0;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (IS_NOT_APP_FW_MODE(tcm_dev->dev_mode)) {
		LOGN("Application firmware not running, current mode: %02x\n",
			tcm_dev->dev_mode);
		return -EINVAL;
	}

	/* collect app info containing most of sensor information */
	retval = syna_tcm_get_app_info(tcm_dev, &tcm_dev->app_info);
	if (retval < 0) {
		LOGE("Fail to get application info\n");
		return -EIO;
	}

	/* set up the format of touch report */
#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
	retval = syna_tcm_set_touch_report_config(tcm_dev,
			custom_touch_format,
			(unsigned int)sizeof(custom_touch_format));
	if (retval < 0) {
		LOGE("Fail to setup the custom touch report format\n");
		return -EIO;
	}
#endif
	/* preserve the format of touch report */
	retval = syna_tcm_preserve_touch_report_config(tcm_dev);
	if (retval < 0) {
		LOGE("Fail to preserve touch report config\n");
		return -EIO;
	}

#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
	/* set up custom touch data parsing method */
	retval = syna_tcm_set_custom_touch_entity_callback(tcm_dev,
			syna_dev_parse_custom_touch_data_cb,
			(void *)tcm);
	if (retval < 0) {
		LOGE("Fail to set up custom touch data parsing method\n");
		return -EIO;
	}
#endif
	return retval;
}

/**
 * syna_dev_reflash_startup_work()
 *
 * Perform firmware update during system startup.
 * Function is available when the 'STARTUP_REFLASH' configuration
 * is enabled.
 *
 * @param
 *    [ in] work: handle of work structure
 *
 * @return
 *    none.
 */
#ifdef STARTUP_REFLASH
static void syna_dev_reflash_startup_work(struct work_struct *work)
{
	int retval;
	struct delayed_work *delayed_work;
	struct syna_tcm *tcm;
	struct tcm_dev *tcm_dev;
	const struct firmware *fw_entry;
	const unsigned char *fw_image = NULL;
	unsigned int fw_image_size;

	delayed_work = container_of(work, struct delayed_work, work);
	tcm = container_of(delayed_work, struct syna_tcm, reflash_work);

	tcm_dev = tcm->tcm_dev;

	/* get firmware image */
	retval = request_firmware(&fw_entry,
			FW_IMAGE_NAME,
			tcm->pdev->dev.parent);
	if (retval < 0) {
		LOGE("Fail to request %s\n", FW_IMAGE_NAME);
		return;
	}

	fw_image = fw_entry->data;
	fw_image_size = fw_entry->size;

	LOGD("Firmware image size = %d\n", fw_image_size);

	pm_stay_awake(&tcm->pdev->dev);


	/* perform fw update */
#ifdef MULTICHIP_DUT_REFLASH
	/* do firmware update for the multichip-based device */
	retval = syna_tcm_romboot_do_multichip_reflash(tcm_dev,
			fw_image,
			fw_image_size,
			RESP_IN_ATTN,
			false);
#else
	/* do firmware update for the common device */
	retval = syna_tcm_do_fw_update(tcm_dev,
			fw_image,
			fw_image_size,
			RESP_IN_ATTN,
			true);//drv-Change the firmware to mandatory upgrade (UPDATE_FIRMWARE_CONFIG) to solve the problem that firmware is not touched due to incorrect Settings (run UPDATE_CONFIG_ONLY)-pengzhipeng-20230515
#endif
	if (retval < 0) {
		LOGE("Fail to do reflash\n");
		goto exit;
	}

	/* re-initialize the app fw */
	retval = syna_dev_set_up_app_fw(tcm);
	if (retval < 0) {
		LOGE("Fail to set up app fw after fw update\n");
		goto exit;
	}
	printk("pzp tcm_dev->app_info.customer_config_id=%s\n", tcm_dev->app_info.customer_config_id);
	/* prize add by kangdong 20210910 start */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
	snprintf (current_tp_info.chip, 25,"S3910P_FW:0x%s",tcm_dev->app_info.customer_config_id);
	strcpy(current_tp_info.vendor," S3910P");
	strcpy(current_tp_info.id," 0x00");
	strcpy(current_tp_info.more," 1080*2412");
#endif
/* prize add by kangdong 20210728 end */
	/* ensure the settings of input device
	 * if needed, re-create a new input device
	 */
	retval = syna_dev_set_up_input_device(tcm);
	if (retval < 0) {
		LOGE("Fail to register input device\n");
		goto exit;
	}
exit:
	pm_relax(&tcm->pdev->dev);
}
#endif
#if defined(POWER_ALIVE_AT_SUSPEND) && !defined(RESET_ON_RESUME)
/**
 * syna_dev_enter_normal_sensing()
 *
 * Helper to enter normal sensing mode
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enter_normal_sensing(struct syna_tcm *tcm)
{
	int retval = 0;

	if (!tcm)
		return -EINVAL;

	/* bring out of sleep mode. */
	retval = syna_tcm_sleep(tcm->tcm_dev, false);
	if (retval < 0) {
		LOGE("Fail to exit deep sleep\n");
		return retval;
	}

	/* disable low power gesture mode, if needed */
	if (tcm->lpwg_enabled) {
		retval = syna_dev_enable_lowpwr_gesture(tcm, false);
		if (retval < 0) {
			LOGE("Fail to disable low power gesture mode\n");
			return retval;
		}
	}

	return 0;
}
#endif
#ifdef POWER_ALIVE_AT_SUSPEND
/**
 * syna_dev_enter_lowpwr_sensing()
 *
 * Helper to enter power-saved sensing mode, that
 * may be the lower power gesture mode or deep sleep mode.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enter_lowpwr_sensing(struct syna_tcm *tcm)
{
	int retval = 0;

	if (!tcm)
		return -EINVAL;

	/* enable low power gesture mode, if needed */
	if (tcm->lpwg_enabled) {
		retval = syna_dev_enable_lowpwr_gesture(tcm, true);
		if (retval < 0) {
			LOGE("Fail to disable low power gesture mode\n");
			return retval;
		}
	} else {
	/* enter sleep mode for non-LPWG cases */
		if (!tcm->slept_in_early_suspend) {
			retval = syna_tcm_sleep(tcm->tcm_dev, true);
			if (retval < 0) {
				LOGE("Fail to enter deep sleep\n");
				return retval;
			}
		}
	}

	return 0;
}
#endif
/**
 * syna_dev_resume()
 *
 * Resume from the suspend state.
 * If RESET_ON_RESUME is defined, a reset is issued to the touch controller.
 * Otherwise, the touch controller is brought out of sleep mode.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_resume(struct device *dev)
{
	int retval;
	struct syna_tcm *tcm = dev_get_drvdata(dev);
	struct syna_hw_interface *hw_if = tcm->hw_if;
	bool irq_enabled = true;
#ifdef RESET_ON_RESUME
	unsigned char status;
#endif
#ifdef ENABLE_WAKEUP_GESTURE	
	atomic_set(&tcm->now_system_status, 1); //prize add, resume
#endif	
	/* exit directly if device isn't in suspend state */
	if (tcm->pwr_state == PWR_ON)
		return 0;

	LOGI("Prepare to resume device\n");

#ifdef ENABLE_WAKEUP_GESTURE
	LOGI("fod_figer_state = %d\n",atomic_read(&tcm->fod_figer_state));
	if (atomic_read(&tcm->fod_figer_state) == 0) /*prize add skip reset when fod down 20230704 */
	{
#endif
#ifdef RESET_ON_RESUME
	LOGI("Do reset on resume\n");
	syna_pal_sleep_ms(RESET_ON_RESUME_DELAY_MS);
	
	if (hw_if->ops_enable_irq)
		hw_if->ops_enable_irq(hw_if,false);

	if (hw_if->ops_hw_reset) {
		hw_if->ops_hw_reset(hw_if);
		syna_pal_sleep_ms(50);
		if (hw_if->ops_enable_irq)
			hw_if->ops_enable_irq(hw_if,true);

		retval = syna_tcm_get_event_data(tcm->tcm_dev,
			&status, NULL);
		if ((retval < 0) || (status != REPORT_IDENTIFY)) {
			LOGE("Fail to complete hw reset retval =%d status=%d\n",retval,status);
			goto exit;
		}
	} else {
		retval = syna_tcm_reset(tcm->tcm_dev);
		if (retval < 0) {
			LOGE("Fail to do sw reset\n");
			goto exit;
		}
	}
#else
#ifdef POWER_ALIVE_AT_SUSPEND
	/* enter normal power mode */
	retval = syna_dev_enter_normal_sensing(tcm);
	if (retval < 0) {
		LOGE("Fail to enter normal power mode\n");
		goto exit;
	}
#endif
	retval = syna_tcm_rezero(tcm->tcm_dev);
	if (retval < 0) {
		LOGE("Fail to rezero\n");
		goto exit;
	}
#endif
#ifdef ENABLE_WAKEUP_GESTURE
	}  /*prize add skip reset when fod down 20230704 */
#endif
	tcm->pwr_state = PWR_ON;

	LOGI("Prepare to set up application firmware\n");

	/* set up app firmware */
	retval = syna_dev_set_up_app_fw(tcm);
	if (retval < 0) {
		LOGE("Fail to set up app firmware on resume\n");
		goto exit;
	}

	retval = 0;

	LOGI("Device resumed (pwr_state:%d)\n", tcm->pwr_state);

exit:
	/* set irq back to active mode if not enabled yet */
	irq_enabled = (!hw_if->bdata_attn.irq_enabled);

	/* enable irq */
	if (irq_enabled && (hw_if->ops_enable_irq))
		hw_if->ops_enable_irq(hw_if, true);

	tcm->slept_in_early_suspend = false;

	return retval;
}

/**
 * syna_dev_suspend()
 *
 * Put device into suspend state.
 * Enter either the lower power gesture mode or sleep mode.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_suspend(struct device *dev)
{
#ifdef POWER_ALIVE_AT_SUSPEND
	int retval;
#endif
	struct syna_tcm *tcm = dev_get_drvdata(dev);
	struct syna_hw_interface *hw_if = tcm->hw_if;
	bool irq_disabled = true;

#ifdef ENABLE_WAKEUP_GESTURE	
	atomic_set(&tcm->now_system_status, 0); //prize add, suspend
#endif	
	/* exit directly if device is already in suspend state */
	if (tcm->pwr_state != PWR_ON)
		return 0;

	LOGI("Prepare to suspend device\n");

	/* clear all input events  */
	syna_dev_free_input_events(tcm);

#ifdef POWER_ALIVE_AT_SUSPEND
	/* enter power saved mode if power is not off */
	retval = syna_dev_enter_lowpwr_sensing(tcm);
	if (retval < 0) {
		LOGE("Fail to enter suspended power mode\n");
		return retval;
	}
	tcm->pwr_state = LOW_PWR;
#else
	tcm->pwr_state = PWR_OFF;
#endif

	/* once lpwg is enabled, irq should be alive.
	 * otherwise, disable irq in suspend.
	 */
	irq_disabled = (!tcm->lpwg_enabled);

	/* disable irq */
	if (irq_disabled && (hw_if->ops_enable_irq))
		hw_if->ops_enable_irq(hw_if, false);

	LOGI("Device suspended (pwr_state:%d)\n", tcm->pwr_state);
	
	/* prize add fod function start */
#ifdef ENABLE_WAKEUP_GESTURE
	atomic_set(&tcm->fod_figer_state, 0);//drv-Fix the fingerprint unlock under the Changan screen, this time press the power button to wake up sleep resulting in TP no touch proble-pengzhipeng-20230815
	last_gesture_id = 0;
#endif
	/* prize add fod function end */

	return 0;
}
//drv-Solve the screen display and touch function-pengzhipeng-20230707-start
struct syna_tcm *tcm_core = NULL;
void prize_syna_dev_suspend(void)
{
	if(tcm_core !=  NULL)
	{
		syna_dev_suspend(&tcm_core->pdev->dev);
	}
}
EXPORT_SYMBOL_GPL(prize_syna_dev_suspend);
//drv-Solve the screen display and touch function-pengzhipeng-20230707-end
#if defined(ENABLE_DISP_NOTIFIER)
/**
 * syna_dev_early_suspend()
 *
 * If having early suspend support, enter the sleep mode for
 * non-lpwg cases.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_early_suspend(struct device *dev)
{
	int retval;
	struct syna_tcm *tcm = dev_get_drvdata(dev);

	/* exit directly if device is already in suspend state */
	if (tcm->pwr_state != PWR_ON)
		return 0;

	if (!tcm->lpwg_enabled) {
		retval = syna_tcm_sleep(tcm->tcm_dev, true);
		if (retval < 0) {
			LOGE("Fail to enter deep sleep\n");
			return retval;
		}
	}

	tcm->slept_in_early_suspend = true;

	return 0;
}
/**
 * syna_dev_fb_notifier_cb()
 *
 * Listen the display screen on/off event and perform the corresponding
 * actions.
 *
 * @param
 *    [ in] nb:     instance of notifier_block
 *    [ in] action: fb action
 *    [ in] data:   fb event data
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_fb_notifier_cb(struct notifier_block *nb,
		unsigned long action, void *data)
{
	int retval;
	int transition;
#if defined(USE_DRM_PANEL_NOTIFIER)
	struct drm_panel_notifier *evdata = data;
#else
	struct fb_event *evdata = data;
#endif
	struct syna_tcm *tcm = container_of(nb, struct syna_tcm, fb_notifier);
	int time = 0;
	int disp_blank_powerdown;
	int disp_early_event_blank;
	int disp_blank;
	int disp_blank_unblank;

	if (!evdata || !evdata->data || !tcm)
		return 0;

	retval = 0;

#if defined(USE_DRM_PANEL_NOTIFIER)
	disp_blank_powerdown = DRM_PANEL_BLANK_POWERDOWN;
	disp_early_event_blank = DRM_PANEL_EARLY_EVENT_BLANK;
	disp_blank = DRM_PANEL_EVENT_BLANK;
	disp_blank_unblank = DRM_PANEL_BLANK_UNBLANK;
#else
	disp_blank_powerdown = FB_BLANK_POWERDOWN;
	disp_early_event_blank = FB_EARLY_EVENT_BLANK;
	disp_blank = FB_EVENT_BLANK;
	disp_blank_unblank = FB_BLANK_UNBLANK;
#endif

	transition = *(int *)evdata->data;

	/* confirm the firmware flashing is completed before screen off */
	if (transition == disp_blank_powerdown) {
		while (ATOMIC_GET(tcm->tcm_dev->firmware_flashing)) {
			syna_pal_sleep_ms(500);

			time += 500;
			if (time >= 5000) {
				LOGE("Timed out waiting for re-flashing\n");
				ATOMIC_SET(tcm->tcm_dev->firmware_flashing, 0);
				return -EIO;
			}
		}
	}

	if (action == disp_early_event_blank &&
		transition == disp_blank_powerdown) {
		retval = syna_dev_early_suspend(&tcm->pdev->dev);
	} else if (action == disp_blank) {
		if (transition == disp_blank_powerdown) {
			retval = syna_dev_suspend(&tcm->pdev->dev);
			tcm->fb_ready = 0;
		} else if (transition == disp_blank_unblank) {
#ifndef RESUME_EARLY_UNBLANK
			retval = syna_dev_resume(&tcm->pdev->dev);
			tcm->fb_ready++;
#endif
		} else if (action == disp_early_event_blank &&
			transition == disp_blank_unblank) {
#ifdef RESUME_EARLY_UNBLANK
			retval = syna_dev_resume(&tcm->pdev->dev);
			tcm->fb_ready++;
#endif
		}
	}

	return 0;
}
#endif

/**
 * syna_dev_disconnect()
 *
 * This function will power off the connected device.
 * Then, all the allocated resource will be released.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_disconnect(struct syna_tcm *tcm)
{
	struct syna_hw_interface *hw_if = tcm->hw_if;

	if (tcm->is_connected == false) {
		LOGI("%s already disconnected\n", PLATFORM_DRIVER_NAME);
		return 0;
	}

#ifdef STARTUP_REFLASH
	cancel_delayed_work_sync(&tcm->reflash_work);
	flush_workqueue(tcm->reflash_workqueue);
	destroy_workqueue(tcm->reflash_workqueue);
#endif

	/* free interrupt line */
	if (hw_if->bdata_attn.irq_id)
		syna_dev_release_irq(tcm);

	/* unregister input device */
	syna_dev_release_input_device(tcm);

	tcm->input_dev_params.max_x = 0;
	tcm->input_dev_params.max_y = 0;
	tcm->input_dev_params.max_objects = 0;

	/* power off */
	if (hw_if->ops_power_on)
		hw_if->ops_power_on(hw_if, false);

	tcm->pwr_state = PWR_OFF;
	tcm->is_connected = false;

	LOGI("%s device disconnected\n", PLATFORM_DRIVER_NAME);

	return 0;
}

/**
 * syna_dev_connect()
 *
 * This function will power on and identify the connected device.
 * At the end of function, the ISR will be registered as well.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_connect(struct syna_tcm *tcm)
{
	int retval;
	struct syna_hw_interface *hw_if = tcm->hw_if;
	struct syna_hw_bus_data *bus = &hw_if->bdata_io;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (!tcm_dev) {
		LOGE("Invalid tcm_dev\n");
		return -EINVAL;
	}

	if (tcm->is_connected) {
		LOGI("%s already connected\n", PLATFORM_DRIVER_NAME);
		return 0;
	}

	/* power on the connected device */
	if (hw_if->ops_power_on) {
		retval = hw_if->ops_power_on(hw_if, true);
		if (retval < 0)
			return -ENODEV;
	}

	/* perform a hardware reset */
	if (hw_if->ops_hw_reset)
		hw_if->ops_hw_reset(hw_if);

	/* detect which modes of touch controller is running
	 *
	 * the signal of the Touch IC ready is called as "identify"
	 * report and generated by firmware
	 */
	retval = syna_tcm_detect_device(tcm->tcm_dev);
	if (retval < 0) {
		LOGE("Fail to detect the device\n");
		goto err_detect_dev;
	}

	switch (retval) {
	case MODE_APPLICATION_FIRMWARE:
		retval = syna_dev_set_up_app_fw(tcm);
		if (retval < 0) {
#ifdef FORCE_CONNECTION
			LOGW("App firmware is not available somehow\n");
			LOGW("Input device not created due to force connect\n");
#else
			LOGE("Fail to set up application firmware\n");
			/* switch to bootloader mode when failed */
			LOGI("Switch device to bootloader mode instead\n");
			syna_tcm_switch_fw_mode(tcm_dev,
					MODE_BOOTLOADER,
					FW_MODE_SWITCH_DELAY_MS);
#endif
		} else {
			/* allocate and register to input device subsystem */
			retval = syna_dev_set_up_input_device(tcm);
			if (retval < 0) {
				LOGE("Fail to set up input device\n");
				goto err_setup_input_dev;
			}
		}

		break;
	default:
		LOGN("Application firmware not running, current mode: %02x\n",
			retval);
		break;
	}

	/* register the interrupt handler */
	retval = syna_dev_request_irq(tcm);
	if (retval < 0) {
		LOGE("Fail to request the interrupt line\n");
		goto err_request_irq;
	}

	/* for the reference,
	 * create a delayed work to perform fw update during the startup time
	 */
#ifdef STARTUP_REFLASH
	tcm->reflash_workqueue =
			create_singlethread_workqueue("syna_reflash");
	INIT_DELAYED_WORK(&tcm->reflash_work, syna_dev_reflash_startup_work);
	queue_delayed_work(tcm->reflash_workqueue, &tcm->reflash_work,
			msecs_to_jiffies(STARTUP_REFLASH_DELAY_TIME_MS));
#endif

	tcm->pwr_state = PWR_ON;
	tcm->is_connected = true;

	LOGI("%s device connected\n", PLATFORM_DRIVER_NAME);

	LOGI("TCM packrat: %d\n", tcm->tcm_dev->packrat_number);
	LOGI("Config: lpwg_mode(%s), custom_tp_config(%s) helper_work(%s)\n",
		(tcm->lpwg_enabled) ? "yes" : "no",
		(tcm->has_custom_tp_config) ? "yes" : "no",
		(tcm->helper_enabled) ? "yes" : "no");
	LOGI("Config: startup_reflash(%s), hw_reset(%s), rst_on_resume(%s)\n",
		(tcm->startup_reflash_enabled) ? "yes" : "no",
		(hw_if->ops_hw_reset) ? "yes" : "no",
		(tcm->rst_on_resume_enabled) ? "yes" : "no");

	/* restore the read/write capability */
	if (tcm->tcm_dev->max_wr_size != bus->wr_chunk_size)
		bus->wr_chunk_size = tcm->tcm_dev->max_wr_size;
	if (tcm->tcm_dev->max_rd_size != bus->rd_chunk_size)
		bus->rd_chunk_size = tcm->tcm_dev->max_rd_size;

	LOGI("Config: write_chunk(%d), read_chunk(%d)\n",
		bus->wr_chunk_size, bus->rd_chunk_size);

	return 0;

err_request_irq:
	/* unregister input device */
	syna_dev_release_input_device(tcm);

err_setup_input_dev:
err_detect_dev:
	if (hw_if->ops_power_on)
		hw_if->ops_power_on(hw_if, false);

	return retval;
}

#ifdef USE_DRM_PANEL_NOTIFIER
static struct drm_panel *syna_dev_get_panel(struct device_node *np)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0)
		return NULL;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			LOGI("Find available panel\n");
			return panel;
		}
	}

	return NULL;
}
#endif

//drv-add GESTURE func-pengzhipeng-20230522-start
#if defined(ENABLE_WAKEUP_GESTURE)


static void syna_double_type_func(unsigned char on)
{

	if(1 == on){
		tcm_core->lpwg_enabled = true;
		tcm_core->wakeup_click_enabled = true;
		printk("%s enter DOUBLE-TAP gesture\n", __func__);
	}else if(0 == on){
		tcm_core->wakeup_click_enabled = false;
		if(!tcm_core->finger_fod_enabled)
		tcm_core->lpwg_enabled = false;
		
		printk("%s close DOUBLE-TAP gesture\n", __func__);
	}

}
static void syna_double_fod_func(unsigned char on)
{
	if(1 == on){
		tcm_core->lpwg_enabled = true;
		tcm_core->finger_fod_enabled = true;
		LOGI("%s enter finger fod\n", __func__);
	}else if(0 == on){
		tcm_core->finger_fod_enabled = false;
		if(!tcm_core->wakeup_click_enabled)
		tcm_core->lpwg_enabled = false;
		
		LOGI("%s close finger fod\n", __func__);
	}
}
#endif
//drv-add GESTURE func-pengzhipeng-20230522-end


/**
 * syna_dev_probe()
 *
 * Install the TouchComm device driver
 *
 * @param
 *    [ in] pdev: an instance of platform device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
extern void prize_common_node_register(char* name,void(*set)(unsigned char on_off));

static int syna_dev_probe(struct platform_device *pdev)
{
	int retval;
	struct syna_tcm *tcm = NULL;
	struct tcm_dev *tcm_dev = NULL;
	struct syna_hw_interface *hw_if = NULL;
#if defined(USE_DRM_PANEL_NOTIFIER)
	struct device *dev;
#endif

	hw_if = pdev->dev.platform_data;
	if (!hw_if) {
		LOGE("Fail to find hardware configuration\n");
		return -EINVAL;
	}

	tcm = syna_pal_mem_alloc(1, sizeof(struct syna_tcm));
	if (!tcm) {
		LOGE("Fail to create the instance of syna_tcm\n");
		return -ENOMEM;
	}

	/* allocate the TouchCom device handle
	 * recommend to set polling mode here because isr is not registered yet
	 */
	retval = syna_tcm_allocate_device(&tcm_dev, hw_if, RESP_IN_POLLING);
	if ((retval < 0) || (!tcm_dev)) {
		LOGE("Fail to allocate TouchCom device handle\n");
		goto err_allocate_cdev;
	}

	tcm->tcm_dev = tcm_dev;
	tcm->pdev = pdev;
	tcm->hw_if = hw_if;

	syna_tcm_buf_init(&tcm->event_data);

	syna_pal_mutex_alloc(&tcm->tp_event_mutex);

#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
	tcm->has_custom_tp_config = true;
#else
	tcm->has_custom_tp_config = false;
#endif
#ifdef STARTUP_REFLASH
	tcm->startup_reflash_enabled = true;
#else
	tcm->startup_reflash_enabled = false;
#endif
#ifdef RESET_ON_RESUME
	tcm->rst_on_resume_enabled = true;
#else
	tcm->rst_on_resume_enabled = false;
#endif
#ifdef ENABLE_HELPER
	tcm->helper_enabled = true;
#else
	tcm->helper_enabled = false;
#endif
#ifdef ENABLE_WAKEUP_GESTURE
	tcm->lpwg_enabled = false;
#else
	tcm->lpwg_enabled = false;
#endif
	tcm->irq_wake = false;

	tcm->is_connected = false;
	tcm->pwr_state = PWR_OFF;

	tcm->dev_connect = syna_dev_connect;
	tcm->dev_disconnect = syna_dev_disconnect;
	tcm->dev_set_up_app_fw = syna_dev_set_up_app_fw;
	tcm->dev_resume = syna_dev_resume;
	tcm->dev_suspend = syna_dev_suspend;

	tcm->userspace_app_info = NULL;

	platform_set_drvdata(pdev, tcm);

	device_init_wakeup(&pdev->dev, 1);

#if defined(TCM_CONNECT_IN_PROBE)
	/* connect to target device */
	retval = tcm->dev_connect(tcm);
	if (retval < 0) {
		LOGE("Fail to connect to the device\n");
		syna_pal_mutex_free(&tcm->tp_event_mutex);
		goto err_connect;
	}
#endif

#ifdef HAS_SYSFS_INTERFACE
	/* create the device file and register to char device classes */
	retval = syna_cdev_create_sysfs(tcm, pdev);
	if (retval < 0) {
		LOGE("Fail to create the device sysfs\n");
		syna_pal_mutex_free(&tcm->tp_event_mutex);
		goto err_create_cdev;
	}
#endif

#if defined(ENABLE_DISP_NOTIFIER)
#if defined(USE_DRM_PANEL_NOTIFIER)
	dev = syna_request_managed_device();
	active_panel = syna_dev_get_panel(dev->of_node);
	if (active_panel) {
		tcm->fb_notifier.notifier_call = syna_dev_fb_notifier_cb;
		retval = drm_panel_notifier_register(active_panel,
				&tcm->fb_notifier);
		if (retval < 0) {
			LOGE("Fail to register FB notifier client\n");
			goto err_create_cdev;
		}
	} else {
		LOGE("No available drm panel\n");
	}
#else
	
	tcm->fb_notifier.notifier_call = syna_dev_fb_notifier_cb;
	retval = fb_register_client(&tcm->fb_notifier);
	if (retval < 0) {
		LOGE("Fail to register FB notifier client\n");
		goto err_create_cdev;
	}
#endif
#endif

#if defined(ENABLE_HELPER)
	ATOMIC_SET(tcm->helper.task, HELP_NONE);
	tcm->helper.workqueue =
			create_singlethread_workqueue("synaptics_tcm_helper");
	INIT_WORK(&tcm->helper.work, syna_dev_helper_work);
	/* set up custom touch data parsing method */
	syna_tcm_set_reset_occurrence_callback(tcm_dev,
			syna_dev_reset_detected_cb,
			(void *)tcm);
#endif
#if defined(ENABLE_WAKEUP_GESTURE)
	tcm_core = tcm;
	prize_common_node_register("GESTURE", &syna_double_type_func);
	prize_common_node_register("FINGER", &syna_double_fod_func);
	atomic_set(&tcm->fod_figer_state, 0);
	atomic_set(&tcm->now_system_status, 1);
#endif
	LOGI("TouchComm driver, %s v%d.%s installed\n",
		PLATFORM_DRIVER_NAME,
		SYNAPTICS_TCM_DRIVER_VERSION,
		SYNAPTICS_TCM_DRIVER_SUBVER);

	return 0;

#ifdef HAS_SYSFS_INTERFACE
err_create_cdev:
	syna_tcm_remove_device(tcm->tcm_dev);
#endif
#if defined(TCM_CONNECT_IN_PROBE)
	tcm->dev_disconnect(tcm);
err_connect:
#endif
	syna_tcm_buf_release(&tcm->event_data);
	syna_pal_mutex_free(&tcm->tp_event_mutex);
err_allocate_cdev:
	syna_pal_mem_free((void *)tcm);

	return retval;
}

/**
 * syna_dev_remove()
 *
 * Release all allocated resources and remove the TouchCom device handle
 *
 * @param
 *    [ in] pdev: an instance of platform device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_remove(struct platform_device *pdev)
{
	struct syna_tcm *tcm = platform_get_drvdata(pdev);

	if (!tcm) {
		LOGW("Invalid handle to remove\n");
		return 0;
	}

#if defined(ENABLE_HELPER)
	cancel_work_sync(&tcm->helper.work);
	flush_workqueue(tcm->helper.workqueue);
	destroy_workqueue(tcm->helper.workqueue);
#endif
#if defined(ENABLE_DISP_NOTIFIER)
#if defined(USE_DRM_PANEL_NOTIFIER)
	if (active_panel)
		drm_panel_notifier_unregister(active_panel,
				&tcm->fb_notifier);
#else
	fb_unregister_client(&tcm->fb_notifier);
#endif
#endif

#ifdef HAS_SYSFS_INTERFACE
	/* remove the cdev and sysfs nodes */
	syna_cdev_remove_sysfs(tcm);
#endif

	/* check the connection status, and do disconnection */
	if (tcm->dev_disconnect(tcm) < 0)
		LOGE("Fail to do device disconnection\n");

	if (tcm->userspace_app_info != NULL)
		syna_pal_mem_free(tcm->userspace_app_info);

	syna_tcm_buf_release(&tcm->event_data);

	syna_pal_mutex_free(&tcm->tp_event_mutex);

	/* remove the allocated tcm device */
	syna_tcm_remove_device(tcm->tcm_dev);

	/* release the device context */
	syna_pal_mem_free((void *)tcm);

	return 0;
}

/**
 * syna_dev_shutdown()
 *
 * Call syna_dev_remove() to release all resources
 *
 * @param
 *    [in] pdev: an instance of platform device
 *
 * @return
 *    none.
 */
static void syna_dev_shutdown(struct platform_device *pdev)
{
	syna_dev_remove(pdev);
}


/**
 * Declare a TouchComm platform device
 */
#ifdef CONFIG_PM
static const struct dev_pm_ops syna_dev_pm_ops = {
#if !defined(ENABLE_DISP_NOTIFIER)
	.suspend = syna_dev_suspend,
	.resume = syna_dev_resume,
#endif
};
#endif

static struct platform_driver syna_dev_driver = {
	.driver = {
		.name = PLATFORM_DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &syna_dev_pm_ops,
#endif
	},
	.probe = syna_dev_probe,
	.remove = syna_dev_remove,
	.shutdown = syna_dev_shutdown,
};


/**
 * syna_dev_module_init()
 *
 * The entry function of the reference driver, which initialize the
 * lower-level bus and register a platform driver.
 *
 * @param
 *    void.
 *
 * @return
 *    0 if the driver registered and bound to a device,
 *    else returns a negative error code and with the driver not registered.
 */
static int __init syna_dev_module_init(void)
{
	int retval;

	retval = syna_hw_interface_init();
	if (retval < 0)
		return retval;

	return platform_driver_register(&syna_dev_driver);
}

/**
 * syna_dev_module_exit()
 *
 * Function is called when un-installing the driver.
 * Remove the registered platform driver and the associated bus driver.
 *
 * @param
 *    void.
 *
 * @return
 *    none.
 */
static void __exit syna_dev_module_exit(void)
{
	platform_driver_unregister(&syna_dev_driver);

	syna_hw_interface_exit();
}

module_init(syna_dev_module_init);
module_exit(syna_dev_module_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics TCM Touch Driver");
MODULE_LICENSE("GPL v2");

