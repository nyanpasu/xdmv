#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <assert.h>
#include <execinfo.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdbe.h>
#include <X11/extensions/Xrandr.h>

#include <fftw3.h>

#include <jack/jack.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

/* Config */
/* TODO replace these with functions that get these values dynamically/from
 * files */
#define xdmv_framerate 60
/* sample interval */
#define xdmv_sample_rate 2048
#define xdmv_height 100
#define xdmv_width 1080
#define xdmv_offset_top 20
#define xdmv_offset_bot (-1)
#define xdmv_padding_x 10
#define xdmv_box_size 7
#define xdmv_box_margin 1
#define xdmv_box_color 0x616568

#define xdmv_lowest_freq 20
#define xdmv_highest_freq 20000

#define xdmv_smooth_passes 2
/* must be odd number */
#define xdmv_smooth_points 3

/* filter settings */
#define xdmv_integral 0.7
#define xdmv_gravity 1.0
double xdmv_weight[64] = {2.4, 2.0, 1.8, 1, 0.8, 0.8, 1, 0.8, 0.8, 1, 1, 0.8, 1, 1,
    0.8, 0.6, 0.6, 0.7, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8,
    0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8,
    0.8, 0.7, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6,
    0.6, 0.6, 0.6, 0.5, 0.4};
/* spectrum margin smoothing settings */
const float marginDecay = 1.6; // I admittedly forget how this works but it probably shouldn't be changed from 1.6
// margin weighting follows a polynomial slope passing through (0, minMarginWeight) and (marginSize, 1)
const float headMargin = 4; // the size of the head margin dropoff zone
const float tailMargin = 2; // the size of the tail margin dropoff zone
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
} xdmv_wav_header;

struct wav_header_chunk {
    char name[4];
    uint32_t size;
};

struct sample {
    int16_t l;
    int16_t r;
} *xdmv_wav_audio;

struct {
    jack_client_t *client;
    jack_status_t status;
    jack_port_t *port_l;
    jack_port_t *port_r;
    unsigned int pos;

    float bufl[xdmv_sample_rate];
    float bufr[xdmv_sample_rate];
} xdmv_jack;

struct {
    pa_simple *s;
    pthread_t thread;
    unsigned int pos;
    int status;
    struct sample buf[xdmv_sample_rate + 256];

} xdmv_pulse;

typedef struct Spectrum {
    float f[400];
    int bars;

    /* filter state */
    int lcf[400], hcf[400];
    float peak[401];
    float fc[400], fre[400], weight[400], fmem[400], flast[400], fall[400],
          fpeak[400];

    double *in;
    fftw_complex *out;
    fftw_plan p;
} Spectrum;

struct xdmv {
    Display *display;
    Window window;
    XEvent event;
    int screen;

    struct output_list {
        XRROutputInfo *info;
        XRRCrtcInfo *crtc;

        Spectrum spectruml, spectrumr;

        struct output_list *next;
    } *output_list;
    XRRScreenResources *screenresources;

    Pixmap bg;
    XdbeBackBuffer backbuffer;
    XdbeSwapInfo swapinfo;

    int song_length;
    uint32_t sample_rate;
} xdmv;

enum {
    source_file_wav = 0,
    source_jack,
    source_pulse,
} xdmv_source;

/* Program */

