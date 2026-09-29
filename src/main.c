/*
 * main.c — the host program: SDL window, audio, input and the CLI.
 */
#include "gb.h"      /* the machine and its host-facing API */
#include "gbdefs.h"   /* SCREEN_W/H, SAMPLE_RATE, APU_BUF_FRAMES, gb_log_set_sink */
#include "joypad.h"   /* JOY_* button indices used to build the input bitmasks */
#include "apu.h"      /* apu_set_sample_rate(): the host's real device rate */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GBEMU_HAVE_SDL
#include <SDL3/SDL.h>
#endif

/* --- Tunables ----------------------------------------------------------- */

#define PRESS_MAX 64               /* --press events we are willing to hold */
#define PRESS_HOLD_FRAMES 8u        /* --press holds a button this many frames */
#define HEADLESS_DEFAULT_FRAMES 3600u /* headless cap when --frames is absent */

/* FNV-1a: a tiny, dependency-free frame checksum. CI compares these hashes
 * against golden values, so the exact constants matter. */
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME       16777619u

/* --- Command-line state -------------------------------------------------- */

/* One --press event: press `button` starting at frame `frame`. */
typedef struct {
    uint64_t frame;
    int      button;   /* a JOY_* index */
} PressEvent;

typedef struct {
    const char *rom;          /* positional <rom.gb>, required */
    const char *boot_rom;     /* --boot-rom PATH */
    const char *serial_log;   /* --serial-log PATH */
    const char *dump_frame;   /* --dump-frame PATH */
    const char *state_out;    /* --state-out PATH */
    const char *state_in;     /* --state-in PATH */
    const char *audio_device; /* --audio-device NAME; NULL means the default */
    const char *audio_dump;   /* --audio-dump PATH; raw S16 stereo, for analysis */

    long frames;              /* --frames N; -1 means "until quit" */
    int  scale;               /* --scale N; window pixels per Game Boy pixel */
    unsigned mute;            /* --mute-ch N; bit N-1 silences channel N */

    bool cgb;                 /* --cgb */
    bool headless;            /* --headless */
    bool frame_hash;          /* --frame-hash */
    bool audio_stats;         /* --audio-stats */
    bool list_audio_devices;  /* --list-audio-devices */
    bool trace;               /* --trace */
    bool debug;               /* --debug */
    bool help;                /* --help */

    PressEvent press[PRESS_MAX];
    size_t     press_count;

    uint16_t breaks[DEBUG_BP_MAX];
    size_t   break_count;

    uint16_t watches[DEBUG_WP_MAX];
    size_t   watch_count;
} Options;

/* --- Audio statistics (--audio-stats) ------------------------------------ */

typedef struct {
    uint64_t frames;    /* stereo frames drained */
    uint64_t samples;   /* individual int16 samples seen */
    uint64_t stalls;    /* times a stalled device forced a queue drop */
    int      peak;      /* largest |sample| */
    double   sum_sq;    /* sum of squares, for RMS */
} AudioStats;

/* --- Small helpers ------------------------------------------------------- */

/* Write a 16-bit little-endian value, which is what the BMP format demands. */
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* Write a 32-bit little-endian value. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ASCII case-insensitive compare, so --press accepts "a" and "A" alike.
 * strcasecmp() is POSIX, not C11, hence our own. */
static bool ascii_ieq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return *a == *b;
}

/* Button names in press-index order, so BUTTON_NAMES[i] names index i.
 * Indices 0-3 are the d-pad and 4-7 the buttons, matching the bit each
 * one occupies inside its group in joypad.h. */
#define BUTTON_COUNT 8
static const char *const BUTTON_NAMES[BUTTON_COUNT] = {
    "RIGHT", "LEFT", "UP", "DOWN", "A", "B", "SELECT", "START"
};

/* Map a --press button name to its press index, or -1 if unknown. */
static int button_index(const char *name)
{
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        if (ascii_ieq(name, BUTTON_NAMES[i])) return i;
    }
    return -1;
}

/* Parse a non-negative decimal integer; reject empty input and trailing junk. */
static bool parse_u64(const char *s, uint64_t *out)
{
    if (s == NULL || *s == '\0') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

/* Parse a 16-bit hexadecimal address (no 0x prefix needed). */
static bool parse_addr(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    /* Anything past FFFF is not an address the SM83 can reach. */
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFu) return false;
    *out = (uint16_t)v;
    return true;
}

/* Parse "FRAME:BTN" for --press. */
static bool parse_press(const char *s, PressEvent *out)
{
    const char *colon = strchr(s, ':');
    if (colon == NULL) return false;

    /* Split "123:A" into a decimal frame number and a button name. */
    char frame_text[32];
    size_t n = (size_t)(colon - s);
    if (n == 0 || n >= sizeof frame_text) return false;
    memcpy(frame_text, s, n);
    frame_text[n] = '\0';

    uint64_t frame;
    if (!parse_u64(frame_text, &frame)) return false;

    int button = button_index(colon + 1);
    if (button < 0) return false;

    out->frame = frame;
    out->button = button;
    return true;
}

