/* Frame pacing and GPU load governor: pure decision logic (gpu_pace.h). */
#include "gpu_pace.h"

#include <math.h>
#include <string.h>

/* ---- present delay ---------------------------------------------------------------------------------------------- */
void pace_delay_init(pace_delay* d, double period_ms) {
    memset(d, 0, sizeof *d);
    d->period_ms = period_ms > 0 ? period_ms : PACE_PERIOD_MS;
    d->delay_ms = 0.5 * d->period_ms;    /* (the first pictures move it to what they need) */
    d->min_ms = 2.0;
    d->max_ms = 3.0 * d->period_ms;
    d->margin_ms = 1.5;
}

static void sortf(float* v, int n) {
    for (int i = 1; i < n; i++) {
        float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

double pace_delay_quantile(const pace_delay* d, double q) {
    if (d->nlat <= 0) return 0;
    float v[PACE_NLAT];
    memcpy(v, d->lat, (size_t)d->nlat * sizeof v[0]);
    sortf(v, d->nlat);
    int i = (int)(q * (double)(d->nlat - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= d->nlat) i = d->nlat - 1;
    return v[i];
}

void pace_delay_sample(pace_delay* d, double ready_ms) {
    if (!(ready_ms >= 0) || ready_ms > 10.0 * d->period_ms) return;   /* (NaN, or a pause: not a pacing sample) */
    d->lat[d->lpos] = (float)ready_ms;
    d->lpos = (d->lpos + 1) % PACE_NLAT;
    if (d->nlat < PACE_NLAT) d->nlat++;
    d->samples++;
    double target = (d->nlat < 8 ? ready_ms : pace_delay_quantile(d, 0.95)) + d->margin_ms;
    if (target < d->min_ms) target = d->min_ms;
    if (target > d->max_ms) target = d->max_ms;
    if (d->nlat < 8) { if (target > d->delay_ms) d->delay_ms = target; return; }
    if (target > d->delay_ms) d->delay_ms = target;
    else d->delay_ms = d->delay_ms - 0.05 < target ? target : d->delay_ms - 0.05;
}

/* ---- present flags ---------------------------------------------------------------------------------------------- */
int pace_present_flags(int vsync, int tearing_ok, double now_ms, double target_ms, int* tear) {
    *tear = 0;
    switch (vsync) {
    case PACE_VSYNC_OFF:
        *tear = tearing_ok != 0;
        return 0;
    case PACE_VSYNC_ADAPTIVE:
        if (target_ms > 0 && now_ms - target_ms > 2.0) { *tear = tearing_ok != 0; return 0; }   /* late: show it now */
        return 1;
    default:
        return 1;
    }
}

/* ---- display ---------------------------------------------------------------------------------------------------- */
void pace_disp_init(pace_disp* p, double refresh_ms, double content_ms) {
    memset(p, 0, sizeof *p);
    p->refresh_ms = refresh_ms;
    p->content_ms = content_ms > 0 ? content_ms : PACE_PERIOD_MS;
}

void pace_disp_interval(pace_disp* p, double interval_ms) {
    /* only intervals near the content cadence say anything (a late or a repeated picture does not) */
    if (!(interval_ms > 0.5 * p->content_ms && interval_ms < 1.5 * p->content_ms)) return;
    p->iv[p->pos] = (float)interval_ms;
    p->pos = (p->pos + 1) % PACE_NIV;
    if (p->n < PACE_NIV) p->n++;
}

int pace_refresh_multiple(double refresh_ms, double content_ms) {
    if (refresh_ms <= 0 || content_ms <= 0) return 0;
    int k = (int)floor(content_ms / refresh_ms + 0.5);
    if (k < 1 || k > 4) return 0;
    return fabs(k * refresh_ms - content_ms) / content_ms < 0.005 ? k : 0;
}

int pace_disp_classify(const pace_disp* p) {
    if (p->refresh_ms <= 0 || p->n < 60) return PACE_DISP_UNKNOWN;
    if (pace_refresh_multiple(p->refresh_ms, p->content_ms)) return PACE_DISP_MULTIPLE;
    float f[PACE_NIV], dev[PACE_NIV];
    for (int i = 0; i < p->n; i++) {
        double r = p->iv[i] / p->refresh_ms;
        f[i] = (float)fabs(r - floor(r + 0.5));
        dev[i] = (float)fabs(p->iv[i] - p->content_ms);
    }
    sortf(f, p->n);
    sortf(dev, p->n);
    double fm = f[p->n / 2], dm = dev[p->n / 2];
    if (fm < 0.08) return PACE_DISP_FIXED;
    if (fm > 0.12 && dm < 3.0) return PACE_DISP_VRR;   /* (3 ms: presents at their ready time vary that much)*/
    return PACE_DISP_UNKNOWN;
}

/* ---- governor --------------------------------------------------------------------------------------------------- */
/* Relative GPU time per scale (16:9 display targets), measured at full GPU clocks (gs_replay back to back, RTX 4060 Ti,
 * pc-pacing): GPU ms per game frame, median of ep1 / ep3 / ep5 hubs and ep7 / ep8 bosses: 1x 2.0, 2x 5.5, 3x 9.2, 4x 16.6;
 * ep1 / ep3 hubs and ep7 boss: 5x 25.9, 6x 35.2, 8x 64.8 (vs 3x 9.9); 7x interpolated. */
double pace_gov_cost(int n, int s) {
    static const double c[9] = { 0, 1.0, 2.7, 4.6, 8.1, 12.0, 16.3, 22.0, 29.4 };
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    return c[n] / c[s];
}

int pace_gov_fit(int cur, double load, double fit) {
    int to = cur - 1;
    while (to > 1 && load * pace_gov_cost(to, cur) > fit) to--;
    return to < 1 ? 1 : to;
}

#define GOV_SKIP 3                       /* pictures after a change not judged (the rescale itself) */
/* pictures after a light second (or the start) in which the overload rule does not judge: a GPU that was idle runs at its
 * lowest clocks and needs a few hundred ms to raise them (NVIDIA: the first 3D frames after the logos took 50-70 ms) */
#define GOV_WARM 60
#define GOV_SETTLE 3                     /* seconds after a change not judged by the per-second rules */
/* per-second rules: auto keeps headroom (down above 90% for 3 s, to 80%; up when the next scale is predicted at
 * <= 75%); a fixed scale is only lowered when it does not fit (above 95% for 2 s, to 85%) and raised back towards
 * the player's scale when that is predicted at <= 80% */
#define AUTO_HIGH 0.90
#define AUTO_DOWN_SECS 3
#define AUTO_FIT 0.80
#define AUTO_UP 0.75
#define OVER_FIT 0.85                    /* the overload rule's target (both modes): it reacts to a short window */
#define FIXED_HIGH 1.0
#define FIXED_DOWN_SECS 2
#define FIXED_FIT 0.85
#define FIXED_UP 0.80

void pace_gov_init(pace_gov* g, int max, int fixed, int start) {
    memset(g, 0, sizeof *g);
    g->max = max < 1 ? 1 : max > 8 ? 8 : max;
    g->fixed = fixed;
    g->cur = start < 1 ? 1 : start > g->max ? g->max : start;
    g->retry_secs = 120;
    g->slow = 1.0;
    g->sec_t0 = -1;
    g->warm = GOV_WARM;
}

static void gov_change(pace_gov* g, int to, int reason, double load) {
    if (to < g->cur && (reason == PACE_GOV_OVERLOAD || reason == PACE_GOV_HIGH)) {   /* left for being too slow: back
                                                                                         off before it is tried again */
        g->backoff[g->cur] = g->left_at[g->cur] ? 2 * g->backoff[g->cur] : g->retry_secs;
        g->left_at[g->cur] = g->secs + 1;
    }
    g->cur = to;
    g->nfr = g->fpos = 0;
    g->skip = GOV_SKIP;
    g->over = 0;
    g->nhist = 0;
    g->settle = GOV_SETTLE;
    g->sec_gpu = 0;
    g->sec_frames = 0;
    g->sec_skipped = 0;
    g->sec_stall_ms = 0;
    g->changes++;
    g->reason = reason;
    g->reason_load = load;
}

static int in_backoff(const pace_gov* g, int n, uint32_t window) {
    return n >= 1 && n <= 8 && g->left_at[n] && g->secs + 1 - g->left_at[n] <= window;
}

void pace_gov_set(pace_gov* g, int max, int fixed, double now_ms) {
    (void)now_ms;
    if (max < 1) max = 1;
    if (max > 8) max = 8;
    g->max = max;
    g->fixed = fixed;
    int to = max;                        /* the player's scale, unless it just failed (fixed: 10 s; auto: its back-off) */
    while (to > 1 && in_backoff(g, to, fixed ? 10 : g->backoff[to])) to--;
    if (to != g->cur) gov_change(g, to, PACE_GOV_MAX, g->load);
}

void pace_gov_evidence(pace_gov* g, int skipped, double stall_ms) {
    g->sec_skipped += skipped;
    g->sec_stall_ms += stall_ms;
}

int pace_gov_frame(pace_gov* g, double gpu_ms, double now_ms) {
    if (!(gpu_ms >= 0)) return 0;
    gpu_ms *= g->slow;
    double period = PACE_PERIOD_MS;
    if (g->skip > 0) { g->skip--; g->sec_t0 = now_ms; g->sec_gpu = 0; g->sec_frames = 0; return 0; }
    g->fr[g->fpos] = (float)gpu_ms;
    g->fpos = (g->fpos + 1) % PACE_GOV_NFR;
    if (g->nfr < PACE_GOV_NFR) g->nfr++;
    /* overload: the GPU cannot draw this scale in real time - pictures queue up and the game would wait for them. Far
     * over (the last 12 pictures over 2.5 periods on average, the last 24 over 1.4) it is lowered at once, so the game
     * slows for about a second at most; a short burst of slow pictures (loading, a GPU clock change, another program
     * using the GPU) is absorbed by the render queue and judged by the per-second rules below. */
    if (g->warm > 0) g->warm--;
    else if (g->cur > 1) {
        double s12 = 0, s24 = 0;
        for (int i = 1; i <= PACE_GOV_NFR && i <= g->nfr; i++) {
            double v = g->fr[(g->fpos - i + PACE_GOV_NFR) % PACE_GOV_NFR];
            if (i <= 12) s12 += v;
            s24 += v;
        }
        double load = 0;
        if (g->nfr >= 12 && s12 / 12 > 2.5 * period) load = s12 / 12 / period;
        if (g->nfr >= 24 && s24 / 24 > 1.4 * period && s24 / 24 / period > load) load = s24 / 24 / period;
        if (load > 0) {
            int to = pace_gov_fit(g->cur, load, OVER_FIT);
            gov_change(g, to, PACE_GOV_OVERLOAD, load);
            return to;
        }
    }
    /* per-second rules */
    if (g->sec_t0 < 0) g->sec_t0 = now_ms;
    g->sec_gpu += gpu_ms;
    g->sec_frames++;
    if (now_ms - g->sec_t0 < 1000.0) return 0;
    double load = g->sec_frames ? g->sec_gpu / g->sec_frames / period : 0;
    int frames = g->sec_frames;
    /* the evidence that the GPU did not keep up whatever its timestamps say (another program using the GPU too: the
     * frames' own GPU time looks fine while the queue grows): the game waited for the render queue, or pictures were
     * skipped because the renderer was behind - such a second counts as overloaded (105%) */
    double behind = frames ? (double)g->sec_skipped / frames : 0;
    g->last_behind = behind;
    g->last_stall_ms = g->sec_stall_ms;
    /* (only while the GPU is busy enough to be the cause: a present held up by the display or the system with an idle GPU is
     * not a reason to draw less) */
    int stalled = g->sec_stall_ms >= 50.0 && load >= 0.7;
    int forced = (behind >= 0.10 || g->sec_stall_ms >= 50.0) && load >= 0.7 && load < 1.05;   /* (then one step down) */
    double raw = load;
    if (forced) load = 1.05;
    g->sec_t0 = now_ms;
    g->sec_gpu = 0;
    g->sec_frames = 0;
    g->sec_skipped = 0;
    g->sec_stall_ms = 0;
    if (load < 0.15) g->warm = GOV_WARM;     /* (a light second: the GPU may lower its clocks) */
    if (frames < 10) return 0;
    g->load = load;
    g->secs++;
    if (g->settle > 0) { g->settle--; return 0; }
    /* the game is waiting for the GPU right now (the queue is full) and the GPU is (nearly) saturated: lower it at once */
    if (stalled && raw >= 0.9 && g->cur > 1) {
        int to = pace_gov_fit(g->cur, raw > 1.05 ? raw : 1.05, OVER_FIT);
        gov_change(g, to, PACE_GOV_OVERLOAD, raw);
        return to;
    }
    g->hist[g->nhist % PACE_GOV_UP_SECS] = load;
    g->nhist++;
    int to = g->cur;
    double high = g->fixed ? FIXED_HIGH : AUTO_HIGH;
    int down_secs = g->fixed ? FIXED_DOWN_SECS : AUTO_DOWN_SECS;
    if (load > high && g->cur > 1) {
        if (++g->over >= down_secs) to = forced ? g->cur - 1 : pace_gov_fit(g->cur, load, g->fixed ? FIXED_FIT : AUTO_FIT);
    } else g->over = 0;
    if (to == g->cur && g->cur < g->max && g->nhist >= PACE_GOV_UP_SECS && !in_backoff(g, g->cur + 1, g->backoff[g->cur + 1])) {
        double v[PACE_GOV_UP_SECS];      /* the lower quartile: a lightly loaded GPU lowers its clocks (NVIDIA P-states)
                                            and then measures up to ~3x longer in some seconds */
        memcpy(v, g->hist, sizeof v);
        for (int i = 1; i < PACE_GOV_UP_SECS; i++) {
            double x = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
            v[j + 1] = x;
        }
        double q = v[PACE_GOV_UP_SECS / 4];
        if (q * pace_gov_cost(g->cur + 1, g->cur) <= (g->fixed ? FIXED_UP : AUTO_UP)) to = g->cur + 1;
    }
    if (g->cur > g->max) to = g->max;
    if (to == g->cur) return 0;
    gov_change(g, to, to > g->cur ? PACE_GOV_FITS : to == g->max && g->cur > g->max ? PACE_GOV_MAX : PACE_GOV_HIGH, load);
    return to;
}

