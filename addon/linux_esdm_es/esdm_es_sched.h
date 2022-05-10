/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _ESDM_ES_SCHED_H
#define _ESDM_ES_SCHED_H

void esdm_sched_es_init(bool highres_timer);

extern struct esdm_es_cb esdm_es_sched;

int __init esdm_es_sched_module_init(void);
void esdm_es_sched_module_exit(void);

#endif /* _ESDM_ES_SCHED_H */