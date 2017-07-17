#pragma once

#include <glib.h>
#include <stddef.h>

enum {
    HTTP_UNKNOWN = 0,
    HTTP_GET = 1,
};

struct hp_state {
    int http_version;
    int method;
    GString *uri;
    int header_parsed;
    int error;
    int parser_internal_state;
    char *remainder;
    size_t remainder_sz;
};

struct hp_state *
hp_new(void);

void
hp_free(struct hp_state *state);

void
hp_parse_chunk(struct hp_state *state, char *data, size_t sz);
