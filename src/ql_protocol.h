/*
 * ql_protocol.h - Brother QL series raster protocol definitions
 *
 * Supports: QL-1100 (102mm), QL-800 (62mm)
 *
 * Protocol reference: Brother QL series raster command reference
 */

#ifndef QL_PROTOCOL_H
#define QL_PROTOCOL_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Printer model identifiers
 * --------------------------------------------------------------------- */
typedef enum {
    QL_MODEL_UNKNOWN = 0,
    QL_MODEL_800,       /* QL-800: 62mm max, 300dpi, 696 dots wide */
    QL_MODEL_1100,      /* QL-1100: 102mm max, 300dpi, 1164 dots wide */
} ql_model_t;

/* -----------------------------------------------------------------------
 * Media type codes (sent in print information command)
 * --------------------------------------------------------------------- */
#define QL_MEDIA_CONTINUOUS     0x0A    /* Continuous length tape */
#define QL_MEDIA_DIECUT         0x0B    /* Die-cut labels */

/* -----------------------------------------------------------------------
 * Print channel / quality flags
 * --------------------------------------------------------------------- */
#define QL_QUALITY_HIGH         0x01
#define QL_QUALITY_FAST         0x00

/* -----------------------------------------------------------------------
 * Command bytes
 * --------------------------------------------------------------------- */

/* Invalidate: 200 x 0x00 — clears any stuck job */
#define QL_INVALIDATE_COUNT     200
#define QL_INVALIDATE_BYTE      0x00

/* Initialize */
#define QL_CMD_INIT_0           0x1B
#define QL_CMD_INIT_1           0x40

/* Switch to raster mode: ESC i a 0x01 */
#define QL_CMD_RASTER_0         0x1B
#define QL_CMD_RASTER_1         0x69
#define QL_CMD_RASTER_2         0x61
#define QL_CMD_RASTER_MODE      0x01

/* Print information command: ESC i z ... (11 bytes total after prefix) */
#define QL_CMD_INFO_0           0x1B
#define QL_CMD_INFO_1           0x69
#define QL_CMD_INFO_2           0x7A

/* Various mode commands */
#define QL_CMD_AUTOCUT_0        0x1B
#define QL_CMD_AUTOCUT_1        0x69
#define QL_CMD_AUTOCUT_2        0x4D    /* 'M' */

#define QL_CMD_CUTEVERY_0       0x1B
#define QL_CMD_CUTEVERY_1       0x69
#define QL_CMD_CUTEVERY_2       0x41   /* 'A' */

/* Expanded mode: ESC i K */
#define QL_CMD_EXPMODE_0        0x1B
#define QL_CMD_EXPMODE_1        0x69
#define QL_CMD_EXPMODE_2        0x4B   /* 'K' */

/* Margins (feed amount): ESC i d <lo> <hi> */
#define QL_CMD_MARGINS_0        0x1B
#define QL_CMD_MARGINS_1        0x69
#define QL_CMD_MARGINS_2        0x64   /* 'd' */

/* Raster line: 0x67 0x00 <n_bytes> <data...> */
#define QL_RASTER_CMD_0         0x67
#define QL_RASTER_CMD_1         0x00

/* Print (with feed): 0x1A */
#define QL_CMD_PRINT_FEED       0x1A

/* Print (no feed / more pages follow): 0x0C */
#define QL_CMD_PRINT_NOFEED     0x0C

/* -----------------------------------------------------------------------
 * Printer geometry
 *
 * raster_bytes is the fixed length of every raster line sent to the head.
 * additional_offset_r is extra offset the wide (102mm) machine needs on top
 * of the per-label offset.  Larger values move the print to the right.
 * QL-1100: brother_ql uses 44; tested on a real QL-1100 the first character
 * was clipped on the left; centred on DK-11241 at +24 dots (2mm) more.
 * --------------------------------------------------------------------- */
typedef struct {
    const char  *model_name;
    ql_model_t   model;
    int          print_dots_wide;   /* printable dots across widest tape */
    int          raster_bytes;      /* bytes per raster line sent to printer */
    int          dpi;
    int          additional_offset_r;
} ql_printer_info_t;

static const ql_printer_info_t QL_PRINTERS[] = {
    /* QL-800: 720-dot head => 90 bytes per raster line */
    { "QL-800",  QL_MODEL_800,   696,  90, 300,  0 },
    /* QL-1100: 1296-dot head => 162 bytes per raster line */
    { "QL-1100", QL_MODEL_1100, 1200, 162, 300, 68 },
    { NULL, QL_MODEL_UNKNOWN, 0, 0, 0, 0 }
};

