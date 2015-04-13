/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <syslog.h>

#include "audio_thread.h"
#include "cras_empty_iodev.h"
#include "cras_iodev.h"
#include "cras_iodev_info.h"
#include "cras_iodev_list.h"
#include "cras_rstream.h"
#include "cras_server.h"
#include "cras_tm.h"
#include "cras_types.h"
#include "cras_system_state.h"
#include "stream_list.h"
#include "test_iodev.h"
#include "utlist.h"

const struct timespec idle_timeout_interval = {
	.tv_sec = 10,
	.tv_nsec = 0
};

/* Linked list of available devices. */
struct iodev_list {
	struct cras_iodev *iodevs;
	size_t size;
};

/* List of enabled input/output devices.
 *    dev - The device.
 *    for_pinned_streams - True if the device is active only for pinned streams.
 */
struct enabled_dev {
	struct cras_iodev *dev;
	int for_pinned_streams;
	struct enabled_dev *prev, *next;
};

/* Lists for devs[CRAS_STREAM_INPUT] and devs[CRAS_STREAM_OUTPUT]. */
static struct iodev_list devs[CRAS_NUM_DIRECTIONS];
/* Keep a list of enabled inputs and outputs. */
static struct enabled_dev *enabled_devs[CRAS_NUM_DIRECTIONS];
/* Keep an empty device per direction. */
static struct cras_iodev *fallback_devs[CRAS_NUM_DIRECTIONS];
/* Keep a constantly increasing index for iodevs. Index 0 is reserved
 * to mean "no device". */
static uint32_t next_iodev_idx = MAX_SPECIAL_DEVICE_IDX;
/* Selected node for input and output. 0 if there is no node selected. */
static cras_node_id_t selected_input;
static cras_node_id_t selected_output;
/* Called when the nodes are added/removed. */
static struct cras_alert *nodes_changed_alert;
/* Called when the active output/input is changed */
static struct cras_alert *active_node_changed_alert;
/* Call when the volume of a node changes. */
static node_volume_callback_t node_volume_callback;
static node_volume_callback_t node_input_gain_callback;
static node_left_right_swapped_callback_t node_left_right_swapped_callback;
/* Thread that handles audio input and output. */
static struct audio_thread *audio_thread;
/* List of all streams. */
static struct stream_list *stream_list;
/* Idle device timer. */
static struct cras_timer *idle_timer;

static void nodes_changed_prepare(struct cras_alert *alert);
static void active_node_changed_prepare(struct cras_alert *alert);
static void idle_dev_check(struct cras_timer *timer, void *data);

static struct cras_iodev *find_dev(size_t dev_index)
{
	struct cras_iodev *dev;

	DL_FOREACH(devs[CRAS_STREAM_OUTPUT].iodevs, dev)
		if (dev->info.idx == dev_index)
			return dev;

	DL_FOREACH(devs[CRAS_STREAM_INPUT].iodevs, dev)
		if (dev->info.idx == dev_index)
			return dev;

	return NULL;
}

static struct cras_ionode *find_node(cras_node_id_t id)
{
	struct cras_iodev *dev;
	struct cras_ionode *node;
	uint32_t dev_index, node_index;

	dev_index = dev_index_of(id);
	node_index = node_index_of(id);

	dev = find_dev(dev_index);
	if (!dev)
		return NULL;

	DL_FOREACH(dev->nodes, node)
		if (node->idx == node_index)
			return node;

	return NULL;
}

