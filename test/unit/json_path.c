#include "json/path.h"
#include "unit.h"
#include "trivia/util.h"
#include <string.h>

#define reset_to_new_path(value) \
	path = value; \
	len = strlen(value); \
	json_path_parser_create(&parser, path, len);

#define is_next_index(value_len, value) \
	path = parser.pos; \
	is(json_next(&parser), 0, "parse <%." #value_len "s>", path); \
	ok(parser.is_index, "<%." #value_len "s> is index", path); \
	is(parser.index, value, "<%." #value_len "s> is " #value, path);

#define is_next_key(value) \
	len = strlen(value); \
	is(json_next(&parser), 0, "parse <" value ">"); \
	is(parser.is_index, false, "<" value "> is not index"); \
	is(parser.key_len, len, "key len is %d", len); \
	is(strncmp(parser.key, value, len), 0, "key is " value);

void
test_basic()
{
	header();
	plan(54);
	const char *path;
	int len;
	struct json_path_parser parser;

	reset_to_new_path("[0].field1.field2['field3'][5]");
	is_next_index(3, 0);
	is_next_key("field1");
	is_next_key("field2");
	is_next_key("field3");
	is_next_index(3, 5);

	reset_to_new_path("[3].field[2].field")
	is_next_index(3, 3);
	is_next_key("field");
	is_next_index(3, 2);
	is_next_key("field");

	reset_to_new_path("[\"f1\"][\"f2'3'\"]");
	is_next_key("f1");
	is_next_key("f2'3'");

	/* Support both '.field1...' and 'field1...'. */
	reset_to_new_path(".field1");
	is_next_key("field1");

	/* Long number. */
	reset_to_new_path("[1234]");
	is_next_index(6, 1234);

	/* Empty path. */
	reset_to_new_path("");
	is(json_next(&parser), 0, "parse empty path");
	is(parser.key_len, 0, "no keys");
	is(parser.is_index, false, "no index");

	/* Path with no '.' at the beginning. */
	reset_to_new_path("field1.field2");
	is_next_key("field1");

	check_plan();
	footer();
}

#define check_new_path_on_error(value, errpos) \
	reset_to_new_path(value); \
	is(json_next(&parser), errpos, "error on position %d" \
	   " for <%s>", errpos, path);

struct path_and_errpos {
	const char *path;
	int errpos;
};

void
test_errors()
{
	header();
	plan(17);
	const char *path;
	int len;
	struct json_path_parser parser;
	const struct path_and_errpos errors[] = {
		/* Double [[. */
		{"[[", 2},
		/* Not string inside []. */
		{"[field]", 2},
		/* String outside of []. */
		{"'field1'.field2", 1},
		/* Empty brackets. */
		{"[]", 2},
		/* Empty string. */
		{"''", 1},
		/* Spaces inside and between identifiers. */
		{" field1", 1},
		{"fiel d1", 5},
		{"field\t1", 6},
		/* Start from digit. */
		{"1field", 1},
		{".1field", 2},
		/* Unfinished identifiers. */
		{"['field", 8},
		{"['field'", 9},
		{"[123", 5},
		/*
		 * Not trivial error: can not write
		 * '[]' after '.'.
		 */
		{".[123]", 2},
		/* Misc. */
		{"[.]", 2},
	};
	for (size_t i = 0; i < lengthof(errors); ++i) {
		reset_to_new_path(errors[i].path);
		int errpos = errors[i].errpos;
		is(json_next(&parser), errpos, "error on position %d for <%s>",
		   errpos, path);
	}

	reset_to_new_path("f.[2]")
	json_next(&parser);
	is(json_next(&parser), 3, "can not write <field.[index]>")

	reset_to_new_path("f.")
	json_next(&parser);
	is(json_next(&parser), 3, "error in leading <.>");

	check_plan();
	footer();
}

int
main()
{
	header();
	plan(2);

	test_basic();
	test_errors();

	int rc = check_plan();
	footer();
	return rc;
}
