#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <math.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdbe.h>

#include <fftw3.h>

/* Config */
/* TODO replace these with functions that get these values dynamically/from
 * files */
#define xdmv_refresh_rate 1000/60
#define xdmv_sample_rate 2048
#define xdmv_height 100
#define xdmv_width 1080
#define xdmv_margin 2
#define xdmv_offset_y 20
#define xdmv_offset_x 0
#define xdmv_box_count 80

#define xdmv_lowest_freq 50
#define xdmv_highest_freq 18000

#define xdmv_smooth_passes 4
/* must be odd number */
#define xdmv_smooth_points 3

#define xdmv_monstercat 1.5
#define xdmv_integral 0.6
/* spectrum margin smoothing settings */
const float spectrumHeight = xdmv_box_count / 4.5;
const float marginDecay = 1.6; // I admittedly forget how this works but it probably shouldn't be changed from 1.6
// margin weighting follows a polynomial slope passing through (0, minMarginWeight) and (marginSize, 1)
const float headMargin = 7; // the size of the head margin dropoff zone
const float tailMargin = 0; // the size of the tail margin dropoff zone
const float minMarginWeight = 0.6; // the minimum weight applied to bars in the dropoff zone
#define headMarginSlope ((1 - minMarginWeight) / pow(headMargin, marginDecay))
#define tailMarginSlope ((1 - minMarginWeight) / pow(tailMargin, marginDecay))

/* Utils */
#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

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
};

struct wav_header_chunk {
    char name[4];
    uint32_t size;
};

struct wav_sample {
    int16_t l;
    int16_t r;
};

struct wav_header xdmv_wav_header;
void *xdmv_wav_data;
struct wav_sample *xdmv_wav_audio;

int lcf[200], hcf[200];
float fc[200], fre[200], weight[200];
float fmem[200], flast[200];

double fweight[64] = {1.0, 1.0, 1, 1, 0.8, 0.8, 1, 0.8, 0.8, 1, 1, 0.8,
                      1, 1, 0.8, 0.6, 0.6, 0.7, 0.8, 0.8, 0.8, 0.8, 0.8,
                      0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8,
                      0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8,
                      0.7, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6,
                      0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6};
struct xdmv {
    Display *display;
    Window window;
    XEvent event;
    int screen;

    Pixmap bg;
    XdbeBackBuffer backbuffer;
    XdbeSwapInfo swapinfo;
} xdmv;

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
    for (int x = 0; x < xdmv_box_count; x++) {
        double delta = (sin(M_PI * coeff + (double)x * M_PI * 2.0 / xdmv_box_count) + 1.0) / 2.0;
        xdmv_render_box(d, s, w, xdmv_width / xdmv_box_count * x + xdmv_offset_x,
                                 xdmv_offset_y,
                                 xdmv_width / xdmv_box_count - xdmv_margin,
                                 xdmv_height * delta);
    }
    XFlush(d);
}

float *
filter_savitskysmooth(float *f, int bars)
{
    /* Savitsky-Golay smoothing algorithm */
    static float newArr[200];
    static float *lastArray;
    lastArray = f;
    for (int pass = 0; pass < xdmv_smooth_passes; pass++) {
        // our window is centered so this is both nL and nR
        double sidePoints = floor(xdmv_smooth_points / 2);
        double cn = 1 / (2 * sidePoints + 1);
        for (int i = 0; i < sidePoints; i++) {
            newArr[i] = lastArray[i];
            newArr[bars - i - 1] = lastArray[bars - i - 1];
        }
        for (int i = sidePoints; i < bars - sidePoints; i++) {
            int sum = 0;
            for (int n = -sidePoints; n <= sidePoints; n++) {
                sum += cn * lastArray[i + n] + n;
            }
            newArr[i] = sum;
        }
        lastArray = newArr;
    }
    return newArr;
}

float *
filter_marginsmooth(float *f, int bars)
{
    static float values[200];
    for (int i = 0; i < bars; i++) {
        float value = f[i];
        if (i < headMargin) {
            value *= headMarginSlope * pow(i + 1, marginDecay) + minMarginWeight;
        } else if (bars - i <= tailMargin) {
            value *= tailMarginSlope * pow(bars - i, marginDecay) + minMarginWeight;
        }
        values[i] = value;
    }
    return values;
}

