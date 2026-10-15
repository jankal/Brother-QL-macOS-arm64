/*
 * rastertoql.c - CUPS raster filter for Brother QL series label printers
 *
 * Converts CUPS raster data to Brother QL ESC/P raster format.
 * Supports QL-800 and QL-1100.
 *
 * Usage (invoked by CUPS):
 *   rastertoql job-id user title copies options [filename]
 *
 * Compile: cc -O2 -arch arm64 -o rastertoql rastertoql.c -lcups
 *
 * Note: We intentionally avoid the PPD C API (ppd_file_t, ppdOpenFile, etc.)
 * because Apple removed those headers from macOS 26+. Instead we read all
 * job options from argv[5] (the CUPS options string) and the CUPS raster
 * page header, which carry all the information we need at filter runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>

#include <cups/cups.h>
#include <cups/raster.h>

#include "ql_protocol.h"

/* -----------------------------------------------------------------------
 * Globals
 * --------------------------------------------------------------------- */
static int g_cancelled = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_cancelled = 1;
}

/* -----------------------------------------------------------------------
 * Write a block of bytes to stdout (→ CUPS → printer)
 * --------------------------------------------------------------------- */
static void write_bytes(const uint8_t *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
}

static void write_byte(uint8_t b)
{
    fputc(b, stdout);
}

/* -----------------------------------------------------------------------
 * Send the invalidate sequence (200 x 0x00)
 * --------------------------------------------------------------------- */
static void send_invalidate(void)
{
    uint8_t buf[QL_INVALIDATE_COUNT];
    memset(buf, QL_INVALIDATE_BYTE, sizeof(buf));
    write_bytes(buf, sizeof(buf));
}

/* -----------------------------------------------------------------------
 * Send initialize command  ESC @
 * --------------------------------------------------------------------- */
