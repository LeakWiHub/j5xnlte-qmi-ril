/* SPDX-License-Identifier: Apache-2.0 */
/* Hosted-only logging hook (not linked into the freestanding probe). */
#include <stdarg.h>
#include <stdio.h>

#include "qmi/qmi_codec.h"

static qmi_log_fn g_log;
static int g_max_level = QMI_LOG_INFO;

void qmi_set_logger(qmi_log_fn fn, int max_level)
{
    g_log = fn;
    g_max_level = max_level;
}

void qmi_logf(int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!g_log || level > g_max_level) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(level, buf);
}
