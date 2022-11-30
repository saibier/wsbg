#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include "json.h"

#define JSON_ERR_END "Unexpected end of document"
#define JSON_ERR_ESC "Invalid escape sequence"
#define JSON_ERR_KEY "Expected key/value separator"
#define JSON_ERR_VAL "Expected value"
#define JSON_ERR_CHR "Invalid character"
#define JSON_ERR_UTF8 "Invalid UTF-8 sequence"
#define JSON_ERR_UTF16 "Invalid UTF-16 sequence"


static bool json__err(struct json_state *s, const unsigned char *cur, const char *msg) {
	s->cur = cur;
	s->end = cur;
	s->err = msg;
	return false;
}

static void json__skip_whitespace(struct json_state *s) {
	while (s->cur != s->end && (
			*s->cur == 0x20 || *s->cur == 0x09 ||
			*s->cur == 0x0A || *s->cur == 0x0D)) {
		++s->cur;
	}
}

static int json__skip_post_value(struct json_state *s) {
	json__skip_whitespace(s);
	if (s->cur != s->end) {
		if (*s->cur == ',') {
			++s->cur;
			json__skip_whitespace(s);
			if (s->cur != s->end && (*s->cur == '}' || *s->cur == ']')) {
				return json__err(s, s->cur, JSON_ERR_VAL);
			}
		} else if (!(*s->cur == '}' || *s->cur == ']')) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
	}
	return true;
}

void json_init(struct json_state *s, const char *buffer, size_t size) {
	s->cur = (const unsigned char *)buffer;
	s->end = s->cur + size;
	s->err = NULL;
	// “In the interests of interoperability, implementations
	//  that parse JSON texts MAY ignore the presence of a
	//  byte order mark rather than treating it as an error.”
	//   — RFC 8259 § 8.1
	if (3 <= size &&
			s->cur[0] == 0xEF &&
			s->cur[1] == 0xBB &&
			s->cur[2] == 0xBF) {
		s->cur += 3;
	}
	json__skip_whitespace(s);
}

static bool json__begin(struct json_state *s, unsigned char bracket) {
	if (s->cur == s->end || *s->cur != bracket) {
		return false;
	}
	++s->cur;
	json__skip_whitespace(s);
	return true;
}

bool json_object(struct json_state *s) {
	return json__begin(s, '{');
}

bool json_list(struct json_state *s) {
	return json__begin(s, '[');
}

static bool json__end(struct json_state *s, unsigned char bracket) {
	if (s->cur == s->end) {
		if (!s->err) {
			json__err(s, s->end, JSON_ERR_END);
		}
		return true;
	} else if (*s->cur == bracket) {
		++s->cur;
		json__skip_post_value(s);
		return true;
	}
	return false;
}

bool json_end_object(struct json_state *s) {
	return json__end(s, '}');
}

bool json_end_list(struct json_state *s) {
	return json__end(s, ']');
}