/* Return the value of a "--opt value" or "--opt=value" pair, advancing *i when
 * the value is the next argv entry. Reports and returns NULL if it is missing. */
static const char *need_value(int argc, char **argv, int *i,
                             const char *name, const char *inline_value)
{
    if (inline_value != NULL) return inline_value;       /* --opt=value */
    if (*i + 1 < argc) return argv[++*i];              /* --opt value */
    fprintf(stderr, "gbemu: option %s needs a value\n", name);
    return NULL;
}

/* --- Usage -------------------------------------------------------------- */

static void print_usage(FILE *out)
{
    fputs(
        "gbemu [options] <rom.gb>\n"
        "  --cgb                 CGB hardware model\n"
        "  --boot-rom PATH        run a boot ROM before the cartridge\n"
        "  --headless             no window and no audio; print serial output and exit\n"
        "  --frames N             stop after N frames (default: run until quit)\n"
        "  --scale N              window scale, default 4\n"
        "  --serial-log PATH      append captured serial output to PATH\n"
        "  --dump-frame PATH      write the final framebuffer to PATH as a BMP\n"
        "  --frame-hash           print the FNV-1a hash of the final framebuffer\n"
        "  --audio-stats          print frames drained, peak sample and RMS\n"
        "  --audio-device NAME   play through the playback device whose name contains NAME\n"
        "                         (the system default is often a monitor with no speakers)\n"
        "  --list-audio-devices  list the playback devices and exit\n"
        "  --audio-dump PATH     write the mix to PATH as raw S16 stereo, for analysis\n"
        "  --mute-ch N           silence channel N (1-4) in the mix, for diagnosis only\n"
        "  --state-out PATH       save a state after the run\n"
        "  --state-in PATH        load a state before the run\n"
        "  --press FRAME:BTN     press BTN at frame FRAME (repeatable; BTN is A B SELECT START UP DOWN LEFT RIGHT), held for 8 frames\n"
        "  --trace                enable instruction tracing\n"
        "  --break ADDR           execution breakpoint (hex, repeatable)\n"
        "  --watch ADDR           read/write watchpoint (hex, repeatable)\n"
        "  --debug                 enter the interactive debugger REPL before running\n",
        out);
}

/* --- Argument parsing --------------------------------------------------- */