/* Adds a device to the list.  Used from add_input and add_output. */
static int add_dev_to_list(struct cras_iodev *dev)
{
	struct cras_iodev *tmp;
	uint32_t new_idx;
	struct iodev_list *list = &devs[dev->direction];

	DL_FOREACH(list->iodevs, tmp)
		if (tmp == dev)
			return -EEXIST;

	dev->format = NULL;
	dev->ext_format = NULL;
	dev->prev = dev->next = NULL;

	/* Move to the next index and make sure it isn't taken. */
	new_idx = next_iodev_idx;
	while (1) {
		if (new_idx < MAX_SPECIAL_DEVICE_IDX)
			new_idx = MAX_SPECIAL_DEVICE_IDX;
		DL_SEARCH_SCALAR(list->iodevs, tmp, info.idx, new_idx);
		if (tmp == NULL)
			break;
		new_idx++;
	}
	dev->info.idx = new_idx;
	next_iodev_idx = new_idx + 1;
	list->size++;

	syslog(LOG_INFO, "Adding %s dev at index %u.",
	       dev->direction == CRAS_STREAM_OUTPUT ? "output" : "input",
	       dev->info.idx);
	DL_PREPEND(list->iodevs, dev);

	cras_iodev_list_update_device_list();
	return 0;
}

/* Removes a device to the list.  Used from rm_input and rm_output. */
static int rm_dev_from_list(struct cras_iodev *dev)
{
	struct cras_iodev *tmp;

	DL_FOREACH(devs[dev->direction].iodevs, tmp)
		if (tmp == dev) {
			if (dev->is_open(dev))
				return -EBUSY;
			DL_DELETE(devs[dev->direction].iodevs, dev);
			devs[dev->direction].size--;
			return 0;
		}

	/* Device not found. */
	return -EINVAL;
}

/* Fills a dev_info array from the iodev_list. */
static void fill_dev_list(struct iodev_list *list,
			  struct cras_iodev_info *dev_info,
			  size_t out_size)
{
	int i = 0;
	struct cras_iodev *tmp;
	DL_FOREACH(list->iodevs, tmp) {
		memcpy(&dev_info[i], &tmp->info, sizeof(dev_info[0]));
		i++;
		if (i == out_size)
			return;
	}
}

static const char *node_type_to_str(enum CRAS_NODE_TYPE type)
{
	switch (type) {
	case CRAS_NODE_TYPE_INTERNAL_SPEAKER:
		return "INTERNAL_SPEAKER";
	case CRAS_NODE_TYPE_HEADPHONE:
		return "HEADPHONE";
	case CRAS_NODE_TYPE_HDMI:
		return "HDMI";
	case CRAS_NODE_TYPE_INTERNAL_MIC:
		return "INTERNAL_MIC";
	case CRAS_NODE_TYPE_MIC:
		return "MIC";
	case CRAS_NODE_TYPE_AOKR:
		return "AOKR";
	case CRAS_NODE_TYPE_USB:
		return "USB";
	case CRAS_NODE_TYPE_BLUETOOTH:
		return "BLUETOOTH";
	case CRAS_NODE_TYPE_KEYBOARD_MIC:
		return "KEYBOARD_MIC";
	case CRAS_NODE_TYPE_UNKNOWN:
	default:
		return "UNKNOWN";
	}
}

/* Fills an ionode_info array from the iodev_list. */
static int fill_node_list(struct iodev_list *list,
			  struct cras_ionode_info *node_info,
			  size_t out_size)
{
	int i = 0;
	struct cras_iodev *dev;
	struct cras_ionode *node;
	DL_FOREACH(list->iodevs, dev) {
		DL_FOREACH(dev->nodes, node) {
			node_info->iodev_idx = dev->info.idx;
			node_info->ionode_idx = node->idx;
			node_info->plugged = node->plugged;
			node_info->plugged_time.tv_sec =
				node->plugged_time.tv_sec;
			node_info->plugged_time.tv_usec =
				node->plugged_time.tv_usec;
			node_info->active = dev->is_active &&
					    (dev->active_node == node);
			node_info->volume = node->volume;
			node_info->capture_gain = node->capture_gain;
			node_info->left_right_swapped = node->left_right_swapped;
			strcpy(node_info->name, node->name);
			snprintf(node_info->type, sizeof(node_info->type), "%s",
				node_type_to_str(node->type));
			node_info->type_enum = node->type;
			node_info++;
			i++;
			if (i == out_size)
				return i;
		}
	}
	return i;
}

/* Copies the info for each device in the list to "list_out". */
static int get_dev_list(struct iodev_list *list,
			struct cras_iodev_info **list_out)
{
	struct cras_iodev_info *dev_info;

