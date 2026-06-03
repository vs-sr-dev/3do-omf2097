/*
 * dump_lang.c -- host-side extractor for OMF language files
 * (ENGLISH.DAT / GERMAN.DAT / ...).
 *
 * Subcommands:
 *
 *   info <FILE.DAT>
 *       Prints total string count + sample entries (head, insult ranges,
 *       arena names + descs) -- sanity check that the parser works.
 *
 *   dump-range <FILE.DAT> <start> <count>
 *       Prints `count` consecutive strings starting at `start`, one per
 *       line, with leading index. Useful for visual diff vs. DOS strings.
 *
 *   gen-insults <FILE.DAT> <out.h>
 *       Emits a C header with two arrays of 121 strings each, covering
 *       lang_get(749..869) (insult slot 0) and lang_get(870..990) (slot 1).
 *       Indexing in the runtime: insult[0][11*p1 + p2], insult[1][11*p2 + p1].
 *
 *   gen-arenas <FILE.DAT> <out.h>
 *       Emits a C header with two arrays of 5 strings each: arena names
 *       (lang_get(56..60)) and arena descriptions (lang_get(66..70)).
 *
 * Links against the same openomf-master/src subset as the other host tools
 * (see Makefile), plus formats/language.c + formats/internal/memreader.c
 * which are new dependencies. See [[project-host-tools-strategy]].
 */

#include "formats/error.h"
#include "formats/language.h"
#include "utils/path.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSULT_P1_BASE   749
#define INSULT_P1_COUNT  121
#define INSULT_P2_BASE   870
#define INSULT_P2_COUNT  121

#define ARENA_NAME_BASE   56
#define ARENA_NAME_COUNT   5
#define ARENA_DESC_BASE   66
#define ARENA_DESC_COUNT   5

static int
load_dat(const char *fname, sd_language *out)
{
    path p;
    path_from_c(&p, fname);
    sd_language_create(out);
    int rc = sd_language_load(out, &p);
    if (rc != SD_SUCCESS) {
        fprintf(stderr, "sd_language_load(%s) -> %d\n", fname, rc);
        return -1;
    }
    return 0;
}

/* Escape a string for embedding in a C source literal: backslash, quote,
 * newline, CR, tab, plus anything outside printable ASCII. Returns the
 * number of bytes written into `dst` (excluding trailing NUL). */
static size_t
c_escape(const char *src, char *dst, size_t dst_max)
{
    size_t di = 0;
    if (src == NULL) src = "";
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (di + 5 >= dst_max) break;     /* room for \xNN + NUL */
        unsigned c = *p;
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c == '\n') {
            dst[di++] = '\\'; dst[di++] = 'n';
        } else if (c == '\r') {
            dst[di++] = '\\'; dst[di++] = 'r';
        } else if (c == '\t') {
            dst[di++] = '\\'; dst[di++] = 't';
        } else if (c >= 0x20 && c < 0x7f) {
            dst[di++] = (char)c;
        } else {
            di += snprintf(dst + di, dst_max - di, "\\x%02x", c);
        }
    }
    dst[di] = 0;
    return di;
}

static int
cmd_info(const char *fname)
{
    sd_language lang;
    if (load_dat(fname, &lang) < 0) return 1;

    printf("file:          %s\n", fname);
    printf("string count:  %u\n", lang.count);
    printf("\nSample head (0..2):\n");
    for (unsigned i = 0; i < 3 && i < lang.count; i++) {
        const sd_lang_string *s = sd_language_get(&lang, i);
        printf("  [%u] desc=\"%s\" len=%zu\n", i, s->description,
               s->data ? strlen(s->data) : 0);
    }

    printf("\nArena names (%u..%u):\n",
           ARENA_NAME_BASE, ARENA_NAME_BASE + ARENA_NAME_COUNT - 1);
    for (unsigned i = 0; i < ARENA_NAME_COUNT; i++) {
        const sd_lang_string *s = sd_language_get(&lang, ARENA_NAME_BASE + i);
        if (s == NULL) { printf("  [%u] (missing)\n", ARENA_NAME_BASE + i); continue; }
        printf("  [%u] \"%s\"\n", ARENA_NAME_BASE + i, s->data ? s->data : "");
    }

    printf("\nFirst 5 insults slot-0 (%u..%u):\n",
           INSULT_P1_BASE, INSULT_P1_BASE + 4);
    for (unsigned i = 0; i < 5; i++) {
        const sd_lang_string *s = sd_language_get(&lang, INSULT_P1_BASE + i);
        if (s == NULL) { printf("  [%u] (missing)\n", INSULT_P1_BASE + i); continue; }
        printf("  [%u] \"%s\"\n", INSULT_P1_BASE + i, s->data ? s->data : "");
    }

    sd_language_free(&lang);
    return 0;
}

static int
cmd_dump_range(const char *fname, unsigned start, unsigned count)
{
    sd_language lang;
    if (load_dat(fname, &lang) < 0) return 1;

    for (unsigned i = 0; i < count; i++) {
        const sd_lang_string *s = sd_language_get(&lang, start + i);
        if (s == NULL) {
            printf("[%u] (missing)\n", start + i);
            continue;
        }
        printf("[%u] %s\n", start + i, s->data ? s->data : "");
    }
    sd_language_free(&lang);
    return 0;
}