static bool parse_args(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->frames = -1;   /* no --frames means "until quit" */
    o->scale = 4;     /* the documented default window scale */

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        /* A bare word (no leading '-') is the ROM path. */
        if (arg[0] != '-') {
            if (o->rom != NULL) {
                fprintf(stderr, "gbemu: more than one ROM given\n");
                return false;
            }
            o->rom = arg;
            continue;
        }

        /* Split "--name=value" into the name and the inline value, if any. */
        const char *inline_value = NULL;
        char name[64];
        const char *eq = strchr(arg, '=');
        size_t n = eq != NULL ? (size_t)(eq - arg) : strlen(arg);
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, arg, n);
        name[n] = '\0';
        if (eq != NULL) inline_value = eq + 1;

        if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
            o->help = true;
        } else if (strcmp(name, "--cgb") == 0) {
            o->cgb = true;
        } else if (strcmp(name, "--headless") == 0) {
            o->headless = true;
        } else if (strcmp(name, "--frame-hash") == 0) {
            o->frame_hash = true;
        } else if (strcmp(name, "--audio-stats") == 0) {
            o->audio_stats = true;
        } else if (strcmp(name, "--list-audio-devices") == 0) {
            o->list_audio_devices = true;
        } else if (strcmp(name, "--audio-device") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->audio_device = v;
        } else if (strcmp(name, "--audio-dump") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->audio_dump = v;
        } else if (strcmp(name, "--mute-ch") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            uint64_t ch;
            if (v == NULL || !parse_u64(v, &ch) || ch < 1u || ch > 4u) {
                fprintf(stderr, "gbemu: --mute-ch needs a channel number from 1 to 4\n");
                return false;
            }
            o->mute |= 1u << (unsigned)(ch - 1u);
        } else if (strcmp(name, "--trace") == 0) {
            o->trace = true;
        } else if (strcmp(name, "--debug") == 0) {
            o->debug = true;
        } else if (strcmp(name, "--boot-rom") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->boot_rom = v;
        } else if (strcmp(name, "--serial-log") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->serial_log = v;
        } else if (strcmp(name, "--dump-frame") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->dump_frame = v;
        } else if (strcmp(name, "--state-out") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->state_out = v;
        } else if (strcmp(name, "--state-in") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            o->state_in = v;
        } else if (strcmp(name, "--frames") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            uint64_t frames;
            if (v == NULL || !parse_u64(v, &frames)) {
                fprintf(stderr, "gbemu: --frames needs a non-negative count\n");
                return false;
            }
            /* Reject anything that would not fit back into the long we store. */
            if (frames > (uint64_t)0x7FFFFFFF) {
                fprintf(stderr, "gbemu: --frames is too large\n");
                return false;
            }
            o->frames = (long)frames;
        } else if (strcmp(name, "--scale") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            uint64_t scale;
            if (v == NULL || !parse_u64(v, &scale) || scale < 1 || scale > 64) {
                fprintf(stderr, "gbemu: --scale needs a number from 1 to 64\n");
                return false;
            }
            o->scale = (int)scale;
        } else if (strcmp(name, "--press") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            if (o->press_count >= PRESS_MAX) {
                fprintf(stderr, "gbemu: at most %d --press events\n", PRESS_MAX);
                return false;
            }
            if (!parse_press(v, &o->press[o->press_count])) {
                fprintf(stderr, "gbemu: bad --press '%s' (want FRAME:BTN)\n", v);
                return false;
            }
            ++o->press_count;
        } else if (strcmp(name, "--break") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            if (o->break_count >= DEBUG_BP_MAX) {
                fprintf(stderr, "gbemu: at most %d --break points\n", DEBUG_BP_MAX);
                return false;
            }
            if (!parse_addr(v, &o->breaks[o->break_count])) {
                fprintf(stderr, "gbemu: bad --break address '%s'\n", v);
                return false;
            }
            ++o->break_count;
        } else if (strcmp(name, "--watch") == 0) {
            const char *v = need_value(argc, argv, &i, name, inline_value);
            if (v == NULL) return false;
            if (o->watch_count >= DEBUG_WP_MAX) {
                fprintf(stderr, "gbemu: at most %d --watch points\n", DEBUG_WP_MAX);
                return false;
            }
            if (!parse_addr(v, &o->watches[o->watch_count])) {
                fprintf(stderr, "gbemu: bad --watch address '%s'\n", v);
                return false;
            }
            ++o->watch_count;
        } else {
            fprintf(stderr, "gbemu: unknown option %s (try --help)\n", arg);
            return false;
        }
    }

    if (o->help) return true;   /* --help wins: no ROM required */
    if (o->list_audio_devices) return true;   /* so does the device listing */

    if (o->rom == NULL) {
        fprintf(stderr, "gbemu: no ROM given (try --help)\n");
        return false;
    }
    return true;
}

/* --- File I/O ----------------------------------------------------------- */

/* Read a whole file into a fresh heap buffer. Returns false and reports on error;
 * on success *out is owned by the caller. */
static bool read_file(const char *path, uint8_t **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "gbemu: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }

    /* ROMs have no length field in the stream, so size the buffer by seeking. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "gbemu: cannot seek %s: %s\n", path, strerror(errno));
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len <= 0) {
        fprintf(stderr, "gbemu: %s is empty or unreadable\n", path);
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "gbemu: cannot seek %s: %s\n", path, strerror(errno));
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc((size_t)len);
    if (buf == NULL) {
        fprintf(stderr, "gbemu: out of memory reading %s\n", path);
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "gbemu: short read on %s: %s\n", path, strerror(errno));
        free(buf);
        fclose(f);
        return false;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "gbemu: error closing %s: %s\n", path, strerror(errno));
        free(buf);
        return false;
    }

    *out = buf;
    *size = (size_t)len;
    return true;
}

/* Append `len` bytes to `path`, creating it if needed. */
static bool append_file(const char *path, const char *data, size_t len)
{
    if (len == 0) return true;   /* nothing to record */
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        fprintf(stderr, "gbemu: cannot append %s: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "gbemu: error writing %s\n", path);
    return ok;
}

/* Write the framebuffer as a 24-bit bottom-up BMP, by hand so that it works
 * headless (no SDL). The 54-byte header is the BITMAPFILEHEADER plus the
 * BITMAPINFOHEADER, with every field stored little-endian. */
static bool write_bmp(const char *path, const uint32_t *fb)
{
    enum {
        ROW_BYTES   = SCREEN_W * 3,             /* 24 bits per pixel */
        ROW_PADDED  = (ROW_BYTES + 3) & ~3      /* BMP rows align to 4 bytes */
    };
    const uint32_t image_bytes = (uint32_t)ROW_PADDED * SCREEN_H;

    uint8_t header[54];
    memset(header, 0, sizeof header);
    header[0] = 'B';                       /* the "BM" signature, two bytes */
    header[1] = 'M';
    put_le32(&header[2], 54u + image_bytes);  /* bfSize: header plus pixels */
    put_le32(&header[10], 54u);               /* bfOffBits: first pixel */
    put_le32(&header[14], 40u);               /* biSize: BITMAPINFOHEADER */
    put_le32(&header[18], SCREEN_W);
    put_le32(&header[22], SCREEN_H);
    put_le16(&header[26], 1u);                /* biPlanes */
    put_le16(&header[28], 24u);               /* biBitCount */
    put_le32(&header[34], image_bytes);        /* biSizeImage */

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "gbemu: cannot write %s: %s\n", path, strerror(errno));
        return false;
    }

    bool ok = fwrite(header, 1, sizeof header, f) == sizeof header;

    /* BMP rows run bottom-up: the first row in the file is the last on screen. */
    uint8_t row[ROW_PADDED];
    for (int y = SCREEN_H - 1; y >= 0 && ok; --y) {
        for (int x = 0; x < SCREEN_W; ++x) {
            /* XRGB8888 is 0x00RRGGBB; BMP wants 24-bit BGR. */
            uint32_t p = fb[(size_t)y * SCREEN_W + (size_t)x];
            row[x * 3 + 0] = (uint8_t)(p & 0xFFu);          /* blue  */
            row[x * 3 + 1] = (uint8_t)((p >> 8) & 0xFFu);   /* green */
            row[x * 3 + 2] = (uint8_t)((p >> 16) & 0xFFu);  /* red   */
        }
        /* The padding bytes must be zero for a well-formed BMP. */
        for (int i = ROW_BYTES; i < ROW_PADDED; ++i) row[i] = 0;
        ok = fwrite(row, 1, ROW_PADDED, f) == ROW_PADDED;
    }

    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "gbemu: error writing %s\n", path);
    return ok;
}