	if (!list_out)
		return list->size;

	*list_out = NULL;
	if (list->size == 0)
		return 0;

	dev_info = malloc(sizeof(*list_out[0]) * list->size);
	if (dev_info == NULL)
		return -ENOMEM;

	fill_dev_list(list, dev_info, list->size);

	*list_out = dev_info;
	return list->size;
}

/* Called when the system volume changes.  Pass the current volume setting to
 * the default output if it is active. */
void sys_vol_change(void *data)
{
	struct cras_iodev *dev;

	DL_FOREACH(devs[CRAS_STREAM_OUTPUT].iodevs, dev) {
		if (dev->set_volume && dev->is_open(dev))
			dev->set_volume(dev);
	}
}

/* Called when the system mute state changes.  Pass the current mute setting
 * to the default output if it is active. */
void sys_mute_change(void *data)
{
	struct cras_iodev *dev;

	DL_FOREACH(devs[CRAS_STREAM_OUTPUT].iodevs, dev) {
		if (dev->set_mute && dev->is_open(dev))
			dev->set_mute(dev);
	}
}

static int dev_has_pinned_stream(unsigned int dev_idx)
{
	const struct cras_rstream *rstream;

	DL_FOREACH(stream_list_get(stream_list), rstream) {
		if (rstream->pinned_dev_idx == dev_idx)
			return 1;
	}
	return 0;
}

static int dev_is_enabled(struct cras_iodev *dev)
{
	struct enabled_dev *edev;

	DL_FOREACH(enabled_devs[dev->direction], edev) {
		if (edev->dev == dev)
			return 1;
	}

	return 0;
}

static void close_dev(struct cras_iodev *dev)
{
	if (!cras_iodev_is_open(dev) ||
	    dev_has_pinned_stream(dev->info.idx))
		return;
	audio_thread_rm_open_dev(audio_thread, dev);
	dev->idle_timeout.tv_sec = 0;
	cras_iodev_close(dev);
	if (idle_timer)
		cras_tm_cancel_timer(cras_system_state_get_tm(), idle_timer);
	idle_dev_check(NULL, NULL);
}

static void idle_dev_check(struct cras_timer *timer, void *data)
{
	struct enabled_dev *edev;
	struct timespec now;
	struct timespec min_idle_expiration;
	unsigned int num_idle_devs = 0;
	unsigned int min_idle_timeout_ms;

	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	min_idle_expiration.tv_sec = 0;
	min_idle_expiration.tv_nsec = 0;

	DL_FOREACH(enabled_devs[CRAS_STREAM_OUTPUT], edev) {
		if (edev->dev->idle_timeout.tv_sec == 0)
			continue;
		if (timespec_after(&now, &edev->dev->idle_timeout)) {
			audio_thread_rm_open_dev(audio_thread, edev->dev);
			edev->dev->idle_timeout.tv_sec = 0;
			cras_iodev_close(edev->dev);
			continue;
		}
		num_idle_devs++;
		if (min_idle_expiration.tv_sec == 0 ||
		    timespec_after(&min_idle_expiration,
				   &edev->dev->idle_timeout))
			min_idle_expiration = edev->dev->idle_timeout;
	}

	idle_timer = NULL;
	if (!num_idle_devs)
		return;
	if (timespec_after(&now, &min_idle_expiration)) {
		min_idle_timeout_ms = 0;
	} else {
		struct timespec timeout;
		subtract_timespecs(&min_idle_expiration, &now, &timeout);
		min_idle_timeout_ms = timespec_to_ms(&timeout);
	}
	/* Wake up when it is time to close the next idle device.  Sleep for a
	 * minimum of 10 milliseconds. */
	idle_timer = cras_tm_create_timer(cras_system_state_get_tm(),
					  MAX(min_idle_timeout_ms, 10),
					  idle_dev_check, NULL);
}

/* Open the device potentially filling the output with a pre buffer. */
static int init_device(struct cras_iodev *dev,
		       struct cras_rstream *rstream)
{
	int rc;
	struct cras_audio_format fmt;