/* -----------------------------------------------------------------------
 * Media table: maps PPD MediaType string → protocol params
 *
 * label_width_mm / label_length_mm are what the printer compares against
 * the loaded DK roll — they must match exactly or the printer rejects the
 * job (flashing red LED).  print_dots_* are the printable area at 300dpi.
 * --------------------------------------------------------------------- */
typedef struct {
    const char *ppd_name;       /* Matches PPD *MediaType value */
    uint8_t     media_type;     /* QL_MEDIA_* */
    int         label_width_mm;
    int         label_length_mm; /* 0 = continuous */
    int         print_dots_wide; /* printable dots across the label */
    int         print_dots_long; /* printable lines (0 = continuous) */
    int         offset_r;        /* right-hand offset in dots */
} ql_media_info_t;

/* QL-1100 media */
static const ql_media_info_t QL1100_MEDIA[] = {
    /* DK-11241: 102mm x 152mm die-cut (printer reports length as 153) */
    { "DK11241",    QL_MEDIA_DIECUT,      102, 153, 1164, 1660, 12 },
    /* DK-11209: 62mm x 29mm die-cut (address) */
    { "DK11209",    QL_MEDIA_DIECUT,       62,  29,  696,  271, 12 },
    /* DK-11202: 62mm x 100mm die-cut (shipping) */
    { "DK11202",    QL_MEDIA_DIECUT,       62, 100,  696, 1109, 12 },
    /* DK-22205: 62mm continuous */
    { "DK22205",    QL_MEDIA_CONTINUOUS,   62,   0,  696,    0, 12 },
    /* DK-22246: 103.6mm continuous (printer reports width as 104) */
    { "DK22246",    QL_MEDIA_CONTINUOUS,  104,   0, 1200,    0, 12 },
    { NULL, 0, 0, 0, 0, 0, 0 }
};

/* QL-800 media */
static const ql_media_info_t QL800_MEDIA[] = {
    /* DK-22206: 62mm continuous white paper */
    { "DK22206",    QL_MEDIA_CONTINUOUS,   62,   0,  696,    0, 12 },
    /* DK-22205: 62mm continuous */
    { "DK22205",    QL_MEDIA_CONTINUOUS,   62,   0,  696,    0, 12 },
    /* DK-11209: 62mm x 29mm die-cut */
    { "DK11209",    QL_MEDIA_DIECUT,       62,  29,  696,  271, 12 },
    /* DK-11202: 62mm x 100mm die-cut */
    { "DK11202",    QL_MEDIA_DIECUT,       62, 100,  696, 1109, 12 },
    { NULL, 0, 0, 0, 0, 0, 0 }
};

/* -----------------------------------------------------------------------
 * Print information command: ESC i z followed by 10 bytes
 *   n1      valid flags
 *   n2      media type
 *   n3      width mm
 *   n4      length mm (0 for continuous)
 *   n5..n8  raster line count, little-endian
 *   n9      0 = first page, 1 = subsequent page
 *   n10     0x00
 * --------------------------------------------------------------------- */
#define QL_PRINT_INFO_LEN           10

/* valid_flags bits */
#define QL_INFO_VALID_MEDIA_TYPE    0x02
#define QL_INFO_VALID_WIDTH         0x04
#define QL_INFO_VALID_LENGTH        0x08
#define QL_INFO_VALID_QUALITY       0x40
#define QL_INFO_VALID_RECOVER       0x80
#define QL_INFO_VALID_ALL           (QL_INFO_VALID_MEDIA_TYPE | \
                                     QL_INFO_VALID_WIDTH | \
                                     QL_INFO_VALID_LENGTH | \
                                     QL_INFO_VALID_QUALITY | \
                                     QL_INFO_VALID_RECOVER)

/* Feed margin for continuous tape, in dots (die-cut uses 0) */
#define QL_CONTINUOUS_MARGIN_DOTS   35

/* -----------------------------------------------------------------------
 * Autocut / mode flag bits
 * --------------------------------------------------------------------- */
#define QL_MODE_AUTOCUT     0x40
#define QL_MODE_MIRROR      0x80

/* Expanded mode bits */
#define QL_EXPMODE_HIGHRES  0x40    /* 300x600dpi — doubles the line count; not used */
#define QL_EXPMODE_CUTEND   0x08    /* cut at end of job */

#endif /* QL_PROTOCOL_H */
