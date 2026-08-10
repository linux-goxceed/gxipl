/* SPDX-License-Identifier: MIT */
#ifndef IPL_CONFIG_API_H
#define IPL_CONFIG_API_H

#include "ipl_config.h"

void ipl_config_defaults(struct ipl_config *cfg);
int ipl_config_load(struct ipl_config *out);
/* Peek SRAM config: 1 if verbose (or defaults), 0 if quiet valid config. */
int ipl_config_verbose_early(void);

#endif
