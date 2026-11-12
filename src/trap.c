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

#include "top-config.h"

// For strsep()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _DARWIN_C_SOURCE

#include <stdlib.h>
#include <string.h>

#include "c-strcase.h"
#include "pl-string.h"
#include "slist.h"
#include "xalloc.h"

#include "events.h"
#include "logging.h"
#include "machine.h"
#include "snapshot.h"
#include "trap.h"
#include "xroar.h"

#ifdef WANT_TRAPS

struct trap_count_range {
	unsigned long first;
	unsigned long last;
};

static int trap_msgr_client_id;
static unsigned num_trap_defs = 0;
static struct trap_def *trap_defs = NULL;
static struct trap_def *cur_trap_def = NULL;

static bool tape_motor_state = 0;
static event_ticks tape_motor_on_time = 0;
static struct slist *tape_motor_traps = NULL;

static void trap_ui_state_notify(void *sptr, int tag, void *smsg);

static void handle_trap(void *);
static void trap_handle_new(void *, bool RnW, uint32_t A);
static void trap_disable_all(void);

// =========================================================================

void trap_init(void) {
	trap_msgr_client_id = messenger_client_register();
	ui_messenger_join_group(trap_msgr_client_id, ui_tag_tape_motor, MESSENGER_NOTIFY_DELEGATE(trap_ui_state_notify, NULL));
}

void trap_shutdown(void) {
	trap_disable_all();
}

// Simple UI notifications where we just track the state

