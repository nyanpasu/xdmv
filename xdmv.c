#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xdbe.h>

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

unsigned long
gettime()
{
    static struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
    window = XCreateWindow(display, RootWindow(display, 0), 10, 10, 200, 200,
            0, CopyFromParent, InputOutput, CopyFromParent, flags, &attrs);

    /* testing pixmaps */
    Pixmap p = XCreatePixmap(display, window, 200, 200, 24);
    /* xcb_copy_area(c, wall, w, gc, 0, 0, 1080, 0, 1920, 1080); */
    XCopyArea(display, window, p, DefaultGC(display, s), 0, 0, 200, 200, 0, 0);

    /* testing xdbe */
    int count = 1;
    XdbeScreenVisualInfo *info = XdbeGetVisualInfo(&display, &window, &count);
    XdbeBackBuffer bbuf = XdbeAllocateBackBufferName(&display, &window, XdbeUndefined);

    /* select kind of events we are interested in */
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    for (;;) {
        /* XNextEvent(display, &event); */

        unsigned long start = gettime();
        double coeff = (start % 4000) / 2000.0;
        double delta = sin(M_PI * coeff) * 20;
        /* xcb_poly_fill_rectangle(c, w, white, 1, &rectbg); */
        XCopyArea(display, p, bbuf, DefaultGC(display, s), 0, 0, 200, 200, 0, 0);
        XFillRectangle(display, bbuf, DefaultGC(display, s), 30 + delta, 20, 10, 10);
        XdbeSwapBuffers(&display, info, 1);

        /* if (event.type == Expose) { */
            /* XFillRectangle(display, window, DefaultGC(display, s), 20, 20, 10, 10); */
            /* XDrawString(display, window, DefaultGC(display, s), 50, 50, msg, strlen(msg)); */
        /* } */
        /* if (event.type == KeyPress) */
        /*     break; */

    }

cleanup:
    XCloseDisplay(display);

    return 0;
}
