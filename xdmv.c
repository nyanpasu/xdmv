#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>

#define eprintf(...) fprintf(stderr, __VA_ARGS__);

#define dieifnull(p, reason) _dieifnull((p), (reason), __LINE__);
void
_dieifnull(void *p, const char *reason, int line)
{
    if (!p) {
        eprintf("%d: %s\n", line, reason);
        exit(1);
    }
}

/* Time in milliseconds since epoch */
unsigned long
gettime()
{
    static struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int
xdmv_set_prop(void)
{
    Atom xa = XInternAtom(display, "_NET_WM_STATE", False);
    Atom xa_prop = XInternAtom(display, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(display, window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *) &xa_prop, 1);
}


int
main(void)
{
    Display *display;
    Window window;
    XEvent event;
    char *msg = "Hello, World!";
    int s;

    /* open connection with the server */
    display = XOpenDisplay(NULL);
    dieifnull(display, "Cannot open display");

    s = DefaultScreen(display);

    int flags = CWBorderPixel | CWColormap | CWOverrideRedirect;
    XSetWindowAttributes attrs = { ParentRelative, 0L, 0, 0L, 0, 0, Always, 0L,
        0L, False, StructureNotifyMask | ExposureMask, 0L, True, 0, 0 };
    window = XCreateWindow(display, RootWindow(display, 0), 10, 10, 800, 200,
            0, CopyFromParent, InputOutput, CopyFromParent, flags, &attrs);

    Atom xa = XInternAtom(display, "_NET_WM_STATE", False);
    Atom xa_prop = XInternAtom(display, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(display, window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *) &xa_prop, 1);

    /* select kind of events we are interested in */
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    /* testing pixmaps */
    Pixmap p = XCreatePixmap(display, window, 800, 200, 24);
    /* xcb_copy_area(c, wall, w, gc, 0, 0, 1080, 0, 1920, 1080); */
    XCopyArea(display, window, p, DefaultGC(display, s), 0, 0, 800, 200, 0, 0);

    for (;;) {
        /* XNextEvent(display, &event); */

        unsigned long start = gettime();
        double coeff = (start % 4000) / 2000.0;
        double delta = sin(M_PI * coeff) * 350;

        /* xcb_poly_fill_rectangle(c, w, white, 1, &rectbg); */
        XCopyArea(display, p, window, DefaultGC(display, s), 0, 0, 800, 200, 0, 0);
        XFlush(display);
        XFillRectangle(display, window, DefaultGC(display, s), 400 + delta, 20, 10, 10);

        XFlush(display);

        if (event.type == KeyPress)
            break;

        long elapsed = gettime() - start;
        if (elapsed < 1000/60) {
            long remaining = 1000/60 - elapsed;
            struct timespec rts = {
                .tv_sec = 0,
                .tv_nsec = remaining * 1000000
            };
            nanosleep(&rts, 0);
        }
    }

cleanup:
    XCloseDisplay(display);

    return 0;
}