	dev->idle_timeout.tv_sec = 0;

	if (cras_iodev_is_open(dev))
		return 0;

	if (dev->ext_format == NULL) {
		fmt = rstream->format;
		rc = cras_iodev_set_format(dev, &fmt);
		if (rc)
			return rc;
	}

	rc = cras_iodev_open(dev);
	if (rc)
		return rc;

	dev->min_cb_level = rstream->cb_threshold;
	dev->max_cb_level = 0;

	rc = audio_thread_add_open_dev(audio_thread, dev);
	if (rc)
		cras_iodev_close(dev);
	return rc;
}

static void suspend_devs()
{
	struct enabled_dev *edev;
	struct cras_rstream *rstream;

	DL_FOREACH(stream_list_get(stream_list), rstream) {
		if (rstream->is_pinned) {
			struct cras_iodev *dev;

			dev = find_dev(rstream->pinned_dev_idx);
			if (dev) {
				audio_thread_disconnect_stream(audio_thread,
							       rstream, dev);
				if (!dev_is_enabled(dev))
					close_dev(dev);
			}
		} else {
			audio_thread_disconnect_stream(audio_thread, rstream,
						       NULL);
		}
	}
	DL_FOREACH(enabled_devs[CRAS_STREAM_OUTPUT], edev) {
		close_dev(edev->dev);
	}
	DL_FOREACH(enabled_devs[CRAS_STREAM_INPUT], edev) {
		close_dev(edev->dev);
	}
}

static void resume_devs()
{
	struct enabled_dev *edev;
	struct cras_rstream *rstream;

	DL_FOREACH(stream_list_get(stream_list), rstream) {
		if (rstream->is_pinned) {
			struct cras_iodev *dev;

			dev = find_dev(rstream->pinned_dev_idx);
			if (dev) {
				init_device(dev, rstream);
				audio_thread_add_stream(audio_thread, rstream,
							dev);
			}
		} else {
			DL_FOREACH(enabled_devs[rstream->direction], edev) {
				init_device(edev->dev, rstream);
				audio_thread_add_stream(audio_thread, rstream,
							edev->dev);
			}
		}
	}
}

/* Called when the system audio is suspended or resumed. */
void sys_suspend_change(void *data)
{
	if (cras_system_get_suspended())
		suspend_devs();
	else
		resume_devs();
}

/* Called when the system capture gain changes.  Pass the current capture_gain
 * setting to the default input if it is active. */
void sys_cap_gain_change(void *data)
{
	struct cras_iodev *dev;

	DL_FOREACH(devs[CRAS_STREAM_INPUT].iodevs, dev) {
		if (dev->set_capture_gain && dev->is_open(dev))
			dev->set_capture_gain(dev);
	}
}

/* Called when the system capture mute state changes.  Pass the current capture
 * mute setting to the default input if it is active. */
void sys_cap_mute_change(void *data)
{
	struct cras_iodev *dev;

	DL_FOREACH(devs[CRAS_STREAM_INPUT].iodevs, dev) {
		if (dev->set_capture_mute && dev->is_open(dev))
			dev->set_capture_mute(dev);
	}
}

static int stream_added_cb(struct cras_rstream *rstream)
{
	struct enabled_dev *edev;
	int rc;

	/* Check that the target device is valid for pinned streams. */
	if (rstream->is_pinned) {
		struct cras_iodev *dev;
		dev = cras_iodev_list_find_dev(rstream->pinned_dev_idx);
		if (!dev)
			return -EINVAL;
		rc = init_device(dev, rstream);
		if (rc)
			return rc;

		return audio_thread_add_stream(audio_thread, rstream, dev);
	}
	DL_FOREACH(enabled_devs[rstream->direction], edev) {
		init_device(edev->dev, rstream);
		rc = audio_thread_add_stream(audio_thread, rstream, edev->dev);
		if (rc)
			syslog(LOG_ERR, "adding stream to thread");
	}
	return 0;
}

