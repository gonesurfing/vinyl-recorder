#include "rate_mon.h"

void rate_mon_init(rate_mon_t *m, unsigned nominal_rate) {
    m->nominal = (double)nominal_rate;
    rate_mon_reset(m);
}

void rate_mon_reset(rate_mon_t *m) {
    m->head = m->count = 0;
    m->have_origin = false;
    m->t0 = 0.0;
    m->f0 = 0;
}

static size_t idx(const rate_mon_t *m, size_t i) {
    return (m->head + i) % RATE_MON_CAP;
}

void rate_mon_add(rate_mon_t *m, double t_sec, uint64_t frames) {
    if (!m->have_origin) {
        m->have_origin = true;
        m->t0 = t_sec;
        m->f0 = frames;
    }
    // Relative to the origin so doubles keep sub-frame precision for hours.
    double t = t_sec - m->t0;
    double f = (double)(frames - m->f0);

    if (m->count > 0 && t - m->t[idx(m, m->count - 1)] < RATE_MON_MIN_STEP_S)
        return;

    // Age out points older than the window, and make room if still full.
    while (m->count > 0 && t - m->t[m->head] > RATE_MON_WINDOW_S) {
        m->head = (m->head + 1) % RATE_MON_CAP;
        m->count--;
    }
    if (m->count == RATE_MON_CAP) {
        m->head = (m->head + 1) % RATE_MON_CAP;
        m->count--;
    }
    size_t j = idx(m, m->count);
    m->t[j] = t;
    m->f[j] = f;
    m->count++;
}

bool rate_mon_ppm(const rate_mon_t *m, double *ppm) {
    if (m->count < 2 || m->nominal <= 0.0) return false;
    double span = m->t[idx(m, m->count - 1)] - m->t[m->head];
    if (span < RATE_MON_MIN_SPAN_S) return false;

    double mt = 0.0, mf = 0.0;
    for (size_t i = 0; i < m->count; i++) {
        mt += m->t[idx(m, i)];
        mf += m->f[idx(m, i)];
    }
    mt /= (double)m->count;
    mf /= (double)m->count;

    double sxy = 0.0, sxx = 0.0;
    for (size_t i = 0; i < m->count; i++) {
        double dt = m->t[idx(m, i)] - mt;
        sxy += dt * (m->f[idx(m, i)] - mf);
        sxx += dt * dt;
    }
    if (sxx <= 0.0) return false;
    double rate = sxy / sxx;               // frames per second
    *ppm = (rate / m->nominal - 1.0) * 1e6;
    return true;
}

bool rate_mon_suspect(double ppm) {
    return ppm > RATE_MON_WARN_PPM || ppm < -RATE_MON_WARN_PPM;
}