float *
filter_freqweight(float *f, int bars)
{
    for (int i = 0; i < bars; i++)
        f[i] *= weight[i];
    return f;
}

float *
filter_monstercat(float *f, int bars)
{
    int y, z, de;
    for (z = 0; z < bars; z++) {
        if (f[z] < 0.125)f[z] = 0.125;
        for (y = z - 1; y >= 0; y--) {
            de = z - y;
            f[y] = max(f[z] / pow(xdmv_monstercat, de), f[y]);
        }
        for (y = z + 1; y < bars; y++) {
            de = y - z;
            f[y] = max(f[z] / pow(xdmv_monstercat, de), f[y]);
        }
    }
    return f;
}

float *
filter_integral(float *f, int bars)
{
    for (int o = 0; o < bars; o++) {
        fmem[o] = fmem[o] * xdmv_integral + f[o];
        f[o] = fmem[o];
    }

    return f;
}

float *
separate_freq_bands(fftw_complex *out, int bars,
                    int *lcf, int *hcf)
{
    /* Original source: cava */
    int o,i;
    float peak[201];
    static float f[200];
    float temp;


    // process: separate frequency bands
    for (o = 0; o < bars; o++) {
        peak[o] = 0;

        /* get peaks */
        for (i = lcf[o]; i <= hcf[o]; i++) {
            peak[o] += pow(pow(out[i][0], 2) + pow(out[i][1], 2), 0.5);
        }

        /* getting average */
        peak[o] /= hcf[o] - lcf[o] + 1;
        peak[o] = pow(peak[o], 0.7);
        f[o] = peak[o];
    }

    return f;
}

void
xdmv_render_spectrum(Display *d, int s, Window w, Pixmap bg, unsigned int t)
{
    size_t offset = xdmv_wav_header.sample_rate * t / 1000;
    float *f;
    static double *in;
    static fftw_complex *out;
    static fftw_plan p;
    if (!in && !out) {
        in = fftw_malloc(sizeof(*in) * xdmv_sample_rate);
        out = fftw_malloc(sizeof(*out) * xdmv_sample_rate);
        p = fftw_plan_dft_r2c_1d(xdmv_sample_rate, in, out, FFTW_MEASURE);
    }

    for (int i = 0; i < xdmv_sample_rate; i++)
        in[i] = xdmv_wav_audio[offset + i].l;

    fftw_execute(p);
    f = separate_freq_bands(out, xdmv_box_count, lcf, hcf);
    f = filter_freqweight(f, xdmv_box_count);
    f = filter_savitskysmooth(f, xdmv_box_count);
    f = filter_marginsmooth(f, xdmv_box_count);
    f = filter_monstercat(f, xdmv_box_count);
    f = filter_integral(f, xdmv_box_count);

    for (int o = 0; o < xdmv_box_count; o++)
        f[o] = f[o] == 0.0 ? 1.0 : f[o];

    /* Render */
    for (int i = 0; i < xdmv_box_count; i++) {
        float height = f[i];

        xdmv_render_box(d, s, w, xdmv_width / xdmv_box_count * i + xdmv_offset_x,
                                 xdmv_offset_y,
                                 xdmv_width / xdmv_box_count - xdmv_margin,
                                 height );
    }
    XFlush(d);
}

int
xdmv_xorg(int argc, char **argv)
{
    xdmv.display = XOpenDisplay(NULL);
    dieifnull(xdmv.display, "Cannot open display");
    Display *display = xdmv.display;
    xdmv.screen = DefaultScreen(display);
    xdmv.window = XDefaultRootWindow(display);
    Window window = xdmv.window;
    int s = xdmv.screen;

    /* select kind of events we are interested in */
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);
    /* testing pixmaps */
    xdmv.bg = XCreatePixmap(display, window, xdmv_width, xdmv_height, 24);
    XCopyArea(display, window, xdmv.bg, DefaultGC(display, s), xdmv_offset_x,
                                                          xdmv_offset_y,
                                                          xdmv_width,
                                                          xdmv_height,
                                                          0, 0);

    /* Set up double buffering */
    int major, minor;
    if (!XdbeQueryExtension(display, &major, &minor)) {
        die("Could not load double buffering extension.");
    }

    xdmv.backbuffer = XdbeAllocateBackBufferName(display, window, XdbeBackground);
    xdmv.swapinfo.swap_window = window;
    xdmv.swapinfo.swap_action = XdbeBackground;

    unsigned long start = gettime(), loop_start = 0;
    for (;;) {
        /* XNextEvent(display, &event); */

        loop_start = gettime();
        /* xdmv_render_test(display, s, xdmv.backbuffer, xdmv.bg, loop_start - start); */
        xdmv_render_spectrum(display, s, xdmv.backbuffer, xdmv.bg, loop_start - start);
        XdbeSwapBuffers(display, &xdmv.swapinfo, 1);

        /* if (event.type == KeyPress) */
        /*     break; */

        unsigned int next = xdmv_refresh_rate;
        long elapsed = gettime() - loop_start;
        if (elapsed < next)
            xdmv_sleep(next - elapsed);
    }

    return 0;
}

