/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"

#include <errno.h>
#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "ui/xemu-settings.h"


/* XEMU_UNREAL_CHAMPIONSHIP_REPORT_BEGIN
 * Per-frame performance report in TSV format.
 */
static FILE *nv2a_report_file;
static bool nv2a_report_open_attempted;
static int64_t nv2a_report_start_us;
static uint64_t nv2a_report_frame_number;

static void nv2a_profile_report_open(void)
{
    if (nv2a_report_open_attempted) {
        return;
    }
    nv2a_report_open_attempted = true;

    const char *base_path = xemu_settings_get_base_path();
    if (!base_path || !base_path[0]) {
        fprintf(stderr, "xemu: performance report path unavailable\n");
        return;
    }

    if (g_mkdir_with_parents(base_path, 0755) != 0) {
        fprintf(stderr, "xemu: cannot create report directory '%s': %s\n",
                base_path, g_strerror(errno));
        return;
    }

    gchar *path = g_build_filename(
        base_path, "unreal-championship-report.txt", NULL);
    nv2a_report_file = g_fopen(path, "w");
    if (!nv2a_report_file) {
        fprintf(stderr, "xemu: cannot open report '%s': %s\n",
                path, g_strerror(errno));
        g_free(path);
        return;
    }

    fputs("# xemu per-frame performance report\n"
          "# TSV; one row per NV2A flip-stall sample\n"
          "elapsed_ms\tframe\tframe_interval_us\tfps_estimate\tmspf"
          "\tsurface_update_us\trenderer_flip_stall_us"
          "\tshader_gen_us\ttex_upload_us",
          nv2a_report_file);

    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        fprintf(nv2a_report_file, "\t%s",
                nv2a_profile_get_counter_name(i));
    }
    fputc('\n', nv2a_report_file);
    fflush(nv2a_report_file);

    fprintf(stderr, "xemu: performance report: %s\n", path);
    g_free(path);
}

static void nv2a_profile_report_frame(
    int mspf, int64_t frame_interval_us,
    int surface_update_us, int renderer_flip_stall_us,
    int shader_gen_us, int tex_upload_us,
    const int *counters, int64_t now_us)
{
    nv2a_profile_report_open();
    if (!nv2a_report_file) {
        return;
    }

    if (nv2a_report_start_us == 0) {
        nv2a_report_start_us = now_us;
    }
    nv2a_report_frame_number++;

    double elapsed_ms =
        (double)(now_us - nv2a_report_start_us) / 1000.0;
    double fps = frame_interval_us > 0
        ? 1000000.0 / (double)frame_interval_us : 0.0;

    fprintf(nv2a_report_file, "%.3f\t%llu\t%lld\t%.3f\t%d\t%d\t%d\t%d\t%d",
            elapsed_ms,
            (unsigned long long)nv2a_report_frame_number,
            (long long)frame_interval_us, fps, mspf,
            surface_update_us, renderer_flip_stall_us,
            shader_gen_us, tex_upload_us);

    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        fprintf(nv2a_report_file, "\t%d", counters[i]);
    }
    fputc('\n', nv2a_report_file);

    if (nv2a_report_frame_number % 60 == 0) {
        fflush(nv2a_report_file);
    }
}
/* XEMU_UNREAL_CHAMPIONSHIP_REPORT_END */

NV2AStats g_nv2a_stats;

void nv2a_profile_increment(void)
{
    int64_t now = g_get_monotonic_time();
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

void nv2a_profile_flip_stall(void)
{
    int64_t now = g_get_monotonic_time();
    static int64_t previous_flip_stall_us;
    int64_t interval = previous_flip_stall_us > 0 &&
                       now >= previous_flip_stall_us
        ? now - previous_flip_stall_us : 0;
    previous_flip_stall_us = now;

    g_nv2a_stats.frame_working.frame_interval_us = interval;
    g_nv2a_stats.frame_working.mspf =
        (now - g_nv2a_stats.last_flip_time) / 1000;

    unsigned int idx = g_nv2a_stats.frame_ptr;
    g_nv2a_stats.frame_history[idx] = g_nv2a_stats.frame_working;

    nv2a_profile_report_frame(
        g_nv2a_stats.frame_history[idx].mspf,
        g_nv2a_stats.frame_history[idx].frame_interval_us,
        g_nv2a_stats.frame_history[idx].surface_update_us,
        g_nv2a_stats.frame_history[idx].renderer_flip_stall_us,
        g_nv2a_stats.frame_history[idx].shader_gen_us,
        g_nv2a_stats.frame_history[idx].tex_upload_us,
        g_nv2a_stats.frame_history[idx].counters, now);

    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