/* --- Frame hash and audio statistics ------------------------------------- */

/* FNV-1a over the framebuffer's bytes. Hashing the raw object representation
 * is deliberate: it catches any pixel change, not just the ones we thought of. */
static uint32_t hash_frame(const uint32_t *fb)
{
    uint32_t h = FNV_OFFSET_BASIS;
    const uint8_t *bytes = (const uint8_t *)fb;   /* unsigned char may alias */
    size_t n = (size_t)SCREEN_W * SCREEN_H * sizeof(uint32_t);
    for (size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* Fold freshly drained samples into the running audio statistics. */
static void audio_stats_add(AudioStats *s, const int16_t *samples, size_t frames)
{
    size_t count = frames * 2;   /* interleaved stereo */
    for (size_t i = 0; i < count; ++i) {
        int v = samples[i];
        int mag = v < 0 ? -v : v;   /* int is 32-bit, so -(-32768) is fine */
        if (mag > s->peak) s->peak = mag;
        s->sum_sq += (double)v * (double)v;
    }
    s->frames += frames;
    s->samples += count;
}

static void print_audio_stats(const AudioStats *s)
{
    double rms = s->samples > 0 ? sqrt(s->sum_sq / (double)s->samples) : 0.0;
    printf("audio: %" PRIu64 " frames drained, peak %d, rms %.2f\n",
           s->frames, s->peak, rms);
    if (s->stalls > 0)
        printf("audio: the playback device was more than a second behind for %" PRIu64
               " of the frames (use --list-audio-devices)\n", s->stalls);
}

/* --- Input -------------------------------------------------------------- */

/* Add every --press event whose 8-frame hold window covers this frame. */
static void apply_presses(const Options *o, uint64_t frame,
                         uint8_t *buttons, uint8_t *dpad)
{
    for (size_t i = 0; i < o->press_count; ++i) {
        const PressEvent *p = &o->press[i];
        if (frame < p->frame || frame >= p->frame + PRESS_HOLD_FRAMES) continue;
        /* Indices 0-3 are the d-pad bits, 4-7 the button bits: the two
         * groups are separate nibbles, exactly as JOYP exposes them. */
        if (p->button < 4) *dpad |= (uint8_t)(1u << p->button);
        else               *buttons |= (uint8_t)(1u << (p->button - 4));
    }
}

/* --- Log sinks ----------------------------------------------------------
 * The core calls gb_log(); the host decides where those lines go. Headless has no
 * SDL, so it prints to stdout; windowed sends them through SDL_Log. */

static void log_to_stdout(const char *line)
{
    fputs(line, stdout);
    fputc('\n', stdout);
}

/* --- Shared run epilogue ------------------------------------------------- */

/* Serial output, checksums, the frame dump and the save state. Shared by both
 * hosts so --headless and windowed agree on the post-run behaviour. */
static int finish_run(GB *gb, const Options *o, const AudioStats *stats,
                      bool print_serial)
{
    int status = 0;

    size_t len = 0;
    const char *serial = gb_serial_output(gb, &len);

    /* Headless runs exist to print the ROM's serial channel. */
    if (print_serial && serial != NULL && len > 0) {
        fwrite(serial, 1, len, stdout);
    }
    if (o->serial_log != NULL && serial != NULL && len > 0) {
        if (!append_file(o->serial_log, serial, len)) status = 1;
    }

    if (o->frame_hash) {
        printf("frame hash: %08" PRIx32 "\n", hash_frame(gb_framebuffer(gb)));
    }
    if (o->audio_stats) {
        print_audio_stats(stats);
    }
    if (o->dump_frame != NULL && !write_bmp(o->dump_frame, gb_framebuffer(gb))) {
        status = 1;
    }
    if (o->state_out != NULL && !gb_save_state(gb, o->state_out)) {
        fprintf(stderr, "gbemu: cannot save state to %s\n", o->state_out);
        status = 1;
    }
    return status;
}

/* --- Headless host ------------------------------------------------------ */

/* No window, no audio device, no SDL calls at all. Runs a bounded number of
 * frames and reports what the caller asked for. */
static int run_headless(GB *gb, const Options *o, AudioStats *stats)
{
    /* Stereo scratch for draining the APU ring; 2 int16 per stereo frame. */
    int16_t audio[APU_BUF_FRAMES * 2];

    /* --audio-dump writes the mix itself, so it can be analysed (a spectrum
     * shows a channel that never stops far better than a peak number does). */
    FILE *dump = o->audio_dump != NULL ? fopen(o->audio_dump, "wb") : NULL;
    if (o->audio_dump != NULL && dump == NULL) {
        fprintf(stderr, "gbemu: cannot write %s: %s\n", o->audio_dump, strerror(errno));
        return 1;
    }

    /* Without --frames, stop as soon as the ROM speaks or after a time cap, so a
     * headless run can never hang a CI job. */
    uint64_t limit = o->frames >= 0 ? (uint64_t)o->frames : HEADLESS_DEFAULT_FRAMES;

    for (uint64_t frame = 0; frame < limit; ++frame) {
        uint8_t buttons = 0;
        uint8_t dpad = 0;
        apply_presses(o, frame, &buttons, &dpad);
        gb_set_buttons(gb, buttons, dpad);

        gb_run_frame(gb);

        /* Draining always keeps the APU ring from filling up, and it is the only
         * source of the --audio-stats numbers. */
        size_t n = gb_drain_audio(gb, audio, APU_BUF_FRAMES);
        if (o->audio_stats) audio_stats_add(stats, audio, n);
        if (dump != NULL && n > 0)
            fwrite(audio, sizeof(int16_t), n * 2, dump);

        if (o->frames < 0) {
            size_t len = 0;
            (void)gb_serial_output(gb, &len);
            if (len > 0) break;   /* the test ROM has finished talking */
        }
    }

    if (dump != NULL) fclose(dump);
    return finish_run(gb, o, stats, true);
}

/* --- Windowed host (SDL) ------------------------------------------------ */

#ifdef GBEMU_HAVE_SDL

/* The core's log lines go through SDL_Log in a windowed build, which routes
 * them to the platform log alongside SDL's own messages. */
static void log_to_sdl(const char *line)
{
    SDL_Log("%s", line);
}

/* Case-insensitive substring test: strcasestr() is not C11, hence our own. */
static bool ascii_icontains(const char *hay, const char *needle)
{
    if (*needle == '\0') return true;
    for (; *hay != '\0'; ++hay) {
        const char *h = hay;
        const char *n = needle;
        while (*h != '\0' && *n != '\0') {
            unsigned char ch = (unsigned char)*h;
            unsigned char cn = (unsigned char)*n;
            if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
            if (cn >= 'A' && cn <= 'Z') cn = (unsigned char)(cn - 'A' + 'a');
            if (ch != cn) break;
            ++h;
            ++n;
        }
        if (*n == '\0') return true;
    }
    return false;
}

/* list_audio_devices — print every playback device SDL can see.
 *
 * This exists because the system default is often a monitor with no speakers: the
 * emulator then produces correct audio that nobody can hear. Students hit this on
 * their first laptop, so the CLI has to be able to show the choice. */
static int list_audio_devices(void)
{
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        fprintf(stderr, "gbemu: SDL_Init(SDL_INIT_AUDIO): %s\n", SDL_GetError());
        return 1;
    }

    int count = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices == NULL) {
        fprintf(stderr, "gbemu: SDL_GetAudioPlaybackDevices: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    const char *current = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    printf("playback devices (system default: %s):\n", current ? current : "none");
    for (int i = 0; i < count; ++i)
        printf("  %s\n", SDL_GetAudioDeviceName(devices[i]));

    SDL_free(devices);
    SDL_Quit();
    return 0;
}

/* find_audio_device — the first playback device whose name contains `name`.
 *
 * A substring match is deliberate: device names carry the OS's own suffixes, so
 * "--audio-device speakers" must work on a machine whose device is called
 * "MacBook Pro Speakers". Returns 0 when nothing matches. */
static SDL_AudioDeviceID find_audio_device(const char *name)
{
    int count = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices == NULL) return 0;

    SDL_AudioDeviceID found = 0;
    for (int i = 0; i < count; ++i) {
        const char *device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name != NULL && ascii_icontains(device_name, name)) {
            found = devices[i];
            break;
        }
    }

    SDL_free(devices);
    return found;
}