unsigned long
gettime()
{
    /* Time in milliseconds since epoch */
    static struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int
xdmv_sleep(unsigned long ms)
{
    static struct timespec rts;
    rts.tv_sec = ms / 1000;
    rts.tv_nsec = ms % 1000 * 1000 * 1000;

    if (clock_nanosleep(CLOCK_MONOTONIC, 0, &rts, 0)) {
        perror("clock_nanosleep");
        die("I'm a terrible person -- clock");
    }

    return 0;
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
    /* For now: a filled box. */
    /* In the future: possibly fancy effects using shaders and pixmaps for
     * backgrounds */
    static GC gc;
    if (!gc) {
        gc = DefaultGC(d, s);
        XSetForeground(d, gc, xdmv_box_color);
    }

    XFillRectangle(d, win, gc, x, y, w, h);
}

void
filter_savitskysmooth(Spectrum *s)
{
    /* Savitsky-Golay smoothing algorithm */
    /* from vis.js */
    int bars = s->bars;
    static float newArr[500];
    float *lastArray = s->f;
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

    for (int i = 0; i < bars; i++)
        s->f[i] = newArr[i];
}

void
filter_marginsmooth(Spectrum *s)
{
    /* from vis.js */
    float *f = s->f; int bars = s->bars;
    for (int i = 0; i < bars; i++) {
        float value = f[i];
        if (i < headMargin) {
            value *= headMarginSlope * pow(i + 1, marginDecay) + minMarginWeight;
        } else if (bars - i <= tailMargin) {
            value *= tailMarginSlope * pow(bars - i, marginDecay) + minMarginWeight;
        }
        s->f[i] = value;
    }
}

void
filter_freqweight(Spectrum *s)
{
    /* from cava */
    float *f = s->f, *weight = s->weight; int bars = s->bars;
    for (int i = 0; i < bars; i++)
        f[i] *= weight[i];
}

void
filter_integral(Spectrum *s)
{
    /* from cava */
    float *f = s->f, *fmem = s->fmem; int bars = s->bars;

    for (int o = 0; o < bars; o++) {
        fmem[o] = fmem[o] * xdmv_integral + f[o];
        f[o] = fmem[o];
    }
}

void
filter_gravity(Spectrum *s)
{
    /* from cava */
    float *f = s->f, *fall = s->fall, *fpeak = s->fpeak, *flast = s->flast;
    int bars = s->bars;
    /* static float g = xdmv_gravity * xdmv_height / 270 * pow(60.0 / xdmv_framerate, 2.5); */
    static float g = xdmv_gravity * pow(120.0 / xdmv_framerate, 2.5);
    float temp;

    for (int o = 0; o < bars; o++) {
        temp = f[o];

        if (temp < flast[o]) {
            f[o] = fpeak[o] - (g * fall[o] * fall[o]);
            fall[o] += 4;
        } else {
            f[o] = temp;
            fpeak[o] = f[o];
            fall[o] = 0;
        }

        flast[o] = f[o];
        f[o] = max(0.0, f[o]);
    }
}

void
separate_freq_bands(Spectrum *s)
{
    /* from cava */

    fftw_complex *out = s->out;
    int *lcf = s->lcf, *hcf = s->hcf;
    int bars = s->bars;
    int o,i;
    float *f = s->f, *peak = s->peak;
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
}

void
xdmv_spectrum_calculate(Spectrum *s)
{
    /* from cava */

    int lowcf = xdmv_lowest_freq,
        highcf = xdmv_highest_freq,
        rate = xdmv.sample_rate / 2,
        M = xdmv_sample_rate - 2;

    int bars = s->bars;
    float *fc = s->fc, *fre = s->fre, *weight = s->weight;
    int *lcf = s->lcf, *hcf = s->hcf;

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
        /* weight[n] = pow(fc[n], 0.75) / xdmv_sample_rate / 2000 * xdmv_height; */
        weight[n] = (double)1 / xdmv_sample_rate * log10(fc[n]) * ((double)n / bars + 1) / 20 * xdmv_height;

        int offset = sizeof(xdmv_weight) / sizeof(*xdmv_weight) * n / bars;
        if (n != 0)
            printf("%d: %f -> %f (%d -> %d) [%fx]\n", n, fc[n - 1], fc[n], lcf[n - 1], hcf[n - 1], weight[n - 1]);

        /* weight[n] *= xdmv_weight[offset]; */
    }

}

void
xdmv_spectrum_create(Display *d, int s, Window w, Pixmap bg, unsigned int t,
        Spectrum *sp)
{
    separate_freq_bands(sp);
    filter_savitskysmooth(sp);
    /* filter_marginsmooth(sp); */
    filter_integral(sp);
    /* filter_gravity(sp); */
    filter_freqweight(sp);
}

float *
xdmv_render_spectrum_top(Display *d, int s, Window w, Pixmap bg,
        unsigned int t, Spectrum *sp, int offx, int offy, int width)
{
    xdmv_spectrum_create(d, s, w, bg, t, sp);

    /* Render */
    for (int i = 0; i < sp->bars; i++) {
        float boxh = sp->f[i] / 4;

        xdmv_render_box(d, s, w, offx + width / sp->bars * i + xdmv_padding_x,
                                 offy + xdmv_offset_top,
                                 xdmv_box_size,
                                 boxh);
    }
    XFlush(d);
}

