/* Traps
 *
 * Copyright 2026 Ciaran Anscomb
 *
 * This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 * XRoar is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * See COPYING.GPL for redistribution conditions.
 */

#ifndef XROAR_TRAP_H_
#define XROAR_TRAP_H_

#include "top-config.h"

#ifdef WANT_TRAPS

struct slist;
struct xroar_timeout;

struct trap_def {
	// Trap condition
	char *cond;

	// Constraints
	struct slist *count;  // list of struct trap_count_range (private)

	// Actions
	char *snap;
	char *timeout;
	int trace;
	int no_trace;
	int ratelimit;

	// Internal
	unsigned long counter;
	struct xroar_timeout *timeout_data;
};

// Initialisation & shutdown
void trap_init(void);
void trap_shutdown(void);

void trap_set(const char *cont);
void trap_set_count(const char *count);
void trap_handle(void *);
void trap_enable_all(void);

#else

#define trap_init()
#define trap_shutdown()

#define trap_set(s)
#define trap_enable_all()
#define trap_disable_all()

#endif

#endif