/* Open the first gamepad SDL knows about, or NULL when there is none. */
static SDL_Gamepad *open_first_gamepad(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids == NULL) return NULL;

    SDL_Gamepad *gp = NULL;
    if (count > 0) gp = SDL_OpenGamepad(ids[0]);

    SDL_free(ids);   /* the array is ours once SDL_GetGamepads returns it */
    return gp;
}

/* Keyboard mapping: arrows are the d-pad, X is A, Z is B, Return is START and
 * Backspace is SELECT. The scancode array is polled, so held keys just work. */
static void read_keyboard(uint8_t *buttons, uint8_t *dpad)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    if (keys == NULL) return;

    if (keys[SDL_SCANCODE_RIGHT]) *dpad |= JOY_RIGHT;
    if (keys[SDL_SCANCODE_LEFT])  *dpad |= JOY_LEFT;
    if (keys[SDL_SCANCODE_UP])    *dpad |= JOY_UP;
    if (keys[SDL_SCANCODE_DOWN])  *dpad |= JOY_DOWN;
    if (keys[SDL_SCANCODE_X])       *buttons |= JOY_A;
    if (keys[SDL_SCANCODE_Z])       *buttons |= JOY_B;
    if (keys[SDL_SCANCODE_RETURN])  *buttons |= JOY_START;
    if (keys[SDL_SCANCODE_BACKSPACE]) *buttons |= JOY_SELECT;
}