float *
xdmv_render_spectrum_bot(Display *d, int s, Window w, Pixmap bg,
        unsigned int t, Spectrum *sp, int offx, int offy, int width, int height)
{
    xdmv_spectrum_create(d, s, w, bg, t, sp);

    /* Render */
    for (int i = 0; i < sp->bars; i++) {
        float boxh = sp->f[i] / 4;

        xdmv_render_box(d, s, w, offx + width / sp->bars * i + xdmv_padding_x,
                                 offy + height - boxh - xdmv_offset_bot,
                                 xdmv_box_size,
                                 boxh);
    }
    XFlush(d);
}

void
xdmv_render_spectrums(Display *d, int s, Window w, Pixmap bg, unsigned long t, struct output_list *ol)
{
    XRRCrtcInfo *crtc = ol->crtc;
    int width = crtc->width, height = crtc->height, offx = crtc->x, offy = crtc->y;
    size_t offset;

    Spectrum *sl = &ol->spectruml;
    Spectrum *sr = &ol->spectrumr;

    switch (xdmv_source) {
        case source_file_wav:
            offset = xdmv.sample_rate * t / 1000;
            for (size_t i = 0; i < xdmv_sample_rate; i++) {
                sl->in[i] = xdmv_wav_audio[offset + i].l;
                sr->in[i] = xdmv_wav_audio[offset + i].r;
            }
            break;
        case source_jack:
            offset = xdmv.sample_rate * t / 4000;
            for (size_t i = 0; i < xdmv_sample_rate; i++) {
                sl->in[i] = xdmv_jack.bufl[(offset + i) % xdmv_sample_rate] * 65536;
                sr->in[i] = xdmv_jack.bufr[(offset + i) % xdmv_sample_rate] * 65536;
            }
            break;
        case source_pulse:
            offset = xdmv.sample_rate * t / 1000;
            for (size_t i = 0; i < xdmv_sample_rate; i++) {
                sl->in[i] = xdmv_pulse.buf[(offset + i) % xdmv_sample_rate].l;
                sr->in[i] = xdmv_pulse.buf[(offset + i) % xdmv_sample_rate].r;
            }
            break;
        default:
            die("wtf?");
    }

    fftw_execute(sl->p);
    fftw_execute(sr->p);
    xdmv_render_spectrum_top(d, s, w, bg, t, sl, offx, offy, width);
    xdmv_render_spectrum_bot(d, s, w, bg, t, sr, offx, offy, width, height);
}

void
xdmv_xorg_cleanup(void)
{
    XdbeSwapBuffers(xdmv.display, &xdmv.swapinfo, 1);
    XFlush(xdmv.display);
    XCloseDisplay(xdmv.display);
}

void
xdmv_fftw_init(Spectrum *s)
{
    s->in = fftw_malloc(sizeof(*s->in) * xdmv_sample_rate);
    s->out = fftw_malloc(sizeof(*s->out) * xdmv_sample_rate);
    s->p = fftw_plan_dft_r2c_1d(xdmv_sample_rate, s->in, s->out, FFTW_MEASURE);
}