static int possibly_close_enabled_devs(enum CRAS_STREAM_DIRECTION dir)
{
	struct enabled_dev *edev;
	const struct cras_rstream *s;

	/* Check if there are still streams attached. */
	DL_FOREACH(stream_list_get(stream_list), s) {
		if (s->direction == dir)
			return 0;
	}

	/* No more default streams, close any device that doesn't have a stream
	 * pinned to it. */
	DL_FOREACH(enabled_devs[dir], edev) {
		if (dev_has_pinned_stream(edev->dev->info.idx))
			continue;
		if (dir == CRAS_STREAM_INPUT) {
			close_dev(edev->dev);
			continue;
		}
		/* Allow output devs to drain before closing. */
		clock_gettime(CLOCK_MONOTONIC_RAW, &edev->dev->idle_timeout);
		add_timespecs(&edev->dev->idle_timeout, &idle_timeout_interval);
		idle_dev_check(NULL, NULL);
	}

	return 0;
}

static void pinned_stream_removed(struct cras_rstream *rstream)
{
	struct cras_iodev *dev;

	dev = find_dev(rstream->pinned_dev_idx);
	if (!dev_is_enabled(dev))
		close_dev(dev);
}

/* Returns the number of milliseconds left to drain this stream.  This is passed
 * directly from the audio thread. */
static int stream_removed_cb(struct cras_rstream *rstream)
{
	enum CRAS_STREAM_DIRECTION direction = rstream->direction;
	int rc;

	rc = audio_thread_drain_stream(audio_thread, rstream);
	if (rc)
		return rc;

	if (rstream->is_pinned)
		pinned_stream_removed(rstream);

	possibly_close_enabled_devs(direction);

	return 0;
}

static int disable_device(struct enabled_dev *edev);

static void possibly_disable_fallback(enum CRAS_STREAM_DIRECTION dir)
{
	struct enabled_dev *edev;

	DL_FOREACH(enabled_devs[dir], edev) {
		if (edev->dev == fallback_devs[dir])
			disable_device(edev);
	}
}

static int enable_device(struct cras_iodev *dev)
{
	struct enabled_dev *edev;
	struct cras_rstream *stream;
	enum CRAS_STREAM_DIRECTION dir = dev->direction;

	DL_FOREACH(enabled_devs[dir], edev) {
		if (edev->dev == dev)
			return -EEXIST;
	}

	edev = calloc(1, sizeof(*edev));
	edev->dev = dev;
	DL_APPEND(enabled_devs[dir], edev);

	/* If there are active streams to attach to this device, open it. */
	DL_FOREACH(stream_list_get(stream_list), stream) {
		if (stream->direction == dir && !stream->is_pinned) {
			init_device(dev, stream);
			audio_thread_add_stream(audio_thread, stream, dev);
		}
	}

	return 0;
}

static int disable_device(struct enabled_dev *edev)
{
	struct cras_iodev *dev = edev->dev;
	enum CRAS_STREAM_DIRECTION dir = dev->direction;
	struct cras_rstream *stream;

	DL_DELETE(enabled_devs[dir], edev);
	free(edev);

	/* Pull all default streams off this device. */
	DL_FOREACH(stream_list_get(stream_list), stream) {
		if (stream->direction != dev->direction || stream->is_pinned)
			continue;
		audio_thread_disconnect_stream(audio_thread, stream, dev);
	}
	close_dev(dev);

	return 0;
}

static int cras_iodev_set_active(struct cras_iodev *new_active)
{
	struct enabled_dev *edev;

	cras_iodev_list_notify_active_node_changed();

	DL_FOREACH(enabled_devs[new_active->direction], edev) {
		disable_device(edev);
	}

	new_active->update_active_node(new_active);

	return enable_device(new_active);
}

/*
 * Exported Interface.
 */

