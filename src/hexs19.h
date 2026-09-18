/** \file
 *
 *  \brief Support for various binary representations.
 *
 *  \copyright Copyright 2003-2016 Ciaran Anscomb
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
 *
 *  Supports:
 *
 *  - Intel HEX
 *
 *  - DragonDOS binary
 *
 *  - CoCo RS-DOS ("DECB") binary
 */

#ifndef XROAR_HEXS19_H_
#define XROAR_HEXS19_H_

struct decb_bin;

int intel_hex_read(const char *filename, int autorun);
int motorola_s19_read(const char *filename, int autorun);
int bin_load(const char *filename, int autorun);

/* Poke a parsed DECB/DragonDOS image through the current machine CPU map
 * and optionally set PC from the postamble EXEC address. */
int bin_apply_image(const struct decb_bin *bin, int autorun);

#endif
