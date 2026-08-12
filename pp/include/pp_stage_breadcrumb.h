/*
 * Durable stage breadcrumbs for 4K crash isolation.
 * Written immediately to USB — not only on clean exit.
 */
#ifndef PP_STAGE_BREADCRUMB_H
#define PP_STAGE_BREADCRUMB_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PP_STAGE_BC_PATH
#define PP_STAGE_BC_PATH "/mnt/usb0/pp_4k_stage_breadcrumb.txt"
#endif

/** Append one stage line + flush. Safe to call frequently. */
void pp_stage_bc(const char *stage_id, const char *detail);

/** Overwrite with a single "last alive" marker (also appends history). */
void pp_stage_bc_checkpoint(const char *stage_id, const char *detail);

#ifdef __cplusplus
}
#endif

#endif