static size_t json__unescape_unicode(struct json_state *s,
		const unsigned char **cur_ptr, unsigned char *buf) {
	const unsigned char *cur = *cur_ptr;
	uint_fast32_t code_point = 0;
	for (int offset = 12; 0 <= offset; offset -= 4) {
		if (++cur == s->end) {
			goto err_end;
		} else if ('0' <= *cur && *cur <= '9') {
			code_point |= (*cur - '0') << offset;
		} else if ('A' <= *cur && *cur <= 'F') {
			code_point |= (*cur - 'A' + 10) << offset;
		} else if ('a' <= *cur && *cur <= 'f') {
			code_point |= (*cur - 'a' + 10) << offset;
		} else {
			goto err_esc;
		}
	}
	if (0xD800 <= code_point) {
		if (code_point < 0xDC00) {
			if (++cur == s->end) {
				goto err_end;
			} else if (*cur != '\\') {
				goto err_utf16;
			} else if (++cur == s->end) {
				goto err_end;
			} else if (*cur != 'u') {
				goto err_utf16;
			} else if (++cur == s->end) {
				goto err_end;
			} else if (*cur != 'D' && *cur != 'd') {
				goto err_utf16;
			}
			code_point = (code_point - 0xD800 + 0x40) << 10;
			if (++cur == s->end) {
				goto err_end;
			} else if ('C' <= *cur && *cur <= 'F') {
				code_point |= (*cur - 'C') << 8;
			} else if ('c' <= *cur && *cur <= 'f') {
				code_point |= (*cur - 'c') << 8;
			} else {
				goto err_utf16;
			}
			for (int offset = 4; 0 <= offset; offset -= 4) {
				if (++cur == s->end) {
					goto err_end;
				} else if ('0' <= *cur && *cur <= '9') {
					code_point |= (*cur - '0') << offset;
				} else if ('A' <= *cur && *cur <= 'F') {
					code_point |= (*cur - 'A' + 10) << offset;
				} else if ('a' <= *cur && *cur <= 'f') {
					code_point |= (*cur - 'a' + 10) << offset;
				} else {
					goto err_esc;
				}
			}
		} else if (code_point < 0x0E00) {
			goto err_utf16;
		}
	}
	*cur_ptr = cur;
	if (code_point < 0x80) {
		buf[0] = code_point;
		return 1;
	} else if (code_point < 0x800) {
		buf[0] = 0xC0 | (code_point >> 6);
		buf[1] = 0x80 | (code_point & 0x3F);
		return 2;
	} else if (code_point < 0x10000) {
		buf[0] = 0xE0 | (code_point >> 12);
		buf[1] = 0x80 | ((code_point >> 6) & 0x3F);
		buf[2] = 0x80 | (code_point & 0x3F);
		return 3;
	} else {
		buf[0] = 0xF0 | (code_point >> 18);
		buf[1] = 0x80 | ((code_point >> 12) & 0x3F);
		buf[2] = 0x80 | ((code_point >> 6) & 0x3F);
		buf[3] = 0x80 | (code_point & 0x3F);
		return 4;
	}
err_esc:
	return json__err(s, cur, JSON_ERR_ESC);
err_end:
	return json__err(s, s->end, JSON_ERR_END);
err_utf16:
	return json__err(s, *cur_ptr, JSON_ERR_UTF16);
}

static bool json__skip_post_key(struct json_state *s) {
	json__skip_whitespace(s);
	if (s->cur == s->end) {
		return json__err(s, s->end, JSON_ERR_END);
	} else if (*s->cur != ':') {
		return json__err(s, s->cur, JSON_ERR_KEY);
	}
	++s->cur;
	json__skip_whitespace(s);
	return true;
}

static bool json__string(struct json_state *s, const char *string, bool is_key) {
	if (s->cur == s->end || *s->cur != '"') {
		return false;
	}
	const unsigned char *key = (const unsigned char *)string;
	const unsigned char *cur = s->cur + 1;
	while (cur != s->end) {
		if (*cur == '"') {
			if (*key == '\0') {
				s->cur = cur + 1;
				return is_key ?
					json__skip_post_key(s) :
					json__skip_post_value(s);;
			}
			return false;
		} else if (*key == '\0') {
			return false;
		} else if (*cur == '\\') {
			if (++cur == s->end) {
				return json__err(s, s->end, JSON_ERR_END);
			}
			switch (*cur) {
				case '"': if (*key != '"' ) return false; else break;
				case '\\':if (*key != '\\') return false; else break;
				case '/': if (*key != '/' ) return false; else break;
				case 'b': if (*key != 0x08) return false; else break;
				case 'f': if (*key != 0x0C) return false; else break;
				case 'n': if (*key != 0x0A) return false; else break;
				case 'r': if (*key != 0x0D) return false; else break;
				case 't': if (*key != 0x09) return false; else break;
				case 'u': {
					unsigned char buf[4];
					size_t buf_size = json__unescape_unicode(s, &cur, buf);
					if (buf_size == 0 || buf[0] == '\0') {
						return false;
					}
					for (size_t i = 0; i < buf_size; ++i) {
						if (*key != buf[i]) {
							return false;
						}
						++key;
					}
					continue;
				}
				default: return json__err(s, cur, JSON_ERR_ESC);
			}
			++cur;
			++key;
		} else if (*cur == *key) {
			++cur;
			++key;
		} else {
			return false;
		}
	}
	return json__err(s, s->end, JSON_ERR_END);
}

