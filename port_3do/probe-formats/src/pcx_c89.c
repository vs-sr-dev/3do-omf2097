/*
 * pcx.c converted to C89-strict style for armcc.
 * Only changes: declarations hoisted to block tops; for-loop var declarations
 * split into separate `int i; for (i = ...)`. No logic touched.
 */
#include "formats/pcx.h"
#include "formats/error.h"
#include "formats/internal/reader.h"
#include <string.h>

static unsigned decode_next_bytes(char *dest, sd_reader *reader) {
    uint8_t fst_byte;
    uint8_t repeat;
    uint8_t snd_byte;
    unsigned i;

    fst_byte = sd_read_ubyte(reader);
    repeat = 0;
    if (fst_byte >= 192) {
        repeat = fst_byte - 192;
    }
    if (repeat == 0) {
        dest[0] = fst_byte;
        return 1;
    }
    snd_byte = sd_read_ubyte(reader);
    for (i = 0; i < repeat; ++i) {
        dest[i] = snd_byte;
    }
    return repeat;
}

int pcx_load(pcx_file *pcx, const path *filename) {
    sd_reader *reader;
    long filesize;
    int ret;
    unsigned i, j;
    char foo;

    memset(pcx, 0, sizeof(*pcx));

    reader = sd_reader_open(filename);
    if (reader == NULL) {
        return SD_FILE_INVALID_TYPE;
    }

    filesize = sd_reader_filesize(reader);
    if (filesize <= 128) {
        sd_reader_close(reader);
        return SD_FILE_INVALID_TYPE;
    }

    pcx->manufacturer    = sd_read_ubyte(reader);
    pcx->version         = sd_read_ubyte(reader);
    pcx->encoding        = sd_read_ubyte(reader);
    pcx->bits_per_plane  = sd_read_ubyte(reader);

    pcx->window_x_min = sd_read_uword(reader);
    pcx->window_y_min = sd_read_uword(reader);
    pcx->window_x_max = sd_read_uword(reader);
    pcx->window_y_max = sd_read_uword(reader);

    pcx->horz_dpi = sd_read_uword(reader);
    pcx->vert_dpi = sd_read_uword(reader);

    if (sd_read_buf(reader, (char *)pcx->header_palette, 48) != 1) {
        sd_reader_close(reader);
        return SD_FILE_READ_ERROR;
    }
    pcx->reserved     = sd_read_ubyte(reader);
    pcx->color_planes = sd_read_ubyte(reader);

    pcx->bytes_per_plane_line = sd_read_uword(reader);
    pcx->palette_info         = sd_read_uword(reader);
    pcx->hor_scr_size         = sd_read_uword(reader);
    pcx->ver_scr_size         = sd_read_uword(reader);

    if (sd_reader_set(reader, 128) != 1) {
        sd_reader_close(reader);
        return SD_FILE_INVALID_TYPE;
    }

    ret = sd_vga_image_create(&(pcx->image), 320, 200);
    if (ret != SD_SUCCESS) {
        return ret;
    }

    for (j = 0; j < 200; ++j) {
        for (i = 0; i < 320;) {
            i += decode_next_bytes(&(pcx->image.data)[(320 * j) + i], reader);
        }
    }

    foo = sd_read_ubyte(reader);
    if (foo != 0x0c) {
        return SD_FILE_READ_ERROR;
    }

    if (sd_read_buf(reader, (char *)&(pcx->palette.colors),
                    256 * sizeof(vga_color)) != 1) {
        sd_reader_close(reader);
        return SD_FILE_READ_ERROR;
    }

    sd_reader_close(reader);
    return SD_SUCCESS;
}

int pcx_load_font(pcx_font *font, const path *filename) {
    int ret;
    char corner_color;
    int i, j;
    int glyph;
    int width;

    memset(font, 0, sizeof(pcx_font));
    ret = pcx_load(&font->pcx, filename);
    if (ret != SD_SUCCESS) {
        return ret;
    }

    corner_color = font->pcx.image.data[0];

    /* check this is actually a font sheet */
    if (font->pcx.image.data[1]   != corner_color ||
        font->pcx.image.data[320] != corner_color ||
        font->pcx.image.data[321] == corner_color) {
        return SD_FORMAT_NOT_SUPPORTED;
    }

    /* figure out the height of the font */
    font->glyph_height = 0;
    for (i = 1; i < 200; i++) {
        if (font->pcx.image.data[(320 * i) + 1] == corner_color) {
            break;
        }
        font->glyph_height++;
    }

    glyph = 0;
    /* first row never has font pixel — start from second */
    for (i = 1;
         i < 200 && font->pcx.image.data[(320 * (i + font->glyph_height)) + 1] == corner_color;
         i += font->glyph_height + 1) {
        width = 0;
        for (j = 1; j < 320; j++) {
            if (font->pcx.image.data[(320 * i) + j] == corner_color) {
                font->glyphs[glyph].width = width;
                font->glyphs[glyph].x     = j - width;
                font->glyphs[glyph].y     = i;
                glyph++;
                width = 0;
            } else {
                width++;
            }
        }
    }

    font->glyph_count = glyph;
    return SD_SUCCESS;
}

int pcx_font_decode(const pcx_font *font, sd_vga_image *o, uint8_t ch, int8_t palette_offset) {
    int i, j, k, l;

    if (ch >= font->glyph_count || font == NULL || o == NULL) {
        return SD_INVALID_INPUT;
    }

    k = 0;
    for (i = font->glyphs[ch].y; i < font->glyphs[ch].y + font->glyph_height; i++) {
        l = 0;
        for (j = font->glyphs[ch].x; j < font->glyphs[ch].x + font->glyphs[ch].width; j++) {
            if (font->pcx.image.data[(i * 320) + j]) {
                o->data[(k * font->glyphs[ch].width) + l] =
                    palette_offset + (int)font->pcx.image.data[(i * 320) + j];
            } else {
                o->data[(k * font->glyphs[ch].width) + l] = 0;
            }
            l++;
        }
        k++;
    }
    return SD_SUCCESS;
}

void pcx_free(pcx_file *pcx) {
    if (pcx == NULL) {
        return;
    }
    sd_vga_image_free(&pcx->image);
}

void pcx_font_free(pcx_font *font) {
    if (font == NULL) {
        return;
    }
    sd_vga_image_free(&font->pcx.image);
}
