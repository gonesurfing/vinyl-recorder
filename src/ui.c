#define _XOPEN_SOURCE_EXTENDED 1
#include "ui.h"
#include "level.h"
#include <locale.h>
#if defined(__APPLE__)
#include <ncurses.h>
#else
#include <ncursesw/ncurses.h>
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>

#define COLOR_PAIR_GREEN  1
#define COLOR_PAIR_YELLOW 2
#define COLOR_PAIR_RED    3
#define COLOR_PAIR_DIM    4
#define COLOR_PAIR_BOLD   5

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(COLOR_PAIR_GREEN,  COLOR_GREEN,  -1);
    init_pair(COLOR_PAIR_YELLOW, COLOR_YELLOW, -1);
    init_pair(COLOR_PAIR_RED,    COLOR_RED,    -1);
    init_pair(COLOR_PAIR_DIM,    COLOR_WHITE,  -1);
    init_pair(COLOR_PAIR_BOLD,   COLOR_WHITE,  -1);
}

static int dbfs_to_col(float db, int width) {
    // Map -90..0 dBFS onto 0..width-1 with a non-linear scale:
    // -90..-60 takes 1/4 of the bar, -60..0 takes 3/4.
    if (db <= -90.0f) return 0;
    if (db >= 0.0f)   return width - 1;
    float s;
    if (db < -60.0f) s = (db + 90.0f) / 30.0f * 0.25f;
    else             s = 0.25f + (db + 60.0f) / 60.0f * 0.75f;
    int c = (int)(s * (width - 1));
    if (c < 0) c = 0;
    if (c > width - 1) c = width - 1;
    return c;
}

static void draw_bar(WINDOW *w, int row, int col, int width,
                     float bar_db, float hold_db) {
    int filled  = dbfs_to_col(bar_db,  width);
    int peakcol = dbfs_to_col(hold_db, width);
    int yellow_start = dbfs_to_col(-18.0f, width);
    int red_start    = dbfs_to_col(-3.0f,  width);

    for (int i = 0; i < width; i++) {
        int pair;
        if      (i < yellow_start) pair = COLOR_PAIR_GREEN;
        else if (i < red_start)    pair = COLOR_PAIR_YELLOW;
        else                        pair = COLOR_PAIR_RED;

        chtype ch;
        if (i == peakcol)         ch = '|' | A_BOLD;
        else if (i <= filled)     ch = ACS_BLOCK;
        else                      ch = ' ';

        wattron(w, COLOR_PAIR(pair));
        mvwaddch(w, row, col + i, ch);
        wattroff(w, COLOR_PAIR(pair));
    }
}

static void draw_scale(WINDOW *w, int row, int col, int width) {
    static const int marks[] = { -90, -60, -40, -30, -18, -12, -6, -3, 0 };
    for (size_t i = 0; i < sizeof(marks)/sizeof(marks[0]); i++) {
        int c = dbfs_to_col((float)marks[i], width);
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", marks[i]);
        int len = (int)strlen(tmp);
        int put = col + c - len/2;
        if (put < col) put = col;
        if (put + len >= col + width) put = col + width - len;
        mvwprintw(w, row, put, "%s", tmp);
    }
}

static const char *state_str(int s, int encoder_active) {
    if (encoder_active && s == REC_IDLE) return "ENCODING";
    switch (s) {
        case REC_IDLE:     return "IDLE";
        case REC_RUNNING:  return "REC";
        case REC_STOPPING: return "STOPPING";
    }
    return "?";
}

