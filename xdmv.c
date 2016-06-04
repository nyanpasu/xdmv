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

#include <fftw3.h>

/* Config */
#define xdmv_refresh_rate 1000/60
#define xdmv_sample_rate 2048
/* TODO Replace these with functions that get these values dynamically/from
 * configs */
#define xdmv_height 100
#define xdmv_width 1080
#define xdmv_margin 2
#define xdmv_offset_y 20
#define xdmv_offset_x 0
#define xdmv_box_count 40

#define xdmv_lowest_freq 50
#define xdmv_highest_freq 10000

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

/* int f[200]; */


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
xdmv_render_clear(Display *d, int s, Window w, Pixmap bg)
{
    XCopyArea(d, bg, w, DefaultGC(d, s), 0, 0, xdmv_width,
                                               xdmv_height,
                                               xdmv_offset_x,
                                               xdmv_offset_y);
    XFlush(d);
}


void
xdmv_render_test(Display *d, int s, Window w, Pixmap bg, unsigned int t)
{
    xdmv_render_clear(d, s, w, bg);

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
separate_freq_bands(fftw_complex out[][2], int bars,
                    int *lcf, int *hcf, float *k)
{
    /* Original source: cava */
    int o,i;
    float peak[201];
    static float f[200];
    float y[xdmv_sample_rate / 2 + 1];
    float temp;


    // process: separate frequency bands
    for (o = 0; o < bars; o++) {
        peak[o] = 0;

        // process: get peaks
        for (i = lcf[o]; i <= hcf[o]; i++) {
            //getting r of complex
            y[i] =  pow(pow(*out[i][0], 2) + pow(*out[i][1], 2), 0.5);
            //adding upp band
            peak[o] += y[i];
        }

        //getting average
        peak[o] = peak[o] / (hcf[o]-lcf[o]+1);

        //multiplying with k and adjusting to sens settings
        /* temp = peak[o] / k[o] * sens; */
        /* if (temp <= 200) */
        /*     temp = 0; */
        temp = peak[o] / fc[o];

        f[o] = temp;
    }

    return f;
}

void
xdmv_render_spectrum(Display *d, int s, Window w, Pixmap bg, unsigned int t)
{
    static float flast[200];
    uint32_t offset = xdmv_wav_header.sample_rate * t / 1000;

    static double *in;
    static fftw_complex *out;
    static fftw_plan p;
    if (!in && !out) {
        in = fftw_malloc(sizeof(*in) * xdmv_sample_rate);
        out = fftw_malloc(sizeof(*out) * xdmv_sample_rate);
        p = fftw_plan_dft_r2c_1d(xdmv_sample_rate, in, out, FFTW_MEASURE);
    }

    for (uint32_t i = 0; i < xdmv_sample_rate; i++)
        in[i] = xdmv_wav_audio[offset + i].l;

    fftw_execute(p);
    float *f = separate_freq_bands(out, xdmv_box_count, lcf, hcf, weight);

    // zero values causes divided by zero segfault
    for (int o = 0; o < xdmv_box_count; o++)
        f[o] = f[o] == 0.0 ? 1.0 : f[o];

    /* Render */
    xdmv_render_clear(d, s, w, bg);
    XFlush(d);

    for (int i = 0; i < xdmv_box_count; i++) {
        float height = abs(f[i] - flast[i]) / 100.0;

        xdmv_render_box(d, s, w, xdmv_width / xdmv_box_count * i + xdmv_offset_x,
                                 xdmv_offset_y,
                                 xdmv_width / xdmv_box_count - xdmv_margin,
                                 xdmv_height * (height > 1.0 ? 1.0 : height) );
        flast[i] = f[i];
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


    unsigned long start = gettime(), loop_start = 0;
    for (;;) {
        /* XNextEvent(display, &event); */

        loop_start = gettime();
        /* xdmv_render_test(display, s, window, bg, loop_start - start); */
        xdmv_render_spectrum(display, s, window, bg, loop_start - start);

        /* if (event.type == KeyPress) */
        /*     break; */

        unsigned int next = xdmv_refresh_rate;
        long elapsed = gettime() - loop_start;
        if (elapsed < next)
            xdmv_sleep(next - elapsed);
    }

cleanup:
    XCloseDisplay(display);

    return 0;
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
xdmv_generate_cutoff()
{
    /* Source: cava */

    /* config */
    int bars = xdmv_box_count,
        lowcf = xdmv_lowest_freq,
        highcf = xdmv_highest_freq,
        rate = 48000,
        M = xdmv_sample_rate;

    double freqconst = log10((float)lowcf / (float)highcf)
                       / ((float)1 / ((float)bars + (float)1) - 1);

    for (int n = 0; n < bars + 1; n++) {
        fc[n] = highcf * pow(10, freqconst * (-1) + ((((float)n + 1)
                                              / ((float)bars + 1)) * freqconst));
        fre[n] = fc[n] / (rate / 2);
        lcf[n] = fre[n] * (M /4);
        if (n != 0) {
            hcf[n - 1] = lcf[n] - 1;
            if (lcf[n] <= lcf[n - 1])
                lcf[n] = lcf[n - 1] + 1;
            hcf[n - 1] = lcf[n] - 1;
        }

        if (n != 0) {
            printf("%d: %f -> %f (%d -> %d) \n", n, fc[n - 1], fc[n], lcf[n - 1], hcf[n - 1]);
        }
    }

    for (int n = 0; n < bars; n++)
        weight[n] = pow(fc[n], 0.85) * (float)xdmv_height / xdmv_sample_rate * 4000;
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
    int n = xdmv_loadwav(f, &xdmv_wav_header, &xdmv_wav_data, &xdmv_wav_audio);
    dieif(n < 0, "Could not load music file");
    fclose(f);

    xdmv_generate_cutoff();

    return xdmv_xorg(argc, argv);
}

