/*
 * qcnpuperf_daemon.c — polls DSP performance metrics every 500 ms and writes
 * a fresh snapshot to /dev/shm/qcnpuperf.
 *
 * The file is replaced atomically via rename(2) on every cycle so readers
 * always see a complete, single-snapshot view — no partial writes, no history.
 * The file is removed on clean shutdown (SIGTERM / SIGINT).
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qcom_dsp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_PATH      "/dev/shm/qcnpuperf"
#define OUTPUT_TMP_PATH  "/dev/shm/.qcnpuperf.tmp"
#define POLL_INTERVAL_NS (500L * 1000L * 1000L)   /* 500 ms */

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART is intentionally absent: allows nanosleep to be interrupted
     * by the signal so the main loop exits promptly. */

    if (sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGINT,  &sa, NULL) != 0) {
        perror("qcnpuperfd: sigaction");
        return -1;
    }
    return 0;
}

/*
 * Atomically replace OUTPUT_PATH with a fresh metrics snapshot.
 * Writes to a temp path first, then rename(2) into place.
 * Returns 0 on success, -1 on any error (non-fatal; next cycle retries).
 */
static int write_metrics(const struct sysmon_query_prof_data *d)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "q6_utilization=%.2f\n"
        "q6_clock_khz=%u\n"
        "hvx_utilization=%.2f\n"
        "hmx_utilization=%.2f\n",
        d->q6_utilization,
        d->q6_clock,
        d->hvx_utilization,
        d->hmx_utilization);

    if (len < 0 || (size_t)len >= sizeof(buf)) {
        fprintf(stderr, "qcnpuperfd: snprintf overflow\n");
        return -1;
    }

    int fd = open(OUTPUT_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("qcnpuperfd: open tmp");
        return -1;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    int saved_errno = errno;
    close(fd);

    if (written != (ssize_t)len) {
        errno = saved_errno;
        perror("qcnpuperfd: write");
        unlink(OUTPUT_TMP_PATH);
        return -1;
    }

    if (rename(OUTPUT_TMP_PATH, OUTPUT_PATH) != 0) {
        perror("qcnpuperfd: rename");
        unlink(OUTPUT_TMP_PATH);
        return -1;
    }

    return 0;
}

int main(void)
{
    if (setup_signals() != 0)
        return 1;

    enum DspReturnCode rc = qcom_dsp_init(DSP_NPU0);
    if (rc != RETURN_CODE_DSP_LIB_SUCCESS) {
        fprintf(stderr, "qcnpuperfd: qcom_dsp_init failed (code %d)\n", (int)rc);
        return 1;
    }

    const struct timespec interval = {
        .tv_sec  = 0,
        .tv_nsec = POLL_INTERVAL_NS,
    };

    while (g_running) {
        int no_metrics = 0;
        struct sysmon_query_prof_data *data =
            qcom_dsp_get_prof_data(DSP_NPU0, &no_metrics);

        if (data != NULL && no_metrics > 0) {
            write_metrics(data);
        }

        /* nanosleep interrupted by signal (EINTR) is the normal exit path */
        if (nanosleep(&interval, NULL) != 0 && errno != EINTR) {
            perror("qcnpuperfd: nanosleep");
        }
    }

    qcom_dsp_deinit(DSP_NPU0);
    unlink(OUTPUT_PATH);
    unlink(OUTPUT_TMP_PATH); /* clean up if a write was interrupted */

    return 0;
}