/* Merge the first gamepad's state, if one is open. */
static void read_gamepad(SDL_Gamepad *gp, uint8_t *buttons, uint8_t *dpad)
{
    if (gp == NULL) return;

    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) *dpad |= JOY_RIGHT;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  *dpad |= JOY_LEFT;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP))    *dpad |= JOY_UP;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  *dpad |= JOY_DOWN;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))      *buttons |= JOY_A;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))       *buttons |= JOY_B;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))      *buttons |= JOY_START;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))       *buttons |= JOY_SELECT;
}

/* Release everything SDL gave us and quit the subsystems we started. */
static void shutdown_sdl(SDL_Texture *tex, SDL_Renderer *renderer,
                        SDL_Window *window, SDL_AudioStream *stream,
                        SDL_Gamepad *gamepad)
{
    if (gamepad != NULL) SDL_CloseGamepad(gamepad);
    if (stream != NULL) SDL_DestroyAudioStream(stream);
    if (tex != NULL) SDL_DestroyTexture(tex);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (window != NULL) SDL_DestroyWindow(window);
    SDL_Quit();
}

static int run_windowed(GB *gb, const Options *o, AudioStats *stats)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gbemu: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const int w = SCREEN_W * o->scale;
    const int h = SCREEN_H * o->scale;
    SDL_Window *window = SDL_CreateWindow("gbemu", w, h, 0);
    if (window == NULL) {
        fprintf(stderr, "gbemu: SDL_CreateWindow: %s\n", SDL_GetError());
        shutdown_sdl(NULL, NULL, NULL, NULL, NULL);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (renderer == NULL) {
        fprintf(stderr, "gbemu: SDL_CreateRenderer: %s\n", SDL_GetError());
        shutdown_sdl(NULL, NULL, window, NULL, NULL);
        return 1;
    }

    /* A streaming texture in the framebuffer's own format; NEAREST keeps the
     * pixels crisp when the window is scaled up. */
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         SCREEN_W, SCREEN_H);
    if (tex == NULL) {
        fprintf(stderr, "gbemu: SDL_CreateTexture: %s\n", SDL_GetError());
        shutdown_sdl(NULL, renderer, window, NULL, NULL);
        return 1;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderVSync(renderer, 1);   /* present at most once per refresh */

    /* S16 stereo at the core's nominal rate. The stream's SOURCE format is what
     * we queue and its DESTINATION format is what the device consumes; SDL
     * resamples between them, so the APU generates at the source rate. */
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = SAMPLE_RATE;

    /* The system default is often a monitor with no speakers, which produces
     * silence that no amount of APU debugging can explain, so the device is
     * selectable by name. */
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (o->audio_device != NULL) {
        device = find_audio_device(o->audio_device);
        if (device == 0) {
            fprintf(stderr, "gbemu: no playback device matching '%s'; "
                            "try --list-audio-devices\n", o->audio_device);
            shutdown_sdl(tex, renderer, window, NULL, NULL);
            return 1;
        }
    }

    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(device, &spec, NULL, NULL);
    if (stream == NULL) {
        fprintf(stderr, "gbemu: SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
        shutdown_sdl(tex, renderer, window, NULL, NULL);
        return 1;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        /* The emulator still runs without audio, but silence that is not
         * explained is the worst kind of bug, so say so and carry on. */
        fprintf(stderr, "gbemu: SDL_ResumeAudioStreamDevice: %s\n", SDL_GetError());
    }

    /* An audio device stream has TWO formats: the source is what the host queues
     * and the destination is what the device consumes, and SDL resamples between
     * them. The APU must generate at the SOURCE rate, because that is the rate
     * the samples we queue are in. Generating at the destination rate instead
     * plays everything at the wrong pitch whenever the device rate differs. */
    SDL_AudioSpec src, dst;
    if (SDL_GetAudioStreamFormat(stream, &src, &dst)) {
        const char *name = SDL_GetAudioDeviceName(device);
        SDL_Log("audio: queue %d Hz %d ch fmt %d -> device %d Hz %d ch fmt %d ('%s')",
                src.freq, src.channels, (int)src.format,
                dst.freq, dst.channels, (int)dst.format,
                name ? name : "default");
        apu_set_sample_rate(&gb->mmu.apu, src.freq);
    } else {
        fprintf(stderr, "gbemu: SDL_GetAudioStreamFormat: %s\n", SDL_GetError());
    }

    SDL_Gamepad *gamepad = open_first_gamepad();

    /* --audio-dump writes the mix itself; a spectrum shows a channel that never
     * stops far better than a peak number does. */
    FILE *dump = o->audio_dump != NULL ? fopen(o->audio_dump, "wb") : NULL;
    if (o->audio_dump != NULL && dump == NULL)
        fprintf(stderr, "gbemu: cannot write %s: %s\n", o->audio_dump, strerror(errno));

    int16_t audio[APU_BUF_FRAMES * 2];
    bool running = true;
    uint64_t frame = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       ev.window.windowID == SDL_GetWindowID(window)) {
                running = false;   /* the user clicked the window's close box */
            } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && gamepad == NULL) {
                gamepad = SDL_OpenGamepad(ev.gdevice.which);
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad != NULL &&
                       SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
            }
        }
        if (!running) break;

        if (o->frames >= 0 && frame >= (uint64_t)o->frames) break;

        /* The host pushes the whole input state once per frame. */
        uint8_t buttons = 0;
        uint8_t dpad = 0;
        read_keyboard(&buttons, &dpad);
        read_gamepad(gamepad, &buttons, &dpad);
        apply_presses(o, frame, &buttons, &dpad);
        gb_set_buttons(gb, buttons, dpad);

        gb_run_frame(gb);

        /* Audio must be drained every frame or the stream underruns. */
        size_t n = gb_drain_audio(gb, audio, APU_BUF_FRAMES);

        /* Queue the samples. On a device stream SDL blocks once the device's
         * buffer is full, which is what paces the emulator at the Game Boy's own
         * 59.7275 frames per second rather than the display's 60: that is the
         * correct clock, and it is why nothing here drops samples. Samples are
         * never dropped to catch up: a drop is a click in the sound. */
        if (n > 0) {
            SDL_PutAudioStreamData(stream, audio, (int)(n * 2 * sizeof(int16_t)));
            if (dump != NULL) fwrite(audio, sizeof(int16_t), n * 2, dump);
        }
        if (o->audio_stats) audio_stats_add(stats, audio, n);

        /* A device that is playing keeps the queue near a frame or two. A queue
         * that only grows means nothing is consuming it, so say so once with the
         * size of the backlog, and let a student pick another device with
         * --list-audio-devices. The queue is never cut to catch up: cutting it
         * is a gap in the sound, and a growing queue is only latency. */
        if (o->audio_stats && frame % 60u == 59u) {
            int queued = SDL_GetAudioStreamQueued(stream);
            SDL_Log("audio: queued %d bytes (%.2f s) after %" PRIu64 " frames",
                    queued, (double)queued / (4.0 * (double)apu_sample_rate(&gb->mmu.apu)),
                    frame + 1u);
        }
        {
            int queued = SDL_GetAudioStreamQueued(stream);
            int limit = 4 * apu_sample_rate(&gb->mmu.apu);   /* 1 s stereo */
            if (queued > limit && stats->stalls == 0) {
                SDL_Log("audio: the playback device is %.2f s behind; is it the "
                        "device you are listening to? (try --list-audio-devices)",
                        (double)queued / (4.0 * (double)apu_sample_rate(&gb->mmu.apu)));
            }
            if (queued > limit) stats->stalls++;
        }

        SDL_UpdateTexture(tex, NULL, gb_framebuffer(gb),
                          SCREEN_W * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, tex, NULL, NULL);
        SDL_RenderPresent(renderer);   /* vsync paces the loop here */

        /* Pace by the Game Boy's own clock, not by the display's. One frame
         * is 70224 T-cycles at 4194304 Hz, i.e. 16742706 ns, which is
         * 59.7275 frames per second. No display refreshes at that rate, so
         * vsync alone runs the machine at the display's rate instead: on this
         * machine (120 Hz) the host produced two seconds of audio per second
         * of play and the queue grew without bound until it had to be cut,
         * which is audible. The deadline is absolute, so the vsync wait above
         * and this sleep together total one Game Boy frame rather than adding
         * up. After a long stall (a breakpoint, a paused process) the schedule
         * is resynced rather than trying to catch up in a burst. */
        {
            static uint64_t due_ns;
            uint64_t now = SDL_GetTicksNS();
            if (due_ns == 0u || now > due_ns + 100000000u) due_ns = now;
            due_ns += 16742706u;   /* 70224 / 4194304 s */
            if (due_ns > now) SDL_DelayPrecise(due_ns - now);
        }

        ++frame;
    }

    if (dump != NULL) fclose(dump);
    shutdown_sdl(tex, renderer, window, stream, gamepad);
    return finish_run(gb, o, stats, false);
}