int
xdmv_xorg(int argc, char **argv)
{
    char *display_name = NULL;
    if (argc > 2)
        display_name = argv[2];
    xdmv.display = XOpenDisplay(display_name);
    dieifnull(xdmv.display, "Cannot open display");
    Display *display = xdmv.display;

    int major, minor;
    if (!XRRQueryExtension(display, &major, &minor)) {
        die("Could not load xrandr extension.");
    }

    xdmv.screenresources =
        XRRGetScreenResources(display, XDefaultRootWindow(display));
    XRRScreenResources *sr = xdmv.screenresources;
    struct output_list **ol = &xdmv.output_list;
    *ol = xmalloc(sizeof **ol);
    /* go through monitors */
    for (int i = 0; i < sr->noutput; i++) {
        XRROutputInfo *info;
        info = XRRGetOutputInfo(display, sr, sr->outputs[i]);
        if (info->connection == RR_Connected) {
            (*ol)->info = info;
            (*ol)->crtc = XRRGetCrtcInfo(display, sr, info->crtc);
            (*ol)->next = xmalloc(sizeof **ol);
            xdmv_fftw_init(&(*ol)->spectruml);
            xdmv_fftw_init(&(*ol)->spectrumr);
            XRRCrtcInfo *crtc = (*ol)->crtc;
            Spectrum *sl = &(*ol)->spectruml;
            Spectrum *sr = &(*ol)->spectrumr;
            int width = crtc->width, height = crtc->height, offx = crtc->x, offy = crtc->y;
            int bars = (width - xdmv_padding_x * 2) / (xdmv_box_size + xdmv_box_margin);
            sl->bars = sr->bars = bars;
            xdmv_spectrum_calculate(sl);
            xdmv_spectrum_calculate(sr);
            ol = &(*ol)->next;
        }
    }
    *ol = NULL;

    xdmv.screen = DefaultScreen(display);
    xdmv.window = XDefaultRootWindow(display);
    Window window = xdmv.window;
    int s = xdmv.screen;

    XMapWindow(display, window);

    /* set up double buffering */
    if (!XdbeQueryExtension(display, &major, &minor)) {
        die("Could not load double buffering extension.");
    }

    xdmv.backbuffer = XdbeAllocateBackBufferName(display, window, XdbeBackground);
    xdmv.swapinfo.swap_window = window;
    xdmv.swapinfo.swap_action = XdbeBackground;

    /* main render loop */
    unsigned long start = gettime(), loop_start = 0;
    unsigned long wav_end = xdmv.song_length * 1000 -
                        xdmv_sample_rate * 1000 / xdmv.sample_rate;
    for (;;) {
        loop_start = gettime();
        unsigned long cur = loop_start - start;
        if (xdmv.song_length > 0 && cur > wav_end)
            break;

        for (struct output_list *ol = xdmv.output_list; ol; ol = ol->next) {
            xdmv_render_spectrums(display, s, xdmv.backbuffer, xdmv.bg, cur, ol);
        }
        XdbeSwapBuffers(display, &xdmv.swapinfo, 1);

        unsigned int next = 1000 / xdmv_framerate;
        long elapsed = gettime() - loop_start;
        if (elapsed < next)
            xdmv_sleep(next - elapsed);
    }

    xdmv_xorg_cleanup();
    return 0;
}

int
xdmv_loadwav(FILE *f, struct wav_header *h, struct sample **samples)
{
    /* I don't care about endianness and do basic validation only */

    int n = fread(h, 1, sizeof *h, f);
    if (n < sizeof *h)
        return -1;

    if (strncmp(h->riff, "RIFF", 4)) return -1;
    if (strncmp(h->wave, "WAVE", 4)) return -1;
    if (strncmp(h->fmt, "fmt", 3))   return -1;

    /* support only these properties for now */
    if (h->bits_per_sample != 16)    return -1;
    if (h->channels != 2)            return -1;

    /* read until data chunk */
    struct wav_header_chunk hc;
    for (;;) {
        if ((n = fread(&hc, 1, sizeof hc, f)) < 0)
            die("Invalid wav file");
        if (!strncmp(hc.name, "data", 4))
            break;
        fseek(f, hc.size, SEEK_CUR);
    }

    unsigned int sz = hc.size;
    xdmv.song_length = sz * 8 / h->bits_per_sample / h->channels / h->sample_rate;
    xdmv.sample_rate = h->sample_rate;

    *samples = xmalloc(sz);
    if (fread(*samples, 1, sz, f) < sz)
        die("Could not read full file\n");

    return sz;
}

int
xdmv_jack_process(jack_nframes_t nframes, void *arg)
{
    float *bufl = jack_port_get_buffer(xdmv_jack.port_l, nframes);
    float *bufr = jack_port_get_buffer(xdmv_jack.port_r, nframes);

    unsigned int i = 0;
    unsigned int pos = xdmv_jack.pos;
    nframes /= 4;
    while(i++, pos++, nframes--) {
        xdmv_jack.bufl[pos % xdmv_sample_rate] = bufl[i];
        xdmv_jack.bufr[pos % xdmv_sample_rate] = bufr[i];
    }
    xdmv_jack.pos = pos;

    return 0;
}

