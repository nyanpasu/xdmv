#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>

/* Config */
#define xdmv_refresh_rate 1000/60
#define xdmv_sample_size 48000
/* TODO Replace these with functions that get these values dynamically/from
 * configs */
#define xdmv_height 100
#define xdmv_width 1080
#define xdmv_margin 2
#define xdmv_offset_y 20
#define xdmv_offset_x 0
#define xdmv_box_count 40

/* Utils */
#define eprintf(...) fprintf(stderr, __VA_ARGS__);

#define dieifnull(p, reason) _dieifnull((p), (reason), __LINE__);
void
_dieifnull(void *p, const char *reason, int line)
{
    if (!p) {
        eprintf("line %d: %s\n", line, reason);
        exit(1);
    }
}

#define dieif(n, reason) _dieif((n), (reason), __LINE__);
void
_dieif(int n, const char *reason, int line)
{
    if (n) {
        eprintf("line %d: %s\n", line, reason);
        exit(1);
    }
}

#define die(reason) _die((reason), __LINE__);
void
_die(const char *reason, int line)
{
    eprintf("%d: %s\n", line, reason);
    exit(1);
}

void *
xmalloc(size_t sz)
{
    void *p = malloc(sz);
    if (!p)
        die("Could not allocate memory");
    
    return p;
}

/* State */
struct wav_header {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt[4];
    uint32_t format_size;
    uint16_t type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t _1;
    uint16_t _2;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};

struct wav_header xrdb_wav_header;
void *xrdb_wav_data;

/* Program */

unsigned long
gettime()
{
    /* Time in milliseconds since epoch */
    static struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int
xdmv_sleep(unsigned long ms)
{
    static struct timespec rts;
    rts.tv_sec = 0;
    rts.tv_nsec = ms * 1000000;

    return nanosleep(&rts, 0);
}

int
xdmv_set_prop(Display *d, Window w, const char *prop, const char *atom)
{
    Atom xa = XInternAtom(d, prop, False);
    Atom xa_prop = XInternAtom(d, atom, False);
    XChangeProperty(d, w, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *) &xa_prop, 1);
}

void
xdmv_render_box(Display *d, int s, Window win, int x, int y, int w, int h)
{
    /* For now: a black box. */
    /* In the future: possibly fancy effects using shaders and pixmaps for
     * backgrounds */
    XFillRectangle(d, win, DefaultGC(d, s), x, y, w, h);
}

void
xdmv_render_test(Display *d, int s, Window w, Pixmap bg, unsigned int t)
{
        double coeff = (t % 4000) / 2000.0;

        XCopyArea(d, bg, w, DefaultGC(d, s), 0, 0, xdmv_width,
                                                   xdmv_height,
                                                   xdmv_offset_x,
                                                   xdmv_offset_y);
        XFlush(d);

        for (int x = 0; x < xdmv_box_count; x++) {
            double delta = (sin(M_PI * coeff + (double)x * M_PI * 2.0 / xdmv_box_count) + 1.0) / 2.0;
            xdmv_render_box(d, s, w, xdmv_width / xdmv_box_count * x + xdmv_offset_x,
                                     xdmv_offset_y,
                                     xdmv_width / xdmv_box_count - xdmv_margin,
                                     xdmv_height * delta);
        }
        XFlush(d);
}

int
xdmv_xorg(int argc, char **argv)
{
    Display *display;
    Window window;
    XEvent event;
    int s;

    display = XOpenDisplay(NULL);
    dieifnull(display, "Cannot open display");

    s = DefaultScreen(display);

    int flags = CWBorderPixel | CWColormap;
    XSetWindowAttributes attrs = { ParentRelative, 0L, 0, 0L, 0, 0, Always, 0L,
        0L, False, StructureNotifyMask | ExposureMask, 0L, True, 0, 0 };
    window = XDefaultRootWindow(display);
    /* select kind of events we are interested in */
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);
    /* testing pixmaps */
    Pixmap bg = XCreatePixmap(display, window, xdmv_width, xdmv_height, 24);
    XCopyArea(display, window, bg, DefaultGC(display, s), xdmv_offset_x,
                                                          xdmv_offset_y,
                                                          xdmv_width,
                                                          xdmv_height,
                                                          0, 0);

    for (;;) {
        /* XNextEvent(display, &event); */

        unsigned long start = gettime();

        xdmv_render_test(display, s, window, bg, start);

        /* if (event.type == KeyPress) */
        /*     break; */

        unsigned int next = xdmv_refresh_rate;
        long elapsed = gettime() - start;
        if (elapsed < next)
            xdmv_sleep(next - elapsed);
    }

cleanup:
    XCloseDisplay(display);

    return 0;
}

int
xdmv_loadwav(FILE *f, struct wav_header *h, void **wav_data)
{
    /* I don't care about endianness and do basic validation only */

    int n = fread(h, 1, sizeof *h, f);
    if (n < sizeof *h)
        return -1;

    if (strncmp(h->riff, "RIFF", 4)) return -1;
    if (strncmp(h->wave, "WAVE", 4)) return -1;
    if (strncmp(h->fmt, "fmt", 3))   return -1;
    /* skip checking data marker cus it varies or something */

    unsigned int sz = h->file_size - 8;
    *wav_data = xmalloc(sz);
    fread(*wav_data, 1, sz, f);

    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        eprintf("Usage: %s music_file\n", *argv);
        exit(1);
    }

    const char *fn = argv[1];
    FILE *f = fopen(fn, "r");
    dieifnull(f, "Could not open music file");
    int n = xdmv_loadwav(f, &xrdb_wav_header, &xrdb_wav_data);
    dieif(n < 0, "Could not load music file");
    fclose(f);

    return xdmv_xorg(argc, argv);
}