static void send_initialize(void)
{
    uint8_t cmd[] = { QL_CMD_INIT_0, QL_CMD_INIT_1 };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send switch-to-raster command  ESC i a 0x01
 * --------------------------------------------------------------------- */
static void send_raster_mode(void)
{
    uint8_t cmd[] = { QL_CMD_RASTER_0, QL_CMD_RASTER_1,
                      QL_CMD_RASTER_2, QL_CMD_RASTER_MODE };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send print information command  ESC i z <10 bytes>
 * --------------------------------------------------------------------- */
static void send_print_info(const ql_media_info_t *media,
                            int raster_lines,
                            int first_page,
                            int quality)
{
    uint8_t cmd[3 + QL_PRINT_INFO_LEN];
    cmd[0]  = QL_CMD_INFO_0;
    cmd[1]  = QL_CMD_INFO_1;
    cmd[2]  = QL_CMD_INFO_2;
    cmd[3]  = QL_INFO_VALID_ALL;                 /* valid flags */
    if (!quality) cmd[3] &= (uint8_t)~QL_INFO_VALID_QUALITY;
    cmd[4]  = media->media_type;
    cmd[5]  = (uint8_t)media->label_width_mm;
    cmd[6]  = (uint8_t)media->label_length_mm;   /* 0 for continuous */
    /* raster line count, little-endian 32-bit */
    cmd[7]  = (uint8_t)( raster_lines        & 0xFF);
    cmd[8]  = (uint8_t)((raster_lines >>  8) & 0xFF);
    cmd[9]  = (uint8_t)((raster_lines >> 16) & 0xFF);
    cmd[10] = (uint8_t)((raster_lines >> 24) & 0xFF);
    cmd[11] = first_page ? 0x00 : 0x01;          /* starting page / other */
    cmd[12] = 0x00;                              /* fixed */
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send autocut mode command  ESC i M <flags>
 * --------------------------------------------------------------------- */
static void send_autocut(int enable)
{
    uint8_t cmd[] = { QL_CMD_AUTOCUT_0, QL_CMD_AUTOCUT_1,
                      QL_CMD_AUTOCUT_2,
                      (uint8_t)(enable ? QL_MODE_AUTOCUT : 0x00) };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send cut-every-N-labels  ESC i A <n>
 * --------------------------------------------------------------------- */
static void send_cut_every(int n)
{
    uint8_t cmd[] = { QL_CMD_CUTEVERY_0, QL_CMD_CUTEVERY_1,
                      QL_CMD_CUTEVERY_2, (uint8_t)n };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send expanded mode  ESC i K <flags>
 * --------------------------------------------------------------------- */
static void send_expanded_mode(int cut_at_end)
{
    /* Do NOT set QL_EXPMODE_HIGHRES: that switches the printer to
       300x600dpi and it then expects twice as many raster lines. */
    uint8_t flags = 0x00;
    if (cut_at_end) flags |= QL_EXPMODE_CUTEND;
    uint8_t cmd[] = { QL_CMD_EXPMODE_0, QL_CMD_EXPMODE_1,
                      QL_CMD_EXPMODE_2, flags };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Send margins (feed amount in dots)  ESC i d <lo> <hi>
 * --------------------------------------------------------------------- */
static void send_margins(int dots)
{
    uint8_t cmd[] = { QL_CMD_MARGINS_0, QL_CMD_MARGINS_1,
                      QL_CMD_MARGINS_2,
                      (uint8_t)(dots & 0xFF),
                      (uint8_t)((dots >> 8) & 0xFF) };
    write_bytes(cmd, sizeof(cmd));
}

/* -----------------------------------------------------------------------
 * Darkness of source pixel x, 0 = white .. 255 = black.  Handles whatever
 * cgpdftoraster produced: 1-bit or 8-bit, and either polarity
 * (K: high = black; W/sGray: 0 = black).
 * --------------------------------------------------------------------- */
static int pixel_darkness(const uint8_t *src, unsigned x,
                          const cups_page_header2_t *h, int black_is_high)
{
    int v;
    if (h->cupsBitsPerPixel == 1)
        v = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0;
    else
        v = src[x * (h->cupsBitsPerPixel / 8)];  /* first channel */
    return black_is_high ? v : 255 - v;
}

/* -----------------------------------------------------------------------
 * Halftoning: grey darkness → black/white dots
 * --------------------------------------------------------------------- */
typedef enum {
    HALFTONE_BINARY = 0,        /* threshold — logos and text */
    HALFTONE_DITHER,            /* 8x8 ordered dither — text and graphics */
    HALFTONE_ERROR_DIFFUSION    /* Floyd-Steinberg — images */
} halftone_t;

static const uint8_t BAYER8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* Convert one line of darkness values (0..255) to dots (0/1) in place.
   err_cur/err_next are print_dots+2 ints used by error diffusion; the
   caller swaps and clears them between lines. */
static void halftone_line(const int *dark, uint8_t *dots, int n, int y,
                          halftone_t mode, int *err_cur, int *err_next)
{
    for (int x = 0; x < n; x++) {
        int v = dark[x];
        switch (mode) {
        case HALFTONE_BINARY:
            dots[x] = v >= 128;
            break;
        case HALFTONE_DITHER:
            dots[x] = v * 64 > BAYER8[y & 7][x & 7] * 255 + 127;
            break;
        case HALFTONE_ERROR_DIFFUSION: {
            v += err_cur[x + 1] / 16;
            int out = v >= 128 ? 255 : 0;
            int e = v - out;
            dots[x] = out != 0;
            err_cur[x + 2]  += e * 7;
            err_next[x]     += e * 3;
            err_next[x + 1] += e * 5;
            err_next[x + 2] += e * 1;
            break;
        }
        }
    }
}

/* -----------------------------------------------------------------------
 * Send one raster line
 *
 * The Brother protocol expects exactly `raster_bytes` bytes per line,
 * 1 = black, and the image mirrored left-to-right (the first bit sent is
 * the right-hand edge of the label as you look at it).
 *
 * `dots` holds one byte per printable dot across the label (non-zero =
 * black), or NULL for a blank line.  The label's printable dots end
 * `offset_r` dots before the end of the raster line (matches the
 * brother_ql reference driver).
 * --------------------------------------------------------------------- */
static void send_raster_line(const uint8_t *dots,
                             int raster_bytes,
                             int print_dots,
                             int offset_r)
{
    uint8_t line[256];   /* max raster_bytes for any QL model */
    if (raster_bytes > (int)sizeof(line)) raster_bytes = (int)sizeof(line);
    memset(line, 0x00, raster_bytes);  /* fill white */

    for (int d = 0; dots && d < print_dots; d++) {
        if (!dots[d]) continue;
        /* label dot d (0 = left edge) → mirrored head position */
        int pos = raster_bytes * 8 - 1 - offset_r - d;
        if (pos >= 0 && pos < raster_bytes * 8)
            line[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
    }

    write_byte(QL_RASTER_CMD_0);
    write_byte(QL_RASTER_CMD_1);
    write_byte((uint8_t)raster_bytes);
    write_bytes(line, raster_bytes);
}

/* -----------------------------------------------------------------------
 * Send end-of-page / print command
 * is_last_page: true → print with feed; false → print, more pages follow
 * --------------------------------------------------------------------- */
static void send_print(int is_last_page)
{
    write_byte(is_last_page ? QL_CMD_PRINT_FEED : QL_CMD_PRINT_NOFEED);
}

/* -----------------------------------------------------------------------
 * Look up media info by PPD MediaType string
 * --------------------------------------------------------------------- */
static const ql_media_info_t *find_media(const ql_media_info_t *table,
                                          const char *name)
{
    for (int i = 0; table[i].ppd_name != NULL; i++) {
        if (strcasecmp(table[i].ppd_name, name) == 0)
            return &table[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Look up printer info by model name
 * --------------------------------------------------------------------- */
static const ql_printer_info_t *find_printer(const char *model_name)
{
    for (int i = 0; QL_PRINTERS[i].model_name != NULL; i++) {
        if (strcasecmp(QL_PRINTERS[i].model_name, model_name) == 0)
            return &QL_PRINTERS[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Parse a named option from the CUPS options string (argv[5]).
 *
 * CUPS passes job options as "key=value key2=value2 ..." in argv[5].
 * cupsGetOption() searches a parsed cups_option_t array for us.
 *
 * Returns the value string, or NULL if not found.
 * Caller must cupsArrayDelete / cupsFreeOptions when done with opts.
 * --------------------------------------------------------------------- */
static const char *get_job_option(int num_options, cups_option_t *options,
                                  const char *keyword)
{
    return cupsGetOption(keyword, num_options, options);
}

/* -----------------------------------------------------------------------
 * Determine printer model from the PRINTER environment variable.
 * CUPS sets PRINTER to the queue name, which we set to match the PPD
 * ModelName ("QL-1100" or "QL-800") when the queue is created.
 * We also accept the queue names "Brother_QL_1100" etc. (macOS replaces
 * spaces with underscores).
 * --------------------------------------------------------------------- */
static const char *get_printer_model(void)
{
    /* Best source: *ModelName in the queue's PPD (CUPS sets $PPD) */
    const char *ppd = getenv("PPD");
    FILE *fp = ppd ? fopen(ppd, "r") : NULL;
    if (fp) {
        char buf[256];
        const char *found = NULL;
        while (!found && fgets(buf, sizeof(buf), fp)) {
            if (strncmp(buf, "*ModelName:", 11) != 0) continue;
            if (strstr(buf, "QL-1100")) found = "QL-1100";
            else if (strstr(buf, "QL-800")) found = "QL-800";
            else break;
        }
        fclose(fp);
        if (found) return found;
    }

    const char *p = getenv("PRINTER");
    if (!p) p = "";
    if (strstr(p, "1100") || strstr(p, "QL1100"))  return "QL-1100";
    if (strstr(p, "800")  || strstr(p, "QL800"))   return "QL-800";
    /* Fall back: check DEVICE_URI */
    p = getenv("DEVICE_URI");
    if (p && (strstr(p, "1100"))) return "QL-1100";
    if (p && (strstr(p, "800")))  return "QL-800";
    return "QL-1100";  /* safest default — wider machine */
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* CUPS filter argument check */
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: rastertoql job-id user title copies options [filename]\n");
        return 1;
    }

    /* Set up signal handling for cancellation */
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, handle_signal);

    /* Parse job options from argv[5] — no PPD API needed */
    int num_options = 0;
    cups_option_t *options = NULL;
    num_options = cupsParseOptions(argv[5], num_options, &options);

    /* Determine printer model */
    const char *model_name = get_printer_model();

    /* Also accept an explicit "Model" job option to override, e.g.
       lp -o Model=QL-800 ... */
    const char *model_override = get_job_option(num_options, options, "Model");
    if (model_override) model_name = model_override;

    /* Select media table and printer geometry */
    const ql_media_info_t  *media_table   = QL1100_MEDIA;
    const ql_printer_info_t *printer_info = find_printer("QL-1100");

    if (strstr(model_name, "800")) {
        media_table   = QL800_MEDIA;
        printer_info  = find_printer("QL-800");
    } else if (strstr(model_name, "1100")) {
        media_table   = QL1100_MEDIA;
        printer_info  = find_printer("QL-1100");
    }

    if (!printer_info) {
        fprintf(stderr, "ERROR: rastertoql: unknown printer model: %s\n", model_name);
        cupsFreeOptions(num_options, options);
        return 1;
    }

    fprintf(stderr, "INFO: rastertoql: model=%s\n", printer_info->model_name);

    /* Determine media type from job options; default to first entry */
    const char *media_type_str = get_job_option(num_options, options, "MediaType");
    if (!media_type_str || !*media_type_str)
        media_type_str = media_table[0].ppd_name;

    const ql_media_info_t *media = find_media(media_table, media_type_str);
    if (!media) {
        fprintf(stderr, "WARNING: rastertoql: unknown media '%s', using first entry\n",
                media_type_str);
        media = &media_table[0];
    }

    fprintf(stderr, "INFO: rastertoql: media=%s (%dx%dmm)\n",
            media->ppd_name, media->label_width_mm, media->label_length_mm);

    /* Autocut option — default on */
    /* AutoCut: True = cut after every label and at the end of the job,
       EndOnly = one cut after the last label, False = never cut */
    const char *autocut_str = get_job_option(num_options, options, "AutoCut");
    int autocut = 1, cut_at_end = 1;
    if (autocut_str && strcasecmp(autocut_str, "False") == 0)
        autocut = cut_at_end = 0;
    else if (autocut_str && strcasecmp(autocut_str, "EndOnly") == 0)
        autocut = 0;

    /* Horizontal adjust in whole mm, e.g. "Plus2" / "Minus1" / "None".
       Positive moves the print to the right. */
    const char *hadj_str = get_job_option(num_options, options, "HorizontalAdjust");
    int hadj_mm = 0;
    if (hadj_str && strncasecmp(hadj_str, "Plus", 4) == 0)  hadj_mm =  atoi(hadj_str + 4);
    if (hadj_str && strncasecmp(hadj_str, "Minus", 5) == 0) hadj_mm = -atoi(hadj_str + 5);
    int hadj_dots = hadj_mm * printer_info->dpi * 10 / 254;

    /* LabelScale: percentage ("98") or "Fit" (largest size that fits the
       printable area).  The page arrives full-size, edges included, and is
       scaled about its centre before being trimmed to the printable dots. */
    const char *scale_str = get_job_option(num_options, options, "LabelScale");
    int scale_fit = 0;
    double scale  = 0.98;
    if (scale_str && strcasecmp(scale_str, "Fit") == 0)
        scale_fit = 1;
    else if (scale_str && atoi(scale_str) >= 50 && atoi(scale_str) <= 100)
        scale = atoi(scale_str) / 100.0;

    /* PrintQuality: Quality (default) sets the printer's quality-priority
       flag; Speed clears it */
    const char *quality_str = get_job_option(num_options, options, "PrintQuality");
    int quality = !(quality_str && strcasecmp(quality_str, "Speed") == 0);

    /* Halftone / Brightness / Contrast (both -20..20, as Brother's driver) */
    const char *ht_str = get_job_option(num_options, options, "Halftone");
    halftone_t halftone = HALFTONE_ERROR_DIFFUSION;
    if (ht_str && strcasecmp(ht_str, "Binary") == 0) halftone = HALFTONE_BINARY;
    if (ht_str && strcasecmp(ht_str, "Dither") == 0) halftone = HALFTONE_DITHER;
    const char *br_str = get_job_option(num_options, options, "Brightness");
    const char *ct_str = get_job_option(num_options, options, "Contrast");
    int brightness = br_str ? atoi(br_str) : 0;
    int contrast   = ct_str ? atoi(ct_str) : 0;
    if (brightness < -20) brightness = -20;
    if (brightness >  20) brightness =  20;
    if (contrast   < -20) contrast   = -20;
    if (contrast   >  20) contrast   =  20;

    /* Darkness lookup.  Both curves keep white white and black black, so
       backgrounds never turn into speckle:
         brightness: gamma, +20 = u^2 (lighter), -20 = u^0.5 (darker)
         contrast:   blend towards an S-curve (+) or its inverse (-) */
    int tone[256];
    for (int v = 0; v < 256; v++) {
        double u = pow(v / 255.0, pow(2.0, brightness / 20.0));
        if (contrast > 0)
            u += contrast / 20.0 * (u * u * (3 - 2 * u) - u);
        else if (contrast < 0)
            u += -contrast / 20.0 * ((0.5 - sin(asin(1 - 2 * u) / 3)) - u);
        int t = (int)(u * 255 + 0.5);
        tone[v] = t < 0 ? 0 : t > 255 ? 255 : t;
    }

    fprintf(stderr, "DEBUG: rastertoql: quality=%d halftone=%d brightness=%d contrast=%d\n",
            quality, (int)halftone, brightness, contrast);

    /* Open raster input */
    int fd = 0; /* stdin */
    if (argc == 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "ERROR: rastertoql: cannot open '%s'\n", argv[6]);
            cupsFreeOptions(num_options, options);
            return 1;
        }
    }

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        fprintf(stderr, "ERROR: rastertoql: cupsRasterOpen failed\n");
        cupsFreeOptions(num_options, options);
        return 1;
    }

    /* ----------------------------------------------------------------
     * Main page loop
     * -------------------------------------------------------------- */
    cups_page_header2_t header;
    int page_count  = 0;
    int copies      = atoi(argv[4]);
    if (copies < 1) copies = 1;

    /* We don't know the total page count in advance from CUPS raster,
       so we read one page at a time and use 0x0C (no-feed) between pages
       and 0x1A (feed+cut) on the last. We track "previous page" to emit
       its print command once we know whether there's a next page. */

    /* First: send job-level init sequence */
    send_invalidate();
    send_initialize();
    send_raster_mode();

    int raster_bytes = printer_info->raster_bytes;
    uint8_t *line_buf = NULL;
    uint8_t *page_buf = NULL;

    while (!g_cancelled && cupsRasterReadHeader2(ras, &header)) {

        /* The previous page is finished and another follows: 0x0C */
        if (page_count > 0)
            send_print(0);

        page_count++;
        fprintf(stderr, "PAGE: %d %d\n", page_count, copies);
        fprintf(stderr, "INFO: Processing page %d (%ux%u, %u bpp, colorspace %d)\n",
                page_count, header.cupsWidth, header.cupsHeight,
                header.cupsBitsPerPixel, (int)header.cupsColorSpace);

        /* The page size chosen in the print dialog (e.g. "DK11209") wins
           over MediaType, so the printer is told the roll actually used */
        const ql_media_info_t *page_media = find_media(media_table,
                                                       header.cupsPageSizeName);
        if (page_media && page_media != media) {
            fprintf(stderr, "INFO: rastertoql: using media %s from page size\n",
                    page_media->ppd_name);
            media = page_media;
        }
        int is_diecut  = media->media_type == QL_MEDIA_DIECUT;
        int print_dots = media->print_dots_wide;
        int offset_r   = media->offset_r + printer_info->additional_offset_r
                         + hadj_dots;
        /* Keep the whole label inside the head */
        int max_offset = raster_bytes * 8 - print_dots;
        if (offset_r < 0) offset_r = 0;
        if (offset_r > max_offset) offset_r = max_offset;

        /* K (and KCMY etc.) put black at the high value; W/sGray/RGB at 0 */
        int black_is_high = header.cupsColorSpace == CUPS_CSPACE_K ||
                            header.cupsColorSpace == CUPS_CSPACE_CMYK ||
                            header.cupsColorSpace == CUPS_CSPACE_KCMY;

        /* Die-cut labels must be sent with exactly the label's line count
           or the printer rejects the job (red LED). */
        int raster_lines = is_diecut ? media->print_dots_long : 0;

        /* Read the whole page as one byte per pixel (1 = black).
           CUPS requires every line to be read. */
        int src_w = (int)header.cupsWidth;
        int src_h = (int)header.cupsHeight;
        free(line_buf);
        free(page_buf);
        line_buf = malloc(header.cupsBytesPerLine);
        page_buf = calloc((size_t)src_w * (size_t)src_h, 1);
        uint8_t *dots = malloc((size_t)print_dots);
        int *dark     = malloc(sizeof(int) * (size_t)print_dots);
        int *err_cur  = calloc((size_t)print_dots + 2, sizeof(int));
        int *err_next = calloc((size_t)print_dots + 2, sizeof(int));
        if (!line_buf || !page_buf || !dots || !dark || !err_cur || !err_next) {
            fprintf(stderr, "ERROR: rastertoql: out of memory\n");
            free(dots); free(dark); free(err_cur); free(err_next);
            g_cancelled = 1;
            break;
        }
        for (int y = 0; y < src_h && !g_cancelled; y++) {
            if (cupsRasterReadPixels(ras, line_buf, header.cupsBytesPerLine) == 0) {
                fprintf(stderr, "ERROR: rastertoql: short raster read at line %d\n", y);
                g_cancelled = 1;
                break;
            }
            uint8_t *row = page_buf + (size_t)y * src_w;
            for (int x = 0; x < src_w; x++)
                row[x] = (uint8_t)pixel_darkness(line_buf, (unsigned)x,
                                                 &header, black_is_high);
        }

        double s = scale;
        if (scale_fit) {
            s = (double)print_dots / src_w;
            if (is_diecut && (double)raster_lines / src_h < s)
                s = (double)raster_lines / src_h;
        }
        /* Continuous tape: the strip length follows the scaled page */
        if (!is_diecut) {
            raster_lines = (int)(src_h * s + 0.5);
            fprintf(stderr, "DEBUG: rastertoql: continuous, %d lines\n", raster_lines);
        }
        fprintf(stderr, "DEBUG: rastertoql: page %dx%d, scale %.3f -> %dx%d\n",
                src_w, src_h, s, print_dots, raster_lines);

        /* Per-page commands (line count is final now) */
        send_print_info(media, raster_lines, page_count == 1, quality);
        send_autocut(autocut);
        if (autocut) send_cut_every(1);
        send_expanded_mode(cut_at_end);
        send_margins(is_diecut ? 0 : QL_CONTINUOUS_MARGIN_DOTS);

        /* Output dot (ox, oy) samples the page about its centre */
        for (int oy = 0; oy < raster_lines && !g_cancelled; oy++) {
            int sy = (int)floor((oy + 0.5 - raster_lines / 2.0) / s + src_h / 2.0);
            for (int ox = 0; ox < print_dots; ox++) {
                int sx = (int)floor((ox + 0.5 - print_dots / 2.0) / s + src_w / 2.0);
                dark[ox] = (sy >= 0 && sy < src_h && sx >= 0 && sx < src_w)
                           ? tone[page_buf[(size_t)sy * src_w + sx]] : tone[0];
            }
            halftone_line(dark, dots, print_dots, oy, halftone, err_cur, err_next);
            int *t = err_cur; err_cur = err_next; err_next = t;
            memset(err_next, 0, sizeof(int) * ((size_t)print_dots + 2));

            int blank = 1;
            for (int ox = 0; ox < print_dots && blank; ox++) blank = !dots[ox];
            send_raster_line(blank ? NULL : dots, raster_bytes, print_dots, offset_r);
        }
        free(dots); free(dark); free(err_cur); free(err_next);

        fprintf(stderr, "INFO: Finished page %d\n", page_count);
    }

    /* Last page: print with feed (0x1A) */
    if (page_count > 0 && !g_cancelled)
        send_print(1);

    /* Cleanup */
    free(line_buf);
    free(page_buf);
    cupsRasterClose(ras);
    if (fd != 0) close(fd);
    cupsFreeOptions(num_options, options);
    fflush(stdout);

    if (g_cancelled) {
        fprintf(stderr, "ERROR: rastertoql: job cancelled\n");
        return 1;
    }

    if (page_count == 0) {
        fprintf(stderr, "ERROR: rastertoql: no pages in job\n");
        return 1;
    }

    fprintf(stderr, "INFO: rastertoql: %d page(s) sent to printer\n", page_count);
    return 0;
}