int
xdmv_jack_sample_rate(jack_nframes_t nframes, void *arg)
{
    xdmv.sample_rate = nframes;
    return 0;
}

int
xdmv_jack_init()
{
    xdmv_jack.client = jack_client_open("xdmv", JackNoStartServer, &xdmv_jack.status);
    jack_client_t *client = xdmv_jack.client;

    if (!client)
        return -1;

    jack_set_process_callback(client, &xdmv_jack_process, 0);
    jack_set_sample_rate_callback(client, &xdmv_jack_sample_rate, 0);

    jack_port_t *l = jack_port_register(client, "xdmv_l",
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,
            xdmv_sample_rate);
    jack_port_t *r = jack_port_register(client, "xdmv_r",
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,
            xdmv_sample_rate);
    assert(l && r);
    xdmv_jack.port_l = l;
    xdmv_jack.port_r = r;

    xdmv.sample_rate = jack_get_sample_rate(client);

    jack_activate(client);

    return 0;
}

void *
xdmv_pulse_process(void *arg)
{
    xdmv_pulse.status = 0;
    pa_simple *s;
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16NE;
    ss.channels = 2;
    ss.rate = 44100;
    s = pa_simple_new(NULL,   // Use the default server.
            "xdmv",           // Our application's name.
            PA_STREAM_RECORD,
            NULL,             // Use the default device.
            "bars",           // Description of our stream.
            &ss,              // Our sample format.
            NULL,             // Use default channel map
            NULL,             // Use default buffering attributes.
            NULL              // Ignore error code.
            );
    if (!s) {
        xdmv_pulse.status = -1;
        return NULL;
    }

    xdmv_pulse.s = s;
    xdmv.sample_rate = ss.rate;

    /* bytes */
    const size_t chunk_size = xdmv_sample_rate / xdmv_framerate;
    xdmv_pulse.status = 1;
    for(;;) {
        size_t offset = xdmv_pulse.pos % xdmv_sample_rate;
        int16_t *b = (int16_t *)(xdmv_pulse.buf + offset);
        int errnum;
        int err = pa_simple_read(xdmv_pulse.s, b, chunk_size * 4, &errnum);
        if (err < 0) {
            eprintf("pa_simple_read: %d\n", errnum);
            die("");
        }
        xdmv_pulse.pos += chunk_size;
    }
    return NULL;
}

int
xdmv_pulse_init()
{
    int n = pthread_create(&xdmv_pulse.thread, NULL, &xdmv_pulse_process, NULL);
    if (n) {
        perror("pthread");
        die("Could not set up pulse input thread");
    }

    while (!xdmv_pulse.status)
        xdmv_sleep(100);

    if (xdmv_pulse.status == 1)
        return 0;

    return xdmv_pulse.status;
}

void
xdmv_load_sources(int argc, char **argv)
{
    if (argc >= 2) {
        const char *fn = argv[1];
        FILE *f = fopen(fn, "r");
        dieifnull(f, "Could not open music file");

        /* TODO support for more formats for testing */
        /* wav is easiest */
        int n = xdmv_loadwav(f, &xdmv_wav_header, &xdmv_wav_audio);
        dieif(n < 0, "Could not load music file");
        fclose(f);
        xdmv_source = source_file_wav;
    } else if (!xdmv_pulse_init()) {
        xdmv_source = source_pulse;
    } else if (!xdmv_jack_init()) {
        xdmv_source = source_jack;
    } else {
        die("Could not load any source");
    }
}

void
sig_handler(int n)
{
    xdmv_xorg_cleanup();
    signal(n, SIG_DFL);
    raise(n);
}

void
sig_segv()
{
    void *bt[20];
    size_t len = backtrace(bt, 20);

    backtrace_symbols_fd(bt, len, STDERR_FILENO);
    exit(1);
}

void
xdmv_signal_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sa.sa_handler = sig_segv;
    sigaction(SIGSEGV, &sa, 0);
}

int
main(int argc, char **argv)
{
    xdmv_signal_init();
    xdmv_load_sources(argc,argv);
    return xdmv_xorg(argc, argv);
}

