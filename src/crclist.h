/** \file
 *
 *  \brief ROM CRC database.
 *
 *  \copyright Copyright 2012-2020 Ciaran Anscomb
 *
 *  \licenseblock This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 *  XRoar is free software; you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  See COPYING.GPL for redistribution conditions.
 *
 *  \endlicenseblock
 */

#ifndef XROAR_CRCLIST_H_
#define XROAR_CRCLIST_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct sdsx_list;

// Assign a crclist.  Overwrites any existing list with provided name.
void crclist_assign(const char *name, struct sdsx_list *values);

/* Attempt to find a CRC image.  If name starts with '@', search the named
 * list for the first accessible entry, otherwise search for a single entry. */
int crclist_match(const char *name, uint32_t crc);

/* Format a CRC list (e.g. "@coco3=0xb4c88d6c,0xff050d80") into buf. */
int crclist_snprintf(char *buf, size_t n, const char *name);

/* Print a list of defined CRC lists to stdout */
void crclist_print_all(FILE *f);
/* Print list and exit */
void crclist_print(void);

/* Tidy up */
void crclist_shutdown(void);

#endif