void ui_run(ui_args_t *args) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_colors();

    // The audio + recorder threads start before initscr() and may fprintf to
    // stderr (e.g. CoreAudio init messages) while ncurses is taking over the
    // terminal. ncurses doesn't see those writes, so its internal screen
    // model is stale and the first frame leaves residue. Force a full
    // clear+repaint on the first refresh — same thing ncurses does
    // internally on SIGWINCH, which is why a resize "fixes" it.
    clearok(stdscr, TRUE);

    ppm_t lp, rp;
    ppm_init(&lp);
    ppm_init(&rp);

    double last = now_seconds();
    double rec_started_at = 0.0;
    int    last_state = REC_IDLE;

    const double frame_dt = 1.0 / UI_REFRESH_HZ;

    while (!atomic_load(&args->st->quit)) {
        double t = now_seconds();
        float dt = (float)(t - last);
        last = t;

        // Read latest peaks; drive PPM ballistics.
        float pl = (float)atomic_load(&args->st->peak_left);
        float pr = (float)atomic_load(&args->st->peak_right);
        ppm_update(&lp, pl, dt);
        ppm_update(&rp, pr, dt);

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (rows < 8 || cols < 50) {
            erase();
            mvprintw(0, 0, "Terminal too small (need >=50x8)");
            refresh();
        } else {
            erase();
            int bar_col   = 4;
            int bar_width = cols - bar_col - 14;

            // Prefer the rate the audio thread actually negotiated (ALSA may
            // snap to a neighboring rate); fall back to the requested rate
            // until the audio thread has opened the device.
            unsigned neg = atomic_load(&args->st->negotiated_rate);
            int display_rate = (neg != 0) ? (int)neg : args->rate;

            mvprintw(0, 0, "%s %s", APP_NAME, APP_VERSION);
            mvprintw(0, cols - 30, "device: %s", args->device);
            mvprintw(1, 0, "rate: %d Hz   output: %s", display_rate, args->output_dir);

            draw_scale(stdscr, 3, bar_col, bar_width);

            mvprintw(4, 0, "L");
            draw_bar(stdscr, 4, bar_col, bar_width,
                     dbfs_from_linear(lp.bar), dbfs_from_linear(lp.hold));
            mvprintw(4, bar_col + bar_width + 1, "%6.1f dB", dbfs_from_linear(lp.bar));

            mvprintw(5, 0, "R");
            draw_bar(stdscr, 5, bar_col, bar_width,
                     dbfs_from_linear(rp.bar), dbfs_from_linear(rp.hold));
            mvprintw(5, bar_col + bar_width + 1, "%6.1f dB", dbfs_from_linear(rp.bar));

            int rs = atomic_load(&args->st->rec_state);
            int enc = atomic_load(&args->st->encoder_active);
            unsigned long fr = (unsigned long)atomic_load(&args->st->frames_recorded);
            int xrun = atomic_load(&args->st->xrun_count);
            int clip = atomic_load(&args->st->clip_count);

            if (rs == REC_RUNNING && last_state != REC_RUNNING)
                rec_started_at = t;
            last_state = rs;

            double elapsed = (rs == REC_RUNNING) ? (t - rec_started_at)
                                                  : (double)fr / (double)display_rate;

            mvprintw(7, 0,
                     "state: %-8s  elapsed: %6.1f s  frames: %lu  xruns: %d  clips: %d",
                     state_str(rs, enc), elapsed, fr, xrun, clip);

            mvprintw(rows - 1, 0, "[r] start/stop   [n] new track   [q] quit");
        }

        wnoutrefresh(stdscr);
        doupdate();

        // Input.
        int ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case 'q': case 'Q':
                    atomic_store(&args->st->quit, 1);
                    break;
                case 'r': case 'R': case ' ': {
                    int rs = atomic_load(&args->st->rec_state);
                    if (rs == REC_IDLE)         atomic_store(&args->st->rec_request, 1);
                    else if (rs == REC_RUNNING) atomic_store(&args->st->rec_request, 2);
                    break;
                }
                case 'n': case 'N':
                    if (atomic_load(&args->st->rec_state) == REC_RUNNING)
                        atomic_store(&args->st->rec_request, 3);
                    break;
            }
        }

        // Sleep until next frame.
        double sleep_for = frame_dt - (now_seconds() - t);
        if (sleep_for > 0) {
            struct timespec ts = {
                .tv_sec  = (time_t)sleep_for,
                .tv_nsec = (long)((sleep_for - (time_t)sleep_for) * 1e9),
            };
            nanosleep(&ts, NULL);
        }
    }

    endwin();
}
