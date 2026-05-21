#ifndef FORM_PARSER_H
#define FORM_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief One field expected in a URL-encoded form body.
 *
 * `name`    — field name, e.g. "ssid"
 * `dst`     — destination buffer for the decoded value
 * `dst_len` — capacity of `dst` in bytes (must include room for NUL)
 */
typedef struct {
    const char *name;
    char *dst;
    size_t dst_len;
} form_field_t;

/**
 * @brief Extract a set of fields from a URL-encoded form body.
 *
 * For each field, locates `name=...` in `body`, copies the value into
 * `dst`, and decodes `+` to space. Returns false if any field is
 * missing or would overflow its destination buffer.
 *
 * NOTE: %XX percent-decoding is not implemented (current SoftAP form
 * does not need it). Add a TODO test before implementing.
 */
bool form_parser_extract(const char *body, const form_field_t *fields, size_t n_fields);

#endif
