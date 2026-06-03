/*
 * Minimal sd_reader stub for the probe build.
 * Real port: implement against 3DO FileSystem (OpenFile/ReadFile API).
 */
#ifndef SD_READER_H
#define SD_READER_H

#include <stdint.h>
#include <stddef.h>
#include "utils/path.h"

typedef struct sd_reader sd_reader;

sd_reader *sd_reader_open(const path *filename);
void sd_reader_close(sd_reader *reader);
long sd_reader_filesize(const sd_reader *reader);
int sd_reader_set(sd_reader *reader, long pos);
int sd_read_buf(sd_reader *reader, char *buf, size_t len);

uint8_t sd_read_ubyte(sd_reader *reader);
uint16_t sd_read_uword(sd_reader *reader);
uint32_t sd_read_udword(sd_reader *reader);

#endif
