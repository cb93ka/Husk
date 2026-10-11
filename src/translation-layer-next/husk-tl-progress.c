/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What a game's start is doing right now, for the app's loading screen: a stage in words ("Loading libunity.so", "Unpacking the
 * game's data") and how far along it is, 0..1, or -1 when there is no telling. Written by whatever part of the start is busy.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_stage[160];
static double g_fraction = -1;

void tl_progress(const char *stage, double fraction)
{
    pthread_mutex_lock(&g_lock);
    if (stage) snprintf(g_stage, sizeof(g_stage), "%s", stage);
    g_fraction = fraction;
    pthread_mutex_unlock(&g_lock);
}

double husk_tl_progress(char *out, unsigned long cap)
{
    pthread_mutex_lock(&g_lock);
    if (out && cap) snprintf(out, cap, "%s", g_stage);
    double f = g_fraction;
    pthread_mutex_unlock(&g_lock);
    return f;
}
