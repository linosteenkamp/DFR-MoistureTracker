#include "form_parser.h"
#include <string.h>

static void decode_plus_to_space(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '+') *p = ' ';
    }
}

static bool extract_one(const char *body, const form_field_t *f) {
    // Build "<name>=" search prefix on the stack (max ~32 bytes).
    char needle[40];
    size_t name_len = strlen(f->name);
    if (name_len + 2 > sizeof(needle)) return false;
    memcpy(needle, f->name, name_len);
    needle[name_len] = '=';
    needle[name_len + 1] = '\0';

    const char *start = strstr(body, needle);
    if (!start) return false;
    start += name_len + 1;

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= f->dst_len) return false;

    memcpy(f->dst, start, len);
    f->dst[len] = '\0';
    decode_plus_to_space(f->dst);
    return true;
}

bool form_parser_extract(const char *body, const form_field_t *fields, size_t n_fields) {
    if (!body || !fields) return false;
    for (size_t i = 0; i < n_fields; i++) {
        if (!extract_one(body, &fields[i])) return false;
    }
    return true;
}