bool json_key(struct json_state *s, const char *key) {
	return json__string(s, key, true);
}

bool json_string(struct json_state *s, const char *string) {
	return json__string(s, string, false);
}

bool json_get_string(struct json_state *s, char *buffer, size_t *size,
		bool is_null_terminated) {
	if (s->cur == s->end || *s->cur != '"') {
		return false;
	}
	*size = 0;
	unsigned char *buf = (unsigned char *)buffer;
	while (++s->cur != s->end) {
		if (*s->cur == '"') {
			if (is_null_terminated) {
				buf[(*size)++] = '\0';
			}
			++s->cur;
			return json__skip_post_value(s);
		} else if (*s->cur == '\\') {
			if (++s->cur == s->end) {
				goto err_end;
			}
			switch (*s->cur) {
				case '"': buf[*size] = '"' ; break;
				case '\\':buf[*size] = '\\'; break;
				case '/': buf[*size] = '/' ; break;
				case 'b': buf[*size] = 0x08; break;
				case 'f': buf[*size] = 0x0C; break;
				case 'n': buf[*size] = 0x0A; break;
				case 'r': buf[*size] = 0x0D; break;
				case 't': buf[*size] = 0x09; break;
				case 'u': {
					size_t n = json__unescape_unicode(s, &s->cur, &buf[*size]);
					if (n == 0) {
						return false;
					} else if (is_null_terminated && buf[*size] == '\0') {
						return json__err(s, s->cur, JSON_ERR_CHR);
					}
					*size += n;
					continue;
				}
				default: return json__err(s, s->cur, JSON_ERR_ESC);
			}
			++*size;
		} else if (*s->cur < 0x20) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		} else if (*s->cur < 0x80) {
			buf[(*size)++] = *s->cur;
		} else {
			if (*s->cur < 0xC2) {
				goto err_utf8;
			} else if (*s->cur < 0xE0) {
				goto final_byte;
			} else if (0xF4 < *s->cur) {
				goto err_utf8;
			}
			unsigned char lead = *s->cur;
			buf[(*size)++] = *s->cur;
			if (++s->cur == s->end) {
				goto err_end;
			} else if (*s->cur < 0x80 || 0xBF < *s->cur ||
					(lead == 0xE0 && *s->cur < 0xA0) ||
					(lead == 0xED && 0x9F < *s->cur) ||
					(lead == 0xF0 && *s->cur < 0x90) ||
					(lead == 0xF4 && 0x8F < *s->cur)) {
				goto err_utf8;
			} else if (lead < 0xF0) {
				goto final_byte;
			}
			buf[(*size)++] = *s->cur;
			if (++s->cur == s->end) {
				goto err_end;
			} else if (*s->cur < 0x80 || 0xBF < *s->cur) {
				goto err_utf8;
			}
		final_byte:
			buf[(*size)++] = *s->cur;
			if (++s->cur == s->end) {
				goto err_end;
			} else if (*s->cur < 0x80 || 0xBF < *s->cur) {
				goto err_utf8;
			}
			buf[(*size)++] = *s->cur;
		}
	}
err_end:
	return json__err(s, s->end, JSON_ERR_END);
err_utf8:
	return json__err(s, s->cur, JSON_ERR_UTF8);
}

static bool json__match(struct json_state *s, const char *token) {
	const unsigned char *cur = s->cur;
	const unsigned char *tok = (const unsigned char *)token;
	if (cur == s->end || *cur != *tok) {
		return false;
	}
	while (*++tok != '\0') {
		if (++cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		} else if (*cur != *tok) {
			return json__err(s, cur, JSON_ERR_CHR);
		}
	}
	s->cur = ++cur;
	return json__skip_post_value(s);
}

bool json_true(struct json_state *s) {
	return json__match(s, "true");
}

bool json_false(struct json_state *s) {
	return json__match(s, "false");
}

bool json_null(struct json_state *s) {
	return json__match(s, "null");
}