void
xdmv_xorg_cleanup(void)
{
    XdbeSwapBuffers(xdmv.display, &xdmv.swapinfo, 1);
    XFlush(xdmv.display);
    XCloseDisplay(xdmv.display);
}

int
xdmv_loadwav(FILE *f, struct wav_header *h, void **wav_data,
             struct wav_sample **wav_audio)
{
    /* I don't care about endianness and do basic validation only */

    int n = fread(h, 1, sizeof *h, f);
    if (n < sizeof *h)
        return -1;

    if (strncmp(h->riff, "RIFF", 4)) return -1;
    if (strncmp(h->wave, "WAVE", 4)) return -1;
    if (strncmp(h->fmt, "fmt", 3))   return -1;
    /* skip checking data marker cus it varies or something */

    /* support only 16 bits for testing */
    if (h->bits_per_sample != 16)    return -1;

    /* Read until data chunk */
    struct wav_header_chunk hc;
    for (;;) {
        if (fread(&hc, 1, sizeof hc, f) < 0)
            die("Invalide wav file");
        if (!strncmp(hc.name, "data", 4))
            break;
        fseek(f, hc.size, SEEK_CUR);
    }

    unsigned int sz = hc.size;

    *wav_data = xmalloc(sz);
    *wav_audio = *wav_data;
    return fread(*wav_data, 1, sz, f);
}

void
xdmv_init()
{
    /* Source: cava */

    int bars = xdmv_box_count,
        lowcf = xdmv_lowest_freq,
        highcf = xdmv_highest_freq,
        rate = xdmv_wav_header.sample_rate,
        M = xdmv_sample_rate;

    double freqconst = log10((float)lowcf / (float)highcf)
                       / ((float)1 / ((float)bars + (float)1) - 1);

    for (int n = 0; n < bars + 1; n++) {
        fc[n] = highcf * pow(10, freqconst * (-1)
                + ((((float)n + 1) / ((float)bars + 1)) * freqconst));
        fre[n] = fc[n] / (rate / 2);
        lcf[n] = fre[n] * (M / 4);
        if (n != 0) {
            hcf[n - 1] = lcf[n] - 1;
            if (lcf[n] <= lcf[n - 1])
                lcf[n] = lcf[n - 1] + 1;
            hcf[n - 1] = lcf[n] - 1;
        }
    }

    for (int n = 0; n < bars; n++) {
        int offset = sizeof(fweight) / sizeof(*fweight) * n / bars;
        weight[n] = pow(fc[n], 0.85) * ((double)xdmv_height / xdmv_sample_rate / 4000) * fweight[offset];
    }
}

void
sig_handler(int sig_no)
{
    xdmv_xorg_cleanup();
    signal(sig_no, SIG_DFL);
    raise(sig_no);
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        eprintf("Usage: %s music_file\n", *argv);
        exit(1);
    }

    signal(SIGINT, &sig_handler);
    signal(SIGTERM, &sig_handler);

    const char *fn = argv[1];
    FILE *f = fopen(fn, "r");
    dieifnull(f, "Could not open music file");
    int n = xdmv_loadwav(f, &xdmv_wav_header, &xdmv_wav_data, &xdmv_wav_audio);
    dieif(n < 0, "Could not load music file");
    fclose(f);

    xdmv_init();

    return xdmv_xorg(argc, argv);
}