void cras_iodev_list_init()
{
	cras_system_register_volume_changed_cb(sys_vol_change, NULL);
	cras_system_register_mute_changed_cb(sys_mute_change, NULL);
	cras_system_register_suspend_cb(sys_suspend_change, NULL);
	cras_system_register_capture_gain_changed_cb(sys_cap_gain_change, NULL);
	cras_system_register_capture_mute_changed_cb(sys_cap_mute_change, NULL);
	nodes_changed_alert = cras_alert_create(nodes_changed_prepare);
	active_node_changed_alert = cras_alert_create(
		active_node_changed_prepare);

	/* Create the audio stream list for the system. */
	stream_list = stream_list_create(stream_added_cb, stream_removed_cb,
					 cras_rstream_create,
					 cras_rstream_destroy,
					 cras_system_state_get_tm());

	/* Add an empty device so there is always something to play to or
	 * capture from. */
	fallback_devs[CRAS_STREAM_OUTPUT] =
			empty_iodev_create(CRAS_STREAM_OUTPUT);
	fallback_devs[CRAS_STREAM_INPUT] =
			empty_iodev_create(CRAS_STREAM_INPUT);
	enable_device(fallback_devs[CRAS_STREAM_OUTPUT]);
	enable_device(fallback_devs[CRAS_STREAM_INPUT]);
	audio_thread = audio_thread_create();
	audio_thread_start(audio_thread);

	cras_iodev_list_update_device_list();
}

void cras_iodev_list_deinit()
{
	cras_system_remove_volume_changed_cb(sys_vol_change, NULL);
	cras_system_remove_mute_changed_cb(sys_vol_change, NULL);
	cras_system_remove_suspend_cb(sys_suspend_change, NULL);
	cras_system_remove_capture_gain_changed_cb(sys_cap_gain_change, NULL);
	cras_system_remove_capture_mute_changed_cb(sys_cap_mute_change, NULL);
	cras_alert_destroy(nodes_changed_alert);
	cras_alert_destroy(active_node_changed_alert);
	nodes_changed_alert = NULL;
	active_node_changed_alert = NULL;
	audio_thread_destroy(audio_thread);
	stream_list_destroy(stream_list);
}

void cras_iodev_list_add_active_node(enum CRAS_STREAM_DIRECTION dir,
				     cras_node_id_t node_id)
{
	struct cras_iodev *new_dev;
	new_dev = find_dev(dev_index_of(node_id));
	if (!new_dev || new_dev->direction != dir)
		return;

	possibly_disable_fallback(dir);
	enable_device(new_dev);
}

void cras_iodev_list_rm_active_node(enum CRAS_STREAM_DIRECTION dir,
				    cras_node_id_t node_id)
{
	struct cras_iodev *dev;
	struct enabled_dev *edev;

	dev = find_dev(dev_index_of(node_id));
	if (!dev)
		return;

	DL_FOREACH(enabled_devs[dir], edev) {
		if (edev->dev == dev) {
			disable_device(edev);
			if (!enabled_devs[dir])
				enable_device(fallback_devs[dir]);
			return;
		}
	}
}

struct cras_iodev *cras_iodev_list_find_dev(size_t dev_index)
{
	return find_dev(dev_index);
}

int cras_iodev_list_add_output(struct cras_iodev *output)
{
	int rc;

	if (output->direction != CRAS_STREAM_OUTPUT)
		return -EINVAL;

	rc = add_dev_to_list(output);
	if (rc)
		return rc;

	return 0;
}

int cras_iodev_list_add_input(struct cras_iodev *input)
{
	int rc;

	if (input->direction != CRAS_STREAM_INPUT)
		return -EINVAL;

	rc = add_dev_to_list(input);
	if (rc)
		return rc;

	return 0;
}

int cras_iodev_list_rm_output(struct cras_iodev *dev)
{
	int res;
	struct enabled_dev *edev;

	/* Retire the current active output device before removing it from
	 * list, otherwise it could be busy and remain in the list.
	 */
	DL_FOREACH(enabled_devs[CRAS_STREAM_OUTPUT], edev) {
		if (edev->dev == dev) {
			disable_device(edev);
			break;
		}
	}
	res = rm_dev_from_list(dev);
	if (res == 0)
		cras_iodev_list_update_device_list();
	return res;
}