static int
emit_string_array(FILE *fp, sd_language *lang,
                  const char *array_name,
                  unsigned base, unsigned count,
                  const char *trailing_comment)
{
    char buf[1024];
    fprintf(fp, "/* lang_get(%u..%u) -- %s */\n",
            base, base + count - 1, trailing_comment);
    fprintf(fp, "static const char *const %s[%u] = {\n", array_name, count);
    for (unsigned i = 0; i < count; i++) {
        const sd_lang_string *s = sd_language_get(lang, base + i);
        const char *data = (s && s->data) ? s->data : "";
        c_escape(data, buf, sizeof(buf));
        fprintf(fp, "    /* [%u] */ \"%s\",\n", base + i, buf);
    }
    fprintf(fp, "};\n\n");
    return 0;
}

static int
cmd_gen_insults(const char *fname, const char *out_h)
{
    sd_language lang;
    if (load_dat(fname, &lang) < 0) return 1;

    FILE *fp = fopen(out_h, "w");
    if (fp == NULL) {
        fprintf(stderr, "fopen(%s): %s\n", out_h, strerror(errno));
        sd_language_free(&lang);
        return 1;
    }

    fprintf(fp, "/* Auto-generated by host_tools/dump_lang gen-insults"
                " -- do NOT edit. */\n");
    fprintf(fp, "/* source: %s */\n", fname);
    fprintf(fp, "#ifndef INSULTS_DATA_H\n#define INSULTS_DATA_H\n\n");

    fprintf(fp, "/* Canon formula (openomf vs.c L686-690):\n"
                " *   insult[0] = lang_get(%u + 11*p1 + p2)\n"
                " *   insult[1] = lang_get(%u + 11*p2 + p1)\n"
                " * with p1, p2 in 0..10 (10 = KREISSACK end-boss).\n"
                " *\n"
                " * We index into the arrays below with (11*p1 + p2)\n"
                " * and (11*p2 + p1) respectively. */\n\n",
            INSULT_P1_BASE, INSULT_P2_BASE);

    emit_string_array(fp, &lang, "lang_insult_p1",
                      INSULT_P1_BASE, INSULT_P1_COUNT,
                      "insult slot 0 (top text), 11x11 = 121 entries");
    emit_string_array(fp, &lang, "lang_insult_p2",
                      INSULT_P2_BASE, INSULT_P2_COUNT,
                      "insult slot 1 (bottom text), 11x11 = 121 entries");

    fprintf(fp, "#define LANG_INSULT_P1_COUNT  %u\n", INSULT_P1_COUNT);
    fprintf(fp, "#define LANG_INSULT_P2_COUNT  %u\n", INSULT_P2_COUNT);
    fprintf(fp, "#define LANG_INSULT_STRIDE    11\n\n");

    fprintf(fp, "#endif /* INSULTS_DATA_H */\n");
    fclose(fp);
    sd_language_free(&lang);
    return 0;
}

static int
cmd_gen_arenas(const char *fname, const char *out_h)
{
    sd_language lang;
    if (load_dat(fname, &lang) < 0) return 1;

    FILE *fp = fopen(out_h, "w");
    if (fp == NULL) {
        fprintf(stderr, "fopen(%s): %s\n", out_h, strerror(errno));
        sd_language_free(&lang);
        return 1;
    }

    fprintf(fp, "/* Auto-generated by host_tools/dump_lang gen-arenas"
                " -- do NOT edit. */\n");
    fprintf(fp, "/* source: %s */\n", fname);
    fprintf(fp, "#ifndef ARENAS_LANG_DATA_H\n#define ARENAS_LANG_DATA_H\n\n");

    emit_string_array(fp, &lang, "lang_arena_name",
                      ARENA_NAME_BASE, ARENA_NAME_COUNT, "arena names");
    emit_string_array(fp, &lang, "lang_arena_desc",
                      ARENA_DESC_BASE, ARENA_DESC_COUNT, "arena descriptions");

    fprintf(fp, "#define LANG_ARENA_COUNT  %u\n\n", ARENA_NAME_COUNT);
    fprintf(fp, "#endif /* ARENAS_LANG_DATA_H */\n");
    fclose(fp);
    sd_language_free(&lang);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  %s info        <FILE.DAT>\n"
            "  %s dump-range  <FILE.DAT> <start> <count>\n"
            "  %s gen-insults <FILE.DAT> <out.h>\n"
            "  %s gen-arenas  <FILE.DAT> <out.h>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "info") == 0) {
        if (argc != 3) goto usage;
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "dump-range") == 0) {
        if (argc != 5) goto usage;
        return cmd_dump_range(argv[2], (unsigned)strtoul(argv[3], NULL, 10),
                              (unsigned)strtoul(argv[4], NULL, 10));
    }
    if (strcmp(cmd, "gen-insults") == 0) {
        if (argc != 4) goto usage;
        return cmd_gen_insults(argv[2], argv[3]);
    }
    if (strcmp(cmd, "gen-arenas") == 0) {
        if (argc != 4) goto usage;
        return cmd_gen_arenas(argv[2], argv[3]);
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
usage:
    return 1;
}
