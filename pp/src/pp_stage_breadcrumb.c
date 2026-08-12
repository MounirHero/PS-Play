#include "pp_stage_breadcrumb.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t bc_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

void pp_stage_bc(const char *stage_id, const char *detail)
{
    FILE *f;
    if (!stage_id)
        return;
    f = fopen(PP_STAGE_BC_PATH, "a");
    if (!f)
        return;
    fprintf(f, "%llu %s%s%s\n", (unsigned long long)bc_now_us(), stage_id,
            (detail && detail[0]) ? " " : "", detail ? detail : "");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

void pp_stage_bc_checkpoint(const char *stage_id, const char *detail)
{
    FILE *f;
    /* Append full history */
    pp_stage_bc(stage_id, detail);
    /* Last-alive single-line file for quick pull */
    f = fopen("/mnt/usb0/pp_4k_stage_last.txt", "w");
    if (!f)
        return;
    fprintf(f, "last=%s detail=%s ts=%llu\n", stage_id, detail ? detail : "",
            (unsigned long long)bc_now_us());
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}
