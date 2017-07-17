#include "http_parser.h"
#include <stdlib.h>

// clang-format off
%%{
    machine http_parser;
    endl = '\r'? '\n';
    method = 'GET' %{ state->method = HTTP_GET; };
    uri_char = (any - '\r' - '\n' - ' ');
    uri = uri_char+ ${ g_string_append_c(state->uri, *p); };
    version = '1.0' %{ state->http_version = 10; }
            | '1.1' %{ state->http_version = 11; };
    query_line = method ' '+ uri ' '+ 'HTTP/' version endl;
    field_name_char = (any - '\r' - '\n' - ':');
    field_name = field_name_char+;
    field_value_char = (any - '\r' - '\n');
    field_value = field_value_char+;
    field = field_name ':' ' '* field_value endl %{};
    main := (query_line field* endl)
        @{
            state->header_parsed = 1;
            state->remainder = (char *)p + 1;
            state->remainder_sz = pe - (p + 1);
        }
        @err { state->error = 1; };
}%%

%% write data;
// clang-format on

struct hp_state *
hp_new(void)
{
    struct hp_state *state = calloc(sizeof(*state), 1);
    state->uri = g_string_new(NULL);

    // clang-format off
    %% variable cs state->parser_internal_state;
    %% write init;
    // clang-format on

    return state;
}

void
hp_free(struct hp_state *state)
{
    g_string_free(state->uri, TRUE);
    free(state);
}

void
hp_parse_chunk(struct hp_state *state, char *data, size_t sz)
{
    const char *p = data;
    const char *pe = data + sz;
    const char *eof = pe + 1;

    // clang-format off
    %% variable cs state->parser_internal_state;
    %% write exec;
    // clang-format on
}
