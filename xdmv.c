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

/* State */


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

#define die(reason) _die((reason), __LINE__);
void
_die(const char *reason, int line)
{
    eprintf("%d: %s\n", line, reason);
    exit(1);
}

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
xdmv_render_test(Display *d, int s, Window w, Pixmap bg, unsigned int t)
{
        double coeff = (t % 4000) / 2000.0;
        double delta = sin(M_PI * coeff) * 350;

        XCopyArea(d, bg, w, DefaultGC(d, s), 0, 0, 800, 200, 0, 0);
        XFlush(d);
        XFillRectangle(d, w, DefaultGC(d, s), 400 + delta, 20, 10, 10);
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
    Pixmap bg = XCreatePixmap(display, window, 800, 200, 24);
    XCopyArea(display, window, bg, DefaultGC(display, s), 0, 0, 800, 200, 0, 0);

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
main(int argc, char **argv)
{
    if (argc < 2) {
        eprintf("Usage: %s music_file\n", *argv);
        exit(1);
    }

    const char *fn = argv[1];
    FILE *f = fopen(fn, "r");
    dieifnull(f, "Could not open music file.");

    return xdmv_xorg(argc, argv);
}