static void trap_ui_state_notify(void *sptr, int tag, void *smsg) {
	(void)sptr;
	struct ui_state_message *uimsg = smsg;
	int value = uimsg->value;
	//const void *data = uimsg->data;

	switch (tag) {

	case ui_tag_tape_motor:
		// Motor state is 0 (off), 1 (on) or the special value of -1
		// which implies end of tape.  For timeouts, we'll ignore any
		// motor off that occurs too soon after a motor on, but we will
		// not ignore end of tape.  XXX may want some way to report how
		// many time a BASIC ROM is expected to flip that state, and
		// ignore the first N?
		{
			bool new_motor_state = (value > 0);
			if (new_motor_state == tape_motor_state) {
				break;
			}
			tape_motor_state = new_motor_state;
			if (tape_motor_state) {
				tape_motor_on_time = event_current_tick;
			}
			bool motor_off = !tape_motor_state;
			bool cancel_timeout = tape_motor_state;
			if (motor_off) {
				int delta = event_tick_delta(event_current_tick, tape_motor_on_time);
				if (delta >= 0 && delta <= 416) {
					cancel_timeout = 1;
				}
			}
			struct slist *iter_next = NULL;
			for (struct slist *iter = tape_motor_traps; iter; iter = iter_next) {
				struct trap_def *trap = iter->data;
				iter_next = iter->next;
				if (motor_off) {
					// Temporarily remove timeout if cancelled
					char *old_timeout = trap->timeout;
					if (cancel_timeout) {
						--trap->counter;
						trap->timeout = NULL;
					}
					handle_trap(trap);
					// Restore timeout
					if (cancel_timeout) {
						if (!trap->timeout) {
							trap->timeout = old_timeout;
						} else if (old_timeout) {
							free(old_timeout);
						}
					}
				}
				if (trap->timeout_data && cancel_timeout) {
					xroar_cancel_timeout(trap->timeout_data);
					trap->timeout_data = NULL;
				}
			}
		}
		break;

	default:
		break;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Traps

static void parse_range_ul(const char *str, unsigned long *a, unsigned long *b) {
	char *str_copy = xstrdup(str);
	char *bstr = str_copy;
	char *astr = strsep(&bstr, "-");
	if (a && astr && *astr) {
		*a = strtoul(astr, NULL, 0);
	}
	if (b && bstr && *bstr) {
		*b = strtoul(bstr, NULL, 0);
	}
	free(str_copy);
}

static void enable_trap(struct trap_def *trap, bool enable) {
	if (0 == c_strcasecmp(trap->cond, "immediate") ||
	    0 == strcmp(trap->cond, "-")) {
		if (enable) {
			handle_trap(trap);
		}
		return;
	}
	if (0 == c_strcasecmp(trap->cond, "tape-motoroff") ||
	    0 == c_strcasecmp(trap->cond, "tape-motor-off")) {
		if (enable && !slist_find(tape_motor_traps, trap)) {
			tape_motor_traps = slist_append(tape_motor_traps, trap);
		}
		return;
	}
	if (!xroar.machine)
		return;
	char *cond_copy = xstrdup(trap->cond);
	char *val = cond_copy;
	char *key = strsep(&val, "=");
	if (key && *key && val && *val) {
		unsigned long a = -1UL, b = -1UL;
		parse_range_ul(val, &a, &b);

		if (a != -1UL) {
			if (b == -1UL) {
				b = a;
			}
			if (a > b) {
				unsigned long c = a;
				a = b;
				b = c;
			}
			if (0 == c_strcasecmp(key, "pc")) {
				if (enable) {
					machine_add_breakpoint(xroar.machine, a, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				} else {
					machine_remove_breakpoint(xroar.machine, a, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				}
			} else if (0 == c_strcasecmp(key, "read")) {
				if (enable) {
					machine_add_watchpoint(xroar.machine, 1, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				} else {
					machine_remove_watchpoint(xroar.machine, 1, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				}
			} else if (0 == c_strcasecmp(key, "write")) {
				if (enable) {
					machine_add_watchpoint(xroar.machine, 0, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				} else {
					machine_remove_watchpoint(xroar.machine, 0, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				}
			} else if (0 == c_strcasecmp(key, "access")) {
				if (enable) {
					machine_add_watchpoint(xroar.machine, 1, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
					machine_add_watchpoint(xroar.machine, 0, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				} else {
					machine_remove_watchpoint(xroar.machine, 1, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
					machine_remove_watchpoint(xroar.machine, 0, a, b, DELEGATE_AS2(void, bool, uint32, trap_handle_new, trap));
				}
			}
		}
		free(cond_copy);
	}
}

void trap_enable_all(void) {
	for (unsigned i = 0; i < num_trap_defs; ++i) {
		enable_trap(&trap_defs[i], 1);
	}
}

static void trap_disable_all(void) {
	for (unsigned i = 0; i < num_trap_defs; ++i) {
		enable_trap(&trap_defs[i], 0);
	}
}

// Temporarily wrap handle_trap() until we modernise watchpoints too
static void trap_handle_new(void *sptr, bool RnW, uint32_t A) {
	(void)RnW;
	(void)A;
	handle_trap(sptr);
}

static void handle_trap(void *sptr) {
	struct trap_def *trap = sptr;
	struct trap_count_range *range = trap->count ? trap->count->data : NULL;
	++trap->counter;
	if (range && trap->counter < range->first) {
		return;
	}

	// Break action
	if (trap->snap) {
		LOG_MOD_DEBUG(2, "trap", "write snapshot: %s\n", trap->snap);
		write_snapshot(trap->snap);
	}
	if (trap->timeout) {
		LOG_MOD_DEBUG(2, "trap", "timeout: %s\n", trap->timeout);
		trap->timeout_data = xroar_set_timeout(trap->timeout);
	}
	if (trap->trace >= 0) {
		LOG_MOD_DEBUG(2, "trap", "trace on\n");
		xroar_set_trace(1, trap->trace);
	}
	if (trap->no_trace > 0) {
		LOG_MOD_DEBUG(2, "trap", "trace off\n");
		xroar_set_trace(0, 0);
	}
	if (trap->ratelimit >= 0) {
		LOG_MOD_DEBUG(2, "trap", "ratelimit %s\n", trap->ratelimit ? "on" : "off");
		ui_update_state(-1, ui_tag_ratelimit_latch, trap->ratelimit, NULL);
	}

	if (range) {
		if (range->last == (unsigned long)-1) {
			// Unlimited - don't remove breakpoint
			return;
		}
		while (range && trap->counter >= range->last) {
			// Done with range - remove from list
			trap->count = slist_remove(trap->count, range);
			free(range);
			// And fetch the next one
			range = trap->count ? trap->count->data : NULL;
		}
		// If any range remains, don't remove breakpoint
		if (range) {
			return;
		}
	}
	// Disable breakpoint
	enable_trap(trap, 0);
}

void trap_set(const char *cond) {
	if (cur_trap_def) {
		if (xroar.cfg.debug.trap_def.count) {
			if (cur_trap_def->count) {
				slist_free_full(cur_trap_def->count, free);
			}
			cur_trap_def->count = xroar.cfg.debug.trap_def.count;
			xroar.cfg.debug.trap_def.count = NULL;
		}
		if (xroar.cfg.debug.trap_def.snap) {
			free(cur_trap_def->snap);
			cur_trap_def->snap = xroar.cfg.debug.trap_def.snap;
			xroar.cfg.debug.trap_def.snap = NULL;
		}
		if (xroar.cfg.debug.trap_def.timeout) {
			free(cur_trap_def->timeout);
			cur_trap_def->timeout = xroar.cfg.debug.trap_def.timeout;
			xroar.cfg.debug.trap_def.timeout = NULL;
		}
		if (xroar.cfg.debug.trap_def.trace >= 0) {
			cur_trap_def->trace = xroar.cfg.debug.trap_def.trace;
			xroar.cfg.debug.trap_def.trace = -1;
			if (!cur_trap_def->count) {
				struct trap_count_range *range = xmalloc(sizeof(*range));
				*range = (struct trap_count_range){ .first = 1, .last = -1UL };
				cur_trap_def->count = slist_append(cur_trap_def->count, range);
			}
		}
		if (xroar.cfg.debug.trap_def.no_trace >= 0) {
			cur_trap_def->no_trace = xroar.cfg.debug.trap_def.no_trace;
			xroar.cfg.debug.trap_def.no_trace = -1;
		}
		if (xroar.cfg.debug.trap_def.ratelimit >= 0) {
			cur_trap_def->ratelimit = xroar.cfg.debug.trap_def.ratelimit;
			xroar.cfg.debug.trap_def.ratelimit = -1;
		}
	}
	if (cond) {
		unsigned i = num_trap_defs++;
		trap_defs = xrealloc(trap_defs, num_trap_defs * sizeof(*trap_defs));
		cur_trap_def = &trap_defs[i];
		*cur_trap_def = (struct trap_def){0};
		cur_trap_def->cond = xstrdup(cond);
		cur_trap_def->trace = -1;
		cur_trap_def->no_trace = -1;
		cur_trap_def->ratelimit = -1;
	}
}

void trap_set_count(const char *count) {
	unsigned long a = -1UL, b = -1UL;
	parse_range_ul(count, &a, &b);
	if (a == -1UL) {
		a = 0;
	}
	struct trap_count_range *range = xmalloc(sizeof(*range));
	*range = (struct trap_count_range){0};
	range->first = a;
	range->last = b;
	xroar.cfg.debug.trap_def.count = slist_append(xroar.cfg.debug.trap_def.count, range);
}

#endif
