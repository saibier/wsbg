#ifndef _WSBG_JSON_H
#define _WSBG_JSON_H

#include <stdbool.h>
#include <stddef.h>

/**
 * JSON parsing state including the current buffer location,
 * the end of the buffer, and an error description.
 */
struct json_state {
	const unsigned char *cur, *end;
	const char *err;
};

/**
 * Initializes JSON state.
 */
void json_init(struct json_state *s, const char *buffer, size_t size);
/**
 * Matches the next JSON token.
 * When a token is matched, the position is moved to the next token.
 * Returns false when the token isn't a match, or there is invalid JSON.
 */
bool json_object(struct json_state *s);
bool json_end_object(struct json_state *s);
bool json_list(struct json_state *s);
bool json_end_list(struct json_state *s);
bool json_key(struct json_state *s, const char *key);
bool json_skip_key(struct json_state *s);
bool json_skip_value(struct json_state *s);
bool json_skip_key_value_pair(struct json_state *s);
bool json_string(struct json_state *s, const char *string);
bool json_get_string(struct json_state *s, char *buffer, size_t *size,
		bool is_null_terminated);
bool json_true(struct json_state *s);
bool json_false(struct json_state *s);
bool json_null(struct json_state *s);

#endif