int cras_iodev_list_rm_input(struct cras_iodev *dev)
{
	int res;
	struct enabled_dev *edev;

	/* Retire the current active input device before removing it from
	 * list, otherwise it could be busy and remain in the list.
	 */
	DL_FOREACH(enabled_devs[CRAS_STREAM_INPUT], edev) {
		if (edev->dev == dev) {
			disable_device(edev);
			break;
		}
	}
	res = rm_dev_from_list(dev);
	if (res == 0)
		cras_iodev_list_update_device_list();
	return res;
}

int cras_iodev_list_get_outputs(struct cras_iodev_info **list_out)
{
	return get_dev_list(&devs[CRAS_STREAM_OUTPUT], list_out);
}

int cras_iodev_list_get_inputs(struct cras_iodev_info **list_out)
{
	return get_dev_list(&devs[CRAS_STREAM_INPUT], list_out);
}

cras_node_id_t cras_iodev_list_get_active_node_id(
	enum CRAS_STREAM_DIRECTION direction)
{
	struct enabled_dev *edev = enabled_devs[direction];

	if (!edev || !edev->dev || !edev->dev->active_node)
		return 0;

	return cras_make_node_id(edev->dev->info.idx,
				 edev->dev->active_node->idx);
}

void cras_iodev_list_update_device_list()
{
	struct cras_server_state *state;

	state = cras_system_state_update_begin();
	if (!state)
		return;

	state->num_output_devs = devs[CRAS_STREAM_OUTPUT].size;
	state->num_input_devs = devs[CRAS_STREAM_INPUT].size;
	fill_dev_list(&devs[CRAS_STREAM_OUTPUT], &state->output_devs[0],
		      CRAS_MAX_IODEVS);
	fill_dev_list(&devs[CRAS_STREAM_INPUT], &state->input_devs[0],
		      CRAS_MAX_IODEVS);

	state->num_output_nodes = fill_node_list(&devs[CRAS_STREAM_OUTPUT],
						 &state->output_nodes[0],
						 CRAS_MAX_IONODES);
	state->num_input_nodes = fill_node_list(&devs[CRAS_STREAM_INPUT],
						&state->input_nodes[0],
						CRAS_MAX_IONODES);
	state->selected_output = selected_output;
	state->selected_input = selected_input;

	cras_system_state_update_complete();
}

int cras_iodev_list_register_nodes_changed_cb(cras_alert_cb cb, void *arg)
{
	return cras_alert_add_callback(nodes_changed_alert, cb, arg);
}

int cras_iodev_list_remove_nodes_changed_cb(cras_alert_cb cb, void *arg)
{
	return cras_alert_rm_callback(nodes_changed_alert, cb, arg);
}

void cras_iodev_list_notify_nodes_changed()
{
	cras_alert_pending(nodes_changed_alert);
}

static void nodes_changed_prepare(struct cras_alert *alert)
{
	cras_iodev_list_update_device_list();
}

int cras_iodev_list_register_active_node_changed_cb(cras_alert_cb cb,
						    void *arg)
{
	return cras_alert_add_callback(active_node_changed_alert, cb, arg);
}

int cras_iodev_list_remove_active_node_changed_cb(cras_alert_cb cb,
						  void *arg)
{
	return cras_alert_rm_callback(active_node_changed_alert, cb, arg);
}

void cras_iodev_list_notify_active_node_changed()
{
	cras_alert_pending(active_node_changed_alert);
}

static void active_node_changed_prepare(struct cras_alert *alert)
{
	cras_iodev_list_update_device_list();
}

void cras_iodev_list_select_node(enum CRAS_STREAM_DIRECTION direction,
				 cras_node_id_t node_id)
{
	struct cras_iodev *old_dev = NULL, *new_dev = NULL;
	cras_node_id_t *selected;

	selected = (direction == CRAS_STREAM_OUTPUT) ? &selected_output :
		&selected_input;

	/* return if no change */
	if (node_id == *selected)
		return;

	/* find the devices for the id. */
	old_dev = find_dev(dev_index_of(*selected));
	new_dev = find_dev(dev_index_of(node_id));

	/* Fail if the direction is mismatched. We don't fail for the new_dev ==
	   NULL case. That can happen if node_id is 0 (no selection), or the
	   client tries to select a non-existing node (maybe it's unplugged just
	   before the client selects it). We will just behave like there is no
	   selected node. */
	if (new_dev && new_dev->direction != direction)
		return;

	/* change to new selection */
	*selected = node_id;

	/* update new device */
	if (new_dev) {
		/* There is an iodev and it isn't the default, switch to it. */
		cras_iodev_set_active(new_dev);
	}

	/* update old device if it is not the same device */
	if (old_dev && old_dev != new_dev)
		old_dev->update_active_node(old_dev);
}