static bool json__skip_string(struct json_state *s, bool is_key) {
	if (s->cur == s->end || *s->cur != '"') {
		return false;
	}
	while (++s->cur != s->end) {
		if (*s->cur == '"') {
			++s->cur;
			return is_key ?
				json__skip_post_key(s) :
				json__skip_post_value(s);
		} else if (*s->cur == '\\') {
			if (++s->cur == s->end) {
				return json__err(s, s->end, JSON_ERR_END);
			}
			switch (*s->cur) {
				case '"':
				case '\\':
				case '/':
				case 'b':
				case 'f':
				case 'n':
				case 'r':
				case 't': break;
				case 'u': {
					for (unsigned i = 0; i < 4; ++i) {
						if (++s->cur == s->end) {
							return json__err(s, s->end, JSON_ERR_END);
						} else if (!(
								('0' <= *s->cur && *s->cur <= '9') ||
								('A' <= *s->cur && *s->cur <= 'F') ||
								('a' <= *s->cur && *s->cur <= 'f'))) {
							return json__err(s, s->cur, JSON_ERR_ESC);
						}
					}
					break;
				}
				default: return json__err(s, s->cur, JSON_ERR_ESC);
			}
		} else if (*s->cur < 0x20) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
	}
	return json__err(s, s->end, JSON_ERR_END);
}

static bool json__skip_number(struct json_state *s) {
	if (s->cur == s->end || !(*s->cur == '-' ||
			('0' <= *s->cur && *s->cur <= '9'))) {
		return false;
	}
	if (*s->cur == '-') {
		if (++s->cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		} else if (!('0' <= *s->cur && *s->cur <= '9')) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
	}
	if (*s->cur == '0') {
		if (++s->cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		}
	} else {
		do {
			if (++s->cur == s->end) {
				return json__err(s, s->end, JSON_ERR_END);
			}
		} while ('0' <= *s->cur && *s->cur <= '9');
	}
	if (*s->cur == '.') {
		if (++s->cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		} else if (!('0' <= *s->cur && *s->cur <= '9')) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
		do {
			if (++s->cur == s->end) {
				return json__err(s, s->end, JSON_ERR_END);
			}
		} while ('0' <= *s->cur && *s->cur <= '9');
	}
	if (*s->cur == 'e' || *s->cur == 'E') {
		if (++s->cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		} else if (!(*s->cur == '-' || *s->cur == '+')) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
		if (++s->cur == s->end) {
			return json__err(s, s->end, JSON_ERR_END);
		} else if (!('0' <= *s->cur && *s->cur <= '9')) {
			return json__err(s, s->cur, JSON_ERR_CHR);
		}
		do {
			if (++s->cur == s->end) {
				return json__err(s, s->end, JSON_ERR_END);
			}
		} while ('0' <= *s->cur && *s->cur <= '9');
	}
	return json__skip_post_value(s);
}

static bool json__err_(struct json_state *s) {
	if (s->err) {
		return false;
	} else if (s->cur == s->end) {
		return json__err(s, s->end, JSON_ERR_END);
	} else {
		return json__err(s, s->cur, JSON_ERR_CHR);
	}
}

bool json_skip_key(struct json_state *s) {
	return json__skip_string(s, true) || json__err_(s);
}

bool json_skip_key_value_pair(struct json_state *s) {
	return json_skip_key(s) && json_skip_value(s);
}

static bool json__skip_object(struct json_state *s) {
	if (!json_object(s)) {
		return false;
	}
	while (s->cur != s->end) {
		if (*s->cur == '}') {
			++s->cur;
			return json__skip_post_value(s);
		} else if (!json_skip_key_value_pair(s)) {
			return false;
		}
	}
	return json__err(s, s->end, JSON_ERR_END);
}

static bool json__skip_list(struct json_state *s) {
	if (!json_list(s)) {
		return false;
	}
	while (s->cur != s->end) {
		if (*s->cur == ']') {
			++s->cur;
			return json__skip_post_value(s);
		} else if (!json_skip_value(s)) {
			return false;
		}
	}
	return json__err(s, s->end, JSON_ERR_END);
}

bool json_skip_value(struct json_state *s) {
	return (
		json__skip_object(s) ||
		json__skip_list(s) ||
		json__skip_string(s, false) ||
		json__skip_number(s) ||
		json_true(s) ||
		json_false(s) ||
		json_null(s) ||
		json__err_(s));
}

