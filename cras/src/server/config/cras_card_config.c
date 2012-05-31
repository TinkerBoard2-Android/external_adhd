/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <iniparser.h>
#include <syslog.h>

#include "cras_util.h"
#include "cras_volume_curve.h"
#include "utlist.h"

/* Allocate 63 chars + 1 for null where declared. */
static const unsigned int MAX_INI_NAME_LEN = 63;

struct cras_card_config {
	dictionary *ini;
};

/*
 * Exported interface.
 */

struct cras_card_config *cras_card_config_create(const char *config_path,
						 const char *card_name)
{
	struct cras_card_config *card_config = NULL;
	char ini_name[MAX_INI_NAME_LEN + 1];
	dictionary *ini;

	snprintf(ini_name, MAX_INI_NAME_LEN, "%s/%s", config_path, card_name);
	ini_name[MAX_INI_NAME_LEN] = '\0';
	ini = iniparser_load(ini_name);
	if (ini == NULL) {
		syslog(LOG_DEBUG, "No ini file %s", ini_name);
		return NULL;
	}

	card_config = calloc(1, sizeof(*card_config));
	if (card_config == NULL) {
		iniparser_freedict(ini);
		return NULL;
	}

	card_config->ini = ini;
	syslog(LOG_DEBUG, "Loaded ini file %s", ini_name);
	return card_config;
}

void cras_card_config_destroy(struct cras_card_config *card_config)
{
	assert(card_config);
	iniparser_freedict(card_config->ini);
	free(card_config);
}

struct cras_volume_curve *cras_card_config_get_volume_curve_for_control(
		const struct cras_card_config *card_config,
		const char *control_name)
{
	/* 63 chars + 1 in declaration for null. */
	static const unsigned int INI_KEY_LEN = 63;
	char ini_key[INI_KEY_LEN + 1];
	const char *curve_type;

	if (card_config == NULL || control_name == NULL)
		return cras_volume_curve_create_default();

	snprintf(ini_key, INI_KEY_LEN, "%s:volume_curve", control_name);
	ini_key[INI_KEY_LEN] = 0;
	curve_type = iniparser_getstring(card_config->ini, ini_key, NULL);

	if (curve_type && strcmp(curve_type, "simple_step") == 0) {
		int max_volume;
		int volume_step;

		snprintf(ini_key, INI_KEY_LEN, "%s:max_volume", control_name);
		ini_key[INI_KEY_LEN] = 0;
		max_volume = iniparser_getint(card_config->ini, ini_key, 0);
		snprintf(ini_key, INI_KEY_LEN, "%s:volume_step", control_name);
		ini_key[INI_KEY_LEN] = 0;
		volume_step = iniparser_getint(card_config->ini, ini_key, 300);
		syslog(LOG_ERR, "Configure curve found for %s.", control_name);
		return cras_volume_curve_create_simple_step(max_volume,
							    volume_step);
	}
	syslog(LOG_ERR, "No configure curve found for %s.", control_name);
	return cras_volume_curve_create_default();
}