int cras_iodev_list_set_node_attr(cras_node_id_t node_id,
				  enum ionode_attr attr, int value)
{
	struct cras_ionode *node;

	node = find_node(node_id);
	if (!node)
		return -EINVAL;

	return cras_iodev_set_node_attr(node, attr, value);
}

int cras_iodev_list_node_selected(struct cras_ionode *node)
{
	cras_node_id_t id = cras_make_node_id(node->dev->info.idx, node->idx);
	return (id == selected_input || id == selected_output);
}

void cras_iodev_list_set_node_volume_callbacks(node_volume_callback_t volume_cb,
					       node_volume_callback_t gain_cb)
{
	node_volume_callback = volume_cb;
	node_input_gain_callback = gain_cb;
}

void cras_iodev_list_set_node_left_right_swapped_callbacks(
		node_left_right_swapped_callback_t swapped_cb)
{
	node_left_right_swapped_callback = swapped_cb;
}

void cras_iodev_list_notify_node_volume(struct cras_ionode *node)
{
	cras_node_id_t id = cras_make_node_id(node->dev->info.idx, node->idx);

	if (node_volume_callback)
		node_volume_callback(id, node->volume);
}

void cras_iodev_list_notify_node_left_right_swapped(struct cras_ionode *node)
{
	cras_node_id_t id = cras_make_node_id(node->dev->info.idx, node->idx);

	if (node_left_right_swapped_callback)
		node_left_right_swapped_callback(id, node->left_right_swapped);
}

void cras_iodev_list_notify_node_capture_gain(struct cras_ionode *node)
{
	cras_node_id_t id = cras_make_node_id(node->dev->info.idx, node->idx);

	if (node_input_gain_callback)
		node_input_gain_callback(id, node->capture_gain);
}

void cras_iodev_list_add_test_dev(enum TEST_IODEV_TYPE type)
{
	if (type != TEST_IODEV_HOTWORD)
		return;
	test_iodev_create(CRAS_STREAM_INPUT, type);
}

void cras_iodev_list_test_dev_command(unsigned int iodev_idx,
				      enum CRAS_TEST_IODEV_CMD command,
				      unsigned int data_len,
				      const uint8_t *data)
{
	struct cras_iodev *dev = find_dev(iodev_idx);

	if (!dev)
		return;

	test_iodev_command(dev, command, data_len, data);
}

struct audio_thread *cras_iodev_list_get_audio_thread()
{
	return audio_thread;
}

struct stream_list *cras_iodev_list_get_stream_list()
{
	return stream_list;
}

void cras_iodev_list_reset()
{
	struct enabled_dev *edev;

	DL_FOREACH(enabled_devs[CRAS_STREAM_OUTPUT], edev) {
		DL_DELETE(enabled_devs[CRAS_STREAM_OUTPUT], edev);
		free(edev);
	}
	enabled_devs[CRAS_STREAM_OUTPUT] = NULL;
	DL_FOREACH(enabled_devs[CRAS_STREAM_INPUT], edev) {
		DL_DELETE(enabled_devs[CRAS_STREAM_INPUT], edev);
		free(edev);
	}
	enabled_devs[CRAS_STREAM_INPUT] = NULL;
	devs[CRAS_STREAM_OUTPUT].iodevs = NULL;
	devs[CRAS_STREAM_INPUT].iodevs = NULL;
	devs[CRAS_STREAM_OUTPUT].size = 0;
	devs[CRAS_STREAM_INPUT].size = 0;
}