#endif /* GBEMU_HAVE_SDL */

/* --- Entry point -------------------------------------------------------- */

int main(int argc, char **argv)
{
    Options o;
    if (!parse_args(argc, argv, &o)) return 1;
    if (o.help) {
        print_usage(stdout);
        return 0;
    }

#ifdef GBEMU_HAVE_SDL
    if (o.list_audio_devices) return list_audio_devices();
#else
    if (o.list_audio_devices) {
        fprintf(stderr, "gbemu: this build has no SDL audio\n");
        return 1;
    }
#endif

#ifndef GBEMU_HAVE_SDL
    /* A build without SDL has exactly one host: the headless one. */
    if (!o.headless) {
        fprintf(stderr, "gbemu: this build has no SDL support; use --headless\n");
        return 1;
    }
#endif

    /* Install the log sink before anything can log. Headless must not touch SDL,
     * so it always prints to stdout. */
#ifdef GBEMU_HAVE_SDL
    gb_log_set_sink(o.headless ? log_to_stdout : log_to_sdl);
#else
    gb_log_set_sink(log_to_stdout);
#endif

    /* The ROM is copied by gb_load_rom(), but the boot ROM is borrowed by the
     * MMU, so both buffers are kept alive until the machine is freed. */
    uint8_t *rom = NULL;
    size_t rom_size = 0;
    if (!read_file(o.rom, &rom, &rom_size)) return 1;

    uint8_t *boot = NULL;
    size_t boot_size = 0;
    if (o.boot_rom != NULL && !read_file(o.boot_rom, &boot, &boot_size)) {
        free(rom);
        return 1;
    }

    GB gb;
    if (!gb_init(&gb, o.cgb)) {
        fprintf(stderr, "gbemu: gb_init failed\n");
        free(rom);
        free(boot);
        return 1;
    }

    if (boot != NULL) gb_set_boot_rom(&gb, boot, boot_size);

    if (!gb_load_rom(&gb, rom, rom_size, o.rom)) {
        fprintf(stderr, "gbemu: cannot load ROM %s\n", o.rom);
        gb_free(&gb);
        free(rom);
        free(boot);
        return 1;
    }

    /* --trace only decides whether the debugger prints; it always records. */
    if (o.trace) debug_trace_enable(&gb.debug, true);

    /* --mute-ch silences a channel in the mixer for diagnosis only. */
    if (o.mute != 0u) apu_set_mute(&gb.mmu.apu, o.mute);

    /* The debugger's breakpoint and watchpoint tables are the host's to fill. */
    for (size_t i = 0; i < o.break_count; ++i) {
        gb.debug.bp[i].addr = o.breaks[i];
        gb.debug.bp[i].enabled = true;
    }
    for (size_t i = 0; i < o.watch_count; ++i) {
        gb.debug.wp[i].addr = o.watches[i];
        gb.debug.wp[i].enabled = true;
        gb.debug.wp_on_write[i] = true;   /* read and write, per the docs */
    }

    /* With the tables installed the debugger is observing, so the trace ring
     * records; without this the per-instruction ring is skipped entirely. */
    debug_attach(&gb.debug);

    if (o.debug) debug_repl(&gb);

    if (o.state_in != NULL && !gb_load_state(&gb, o.state_in)) {
        fprintf(stderr, "gbemu: cannot load state from %s\n", o.state_in);
        gb_free(&gb);
        free(rom);
        free(boot);
        return 1;
    }

    AudioStats stats;
    memset(&stats, 0, sizeof stats);

    int status;
#ifdef GBEMU_HAVE_SDL
    status = o.headless ? run_headless(&gb, &o, &stats)
                        : run_windowed(&gb, &o, &stats);
#else
    status = run_headless(&gb, &o, &stats);
#endif

    gb_free(&gb);
    free(rom);
    free(boot);
    return status;
}
