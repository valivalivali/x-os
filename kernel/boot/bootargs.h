#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* XNU-inspired boot-args (see xnu pexpert/gen/bootargs.c — reimplemented).
 * Parsed from Limine kernel cmdline. Examples:
 *   -v              verbose boot (serial [boot] logs)
 *   -x              safe-ish / reduced feature boot (reserved)
 *   serial=3        keep serial console hot (informational)
 *   debug=0x...     reserved for future
 */

#define BOOT_ARGS_MAX 512

void bootargs_init(const char *cmdline);

/* True if flag present (e.g. "-v"). */
bool bootargs_has(const char *name);

/* Parse name[=value]. Boolean flags (-v) write 1. Returns true if found. */
bool bootargs_parse(const char *name, void *out, size_t out_len);

const char *bootargs_raw(void);
bool bootargs_verbose(void);
