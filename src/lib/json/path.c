/*
 * Copyright 2010-2016 Tarantool AUTHORS: please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "path.h"
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>

/** State of a parser. */
enum json_parser_state {
	/**
	 * A current position is outside of <''>, <"">, <[]> and
	 * <.> .
	 */
	TOP_LEVEL,
	/** A current position is inside <[]>. */
	BRACKETS,
	/** A current position is inside <''> or <"">. */
	STRING,
	/*
	 * A current position is inside <.>. For example:
	 * .field1.field2.field3 ...
	 *    ^      ^      ^
	 *  FIELD  FIELD  FIELD
	 */
	FIELD
};

/** Same as strtoi(), but with limited length. */
static inline int
strntoi(const char *src, int len) {
	int value = 0;
	for (const char *end = src + len; src < end; ++src) {
		assert(isdigit(*src));
		value = value * 10 + *src - (int)'0';
	}
	return value;
}

int
json_next(struct json_path_parser *parser)
{
	enum json_parser_state state = TOP_LEVEL;
	char quote_type;
	const char *src = parser->src;
	const char *end = src + parser->src_len;
	const char *pos = parser->pos;
	bool is_index = false;
	const char *key = pos;
	int key_len = 0;
	for (char c = *pos; pos < end; c = *++pos) {
		bool is_quote = c == '\'' || c == '"';
		if (state == STRING) {
			if (is_quote && quote_type == c) {
				/* End of string. */
				state = BRACKETS;
				key_len = pos - key;
			} /* Else inside string. */
		} else if (is_quote) {
			/*
			 * String can by only inside []. Can not
			 * write <'field'.'field2'>.
			 */
			if (state != BRACKETS)
				return pos - src + 1;
			is_index = false;
			state = STRING;
			quote_type = c;
			key = pos + 1;
		} else if (c == '.') {
			if (state == TOP_LEVEL) {
				if (pos > parser->pos) {
					/*
					 * Example: <field.field>.
					 */
					key = parser->pos;
					break;
				} else {
					/* Example: <.field>. */
					key = pos + 1;
					state = FIELD;
				}
			} else if (state == FIELD) {
				/* Example: <.field.field>. */
				key_len = pos - key;
				break;
			} else {
				return pos - src + 1;
			}
		} else if (c == '[') {
			if (state != TOP_LEVEL && state != FIELD)
				return pos - src + 1;
			if (pos > parser->pos) {
				/*
				 * Example: <field[10]> or
				 * <.field[10]>.
				 */
				break;
			}
			is_index = true;
			state = BRACKETS;
			key = pos + 1;
		} else if (c == ']') {
			if (state != BRACKETS || key_len == 0)
				return pos - src + 1;
			/* Skip ']'. */
			++pos;
			state = TOP_LEVEL;
			break;
		} else {
			assert(state == BRACKETS || state == FIELD ||
			       state == TOP_LEVEL);
			if (state == BRACKETS) {
				if (!isdigit(c))
					return pos - src + 1;
			} else {
				if (!isalpha(c) && c != '_') {
					bool is_digit = isdigit(c);
					/* Can not start from digit. */
					if (!is_digit ||
					    (is_digit && key_len == 0))
						return pos - src + 1;
				}
			}
			++key_len;
		}
	}
	/* Check on unfinished brackets or strings. */
	if (state == BRACKETS || state == STRING ||
	    /* Leading <.>. */
	    (state == FIELD && key_len == 0))
		return pos - src + 1;
	parser->is_index = is_index;
	parser->pos = pos;
	if (is_index) {
		parser->index = strntoi(key, key_len);
	} else {
		parser->key = key;
		parser->key_len = key_len;
	}
	return 0;
}
