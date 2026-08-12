/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#pragma once

#include <stdint.h>
#include <unistd.h>


pid_t hbldr_launch(const char* cwd, const char* path, int stdio, char** argv,
		   char** envp);

/*
 * Create and validate the system BigApp host used by hbldr. This is exposed so
 * the standalone launcher can fail early instead of publishing a dead tile.
 */
int hbldr_prepare_host(void);

/*
 * Remove the Prospero system BigApp host under /system_ex/app/<titleId>.
 * Remounts system_ex when needed. Safe to call if the host is already gone.
 */
int hbldr_remove_host(void);

/*
 * Launch an ELF already resident in the payload image. The buffer remains
 * owned by the caller and only needs to remain valid until this returns.
 */
pid_t hbldr_launch_buffer(const char* cwd, const char* path, int stdio,
                          char** argv, char** envp, const uint8_t* elf);
