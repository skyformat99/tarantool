#include <trivia/config.h>
#include "memory.h"
#include "fiber.h"
#include <small/slab_cache.h>
#include "vy_iterators_helper.h"

static void
test_basic(void)
{
	header();

	plan(12);

	/* Create key_def */
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	assert(key_def != NULL);
	/* Create lsregion */
	struct lsregion lsregion;
	struct slab_cache *slab_cache = cord_slab_cache();
	lsregion_create(&lsregion, slab_cache->arena);
	struct vy_mem *mem = create_test_mem(&lsregion, key_def);

	is(mem->min_lsn, INT64_MAX, "mem->min_lsn on empty mem");
	is(mem->max_lsn, -1, "mem->max_lsn on empty mem");
	const struct vy_stmt_template stmts[] = {
		STMT_TEMPLATE(100, REPLACE, 1), STMT_TEMPLATE(101, REPLACE, 1),
		STMT_TEMPLATE(102, REPLACE, 1), STMT_TEMPLATE(103, REPLACE, 1),
		STMT_TEMPLATE(104, REPLACE, 1)
	};

	/* Check min/max lsn */
	const struct tuple *stmt = vy_mem_insert_template(mem, &stmts[0]);
	is(mem->min_lsn, INT64_MAX, "mem->min_lsn after prepare");
	is(mem->max_lsn, -1, "mem->max_lsn after prepare");
	vy_mem_commit_stmt(mem, stmt);
	is(mem->min_lsn, 100, "mem->min_lsn after commit");
	is(mem->max_lsn, 100, "mem->max_lsn after commit");

	/* Check vy_mem_older_lsn */
	const struct tuple *older = stmt;
	stmt = vy_mem_insert_template(mem, &stmts[1]);
	is(vy_mem_older_lsn(mem, stmt), older, "vy_mem_older_lsn 1");
	is(vy_mem_older_lsn(mem, older), NULL, "vy_mem_older_lsn 2");
	vy_mem_commit_stmt(mem, stmt);

	/* Check rollback  */
	const struct tuple *olderolder = stmt;
	older = vy_mem_insert_template(mem, &stmts[2]);
	stmt = vy_mem_insert_template(mem, &stmts[3]);
	is(vy_mem_older_lsn(mem, stmt), older, "vy_mem_rollback 1");
	vy_mem_rollback_stmt(mem, older);
	is(vy_mem_older_lsn(mem, stmt), olderolder, "vy_mem_rollback 2");

	/* Check version  */
	stmt = vy_mem_insert_template(mem, &stmts[4]);
	is(mem->version, 6, "vy_mem->version")
	vy_mem_commit_stmt(mem, stmt);
	is(mem->version, 6, "vy_mem->version")

	/* Clean up */
	vy_mem_delete(mem);
	lsregion_destroy(&lsregion);
	box_key_def_delete(key_def);

	fiber_gc();
	footer();

	check_plan();
}

static void
test_iterator_initial_restore()
{
	header();

	plan(3);

	/* Create key_def */
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	assert(key_def != NULL);

	/* Create lsregion */
	struct lsregion lsregion;
	struct slab_cache *slab_cache = cord_slab_cache();
	lsregion_create(&lsregion, slab_cache->arena);

	struct vy_mem *mem = create_test_mem(&lsregion, key_def);

	const uint64_t count = 100;
	for (uint64_t i = 0; i < count; i++) {
		const struct vy_stmt_template stmts[2] = {
			STMT_TEMPLATE(200, REPLACE, i * 2 + 1),
			STMT_TEMPLATE(300, REPLACE, i * 2 + 1)
		};
		vy_mem_insert_template(mem, &stmts[0]);
		vy_mem_insert_template(mem, &stmts[1]);
	}

	/* initial restore */
	bool wrong_rc = false;
	bool wrong_lsn = false;
	bool wrong_value = false;
	int i_fail = 0;
	for (uint64_t i = 0; i < (count * 2 + 1) * 3; i++) {
		uint64_t key = i % (count * 2 + 1);
		int64_t lsn = (i / (count * 2 + 1)) * 100 + 100;
		bool value_is_expected = lsn != 100 && (key % 2 == 1);
		char data[16];
		char *end = data;
		end = mp_encode_uint(end, key);
		assert(end <= data + sizeof(data));
		struct tuple *stmt = vy_stmt_new_select(mem->format, data, 1);

		struct vy_mem_iterator itr;
		struct vy_mem_iterator_stat stats = {0, {0, 0}};
		struct vy_read_view rv;
		rv.vlsn = lsn;
		const struct vy_read_view *prv = &rv;
		vy_mem_iterator_open(&itr, &stats, mem, ITER_EQ, stmt,
				     &prv, NULL);
		struct tuple *t;
		bool stop = false;
		int rc = itr.base.iface->restore(&itr.base, NULL, &t, &stop);

		if (rc != 0) {
			wrong_rc = true;
			i_fail = i;
			itr.base.iface->cleanup(&itr.base);
			itr.base.iface->close(&itr.base);
			continue;
		}

		if (value_is_expected) {
			if (t == NULL) {
				wrong_value = true;
				i_fail = i;
				itr.base.iface->cleanup(&itr.base);
				itr.base.iface->close(&itr.base);
				continue;
			}
			if (vy_stmt_lsn(t) != lsn) {
				wrong_lsn = true;
				i_fail = i;
			}
			uint32_t got_val;
			if (tuple_field_u32(t, 0, &got_val) ||
			    got_val != key) {
				wrong_value = true;
				i_fail = i;
			}
		} else {
			if (t != NULL) {
				wrong_value = true;
				i_fail = i;
			}
		}

		itr.base.iface->cleanup(&itr.base);
		itr.base.iface->close(&itr.base);

		tuple_unref(stmt);
	}

	ok(!wrong_rc, "check rc %d", i_fail);
	ok(!wrong_lsn, "check lsn %d", i_fail);
	ok(!wrong_value, "check value %d", i_fail);

	/* Clean up */
	vy_mem_delete(mem);
	lsregion_destroy(&lsregion);
	box_key_def_delete(key_def);

	fiber_gc();

	check_plan();

	footer();
}

static void
test_iterator_initial_restore_on_key()
{
	header();

	plan(2);

	/* Create key_def */
	uint32_t fields[] = { 0, 1 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED, FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 2);
	assert(key_def != NULL);

	/* Create lsregion */
	struct lsregion lsregion;
	struct slab_cache *slab_cache = cord_slab_cache();
	lsregion_create(&lsregion, slab_cache->arena);

	struct vy_mem *mem = create_test_mem(&lsregion, key_def);

	const int64_t count = 10;
	for (int i = 0; i < count; i++) {
		for (int j = 0; j < count; j++) {
			const struct vy_stmt_template stmts[2] = {
				STMT_TEMPLATE(200, REPLACE, i * 2 + 1, j * 2 + 1),
				STMT_TEMPLATE(300, REPLACE, i * 2 + 1, j * 2 + 1),
			};
			vy_mem_insert_template(mem, &stmts[0]);
			vy_mem_insert_template(mem, &stmts[1]);
		}
	}

	/* initial restore on key */
	bool wrong_lsn = false;
	bool wrong_value = false;
	int64_t k_fail = 0;
	uint64_t i_count = 21; /* 0,1,..,20 */
	uint64_t j_count = 21; /* 0,1,..,20 */
	int64_t vlsns[] = {INT64_MAX, 300, 200, 100};
	size_t vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	int64_t last_lsns[] = {350, 300, 250, 200, 150};
	size_t last_lsns_count = sizeof(last_lsns) / sizeof(last_lsns[0]);
	int64_t all_size = i_count * j_count * vlsns_count * last_lsns_count;
	for (int64_t k = 0; k < all_size; k++) {
		int64_t l = k;
		int64_t i = l % i_count;
		l /= i_count;
		int64_t j = l % j_count;
		l /= j_count;
		int64_t vlsn = vlsns[l % vlsns_count];
		l /= vlsns_count;
		int64_t last_lsn = last_lsns[l % last_lsns_count];
		l /= last_lsns_count;

		int64_t expect[3] = {-1, -1, -1};
		if ((i & 1) && vlsn >= 200) {
			if ((j & 1) == 0) {
				if (j != 20) {
					expect[0] = i;
					expect[1] = j + 1;
					if (vlsn >= 300)
						expect[2] = 300;
					else
						expect[2] = 200;
				}
			} else {
				bool next = last_lsn > vlsn ||
					last_lsn <= 200;
				if (next) {
					if (j != 19) {
						expect[0] = i;
						expect[1] = j + 2;
						if (vlsn >= 300)
							expect[2] = 300;
						else
							expect[2] = 200;
					}
				} else {
					assert(last_lsn <= vlsn &&
						       last_lsn > 200);
					expect[0] = i;
					expect[1] = j;
					if (last_lsn > 300)
						expect[2] = 300;
					else
						expect[2] = 200;
				}
			}
		}

		const struct vy_stmt_template key_templ =
			STMT_TEMPLATE(0, SELECT, i);
		struct tuple *key =
			vy_new_simple_stmt(mem->format, mem->upsert_format,
					   mem->format_with_colmask,
					   &key_templ);
		assert(key != NULL);

		const struct vy_stmt_template last_templ =
			STMT_TEMPLATE(last_lsn, REPLACE, i, j);
		struct tuple *last =
			vy_new_simple_stmt(mem->format, mem->upsert_format,
					   mem->format_with_colmask,
					   &last_templ);
		assert(last != NULL);

		struct vy_read_view rv;
		rv.vlsn = vlsn;
		const struct vy_read_view *prv = &rv;

		struct vy_mem_iterator itr;
		struct vy_mem_iterator_stat stats = {0, {0, 0}};
		vy_mem_iterator_open(&itr, &stats, mem, ITER_EQ, key,
				     &prv, NULL);
		struct tuple *t;
		bool stop = false;
		int rc = itr.base.iface->restore(&itr.base, last, &t, &stop);

		int64_t got[3] = {-1, -1, -1};
		if (t != NULL) {
			uint32_t tmp;
			tuple_field_u32(t, 0, &tmp);
			got[0] = tmp;
			tuple_field_u32(t, 1, &tmp);
			got[1] = tmp;
			got[2] = vy_stmt_lsn(t);
		}

		if (got[0] != expect[0] || got[1] != expect[1]) {
			k_fail = k;
			wrong_value = true;
		}
		if (got[2] != expect[2]) {
			k_fail = k;
			wrong_lsn = true;
		}

		itr.base.iface->cleanup(&itr.base);
		itr.base.iface->close(&itr.base);
		tuple_unref(key);
		tuple_unref(last);
	}

	ok(!wrong_lsn, "check lsn %lld", (long long)k_fail);
	ok(!wrong_value, "check value %lld", (long long)k_fail);

	/* Clean up */
	vy_mem_delete(mem);
	lsregion_destroy(&lsregion);
	box_key_def_delete(key_def);

	fiber_gc();

	check_plan();

	footer();
}

static void
test_iterator_restore_after_insertion()
{
	header();

	plan(1);

	/* Create key_def */
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	assert(key_def != NULL);

	/* Create format */
	struct tuple_format *format = tuple_format_new(&vy_tuple_format_vtab,
						       &key_def, 1, 0);
	assert(format != NULL);
	tuple_format_ref(format);

	/* Create lsregion */
	struct lsregion lsregion;
	struct slab_cache *slab_cache = cord_slab_cache();
	lsregion_create(&lsregion, slab_cache->arena);

	struct tuple *select_key = vy_stmt_new_select(format, "", 0);

	const int x[] = {0, 10, 20, 30, 40, 50, 60};
	const size_t x_count = sizeof(x) / sizeof(x[0]);
	const int restore_on_value = x[1];
	const int restore_on_value_reverse = x[x_count - 2];
	const int middle_value = x[x_count / 2];

	char data[16];
	char *end = data;
	end = mp_encode_array(end, 1);
	end = mp_encode_uint(end, (uint64_t)restore_on_value);
	struct tuple *restore_on_key = vy_stmt_new_replace(format, data, end);
	vy_stmt_set_lsn(restore_on_key, 100);
	end = data;
	end = mp_encode_array(end, 1);
	end = mp_encode_uint(end, (uint64_t)restore_on_value_reverse);
	struct tuple *restore_on_key_reverse = vy_stmt_new_replace(format, data, end);
	vy_stmt_set_lsn(restore_on_key_reverse, 100);

	bool wrong_output = false;
	uint64_t i_fail = 0;

	uint64_t num_of_variants = 1 << 3; /* direct, has_lsn_50/100/150 */
	for (size_t i = 0; i < x_count; i++)
		num_of_variants *= 3;
	for (uint64_t i = 0; i < num_of_variants; i++) {
		uint64_t v = i;
		bool direct = !(v & 1);
		v >>= 1;
		bool has_lsn_50 = v & 1;
		v >>= 1;
		bool has_lsn_100 = v & 1;
		v >>= 1;
		bool has_lsn_150 = v & 1;
		v >>= 1;
		bool has_x[x_count];
		bool add_x[x_count];
		bool add_smth = false;
		for (size_t j = 0; j < x_count; j++) {
			uint64_t trinity = v % 3;
			v /= 3;
			has_x[j] = trinity == 1;
			add_x[j] = trinity == 2;
			add_smth = add_smth || add_x[j];
		}
		if (!add_smth)
			continue;

		size_t expected_count = 0;
		int expected_values[x_count];
		int64_t expected_lsns[x_count];
		if (direct) {
			for (size_t j = 0; j < x_count; j++) {
				if (has_x[j] && has_lsn_100) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 100;
					expected_count++;
				} else if (has_x[j] && has_lsn_50) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 50;
					expected_count++;
				}
			}
		} else {
			for (size_t k = x_count; k > 0; k--) {
				size_t j = k - 1;
				if (has_x[j] && has_lsn_100) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 100;
					expected_count++;
				} else if (has_x[j] && has_lsn_50) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 50;
					expected_count++;
				}
			}
		}

		/* Create mem */
		struct vy_mem *mem = create_test_mem(&lsregion, key_def);
		for (size_t j = 0; j < x_count; j++) {
			if (has_x[j] && has_lsn_50) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(50, REPLACE, x[j]);
				vy_mem_insert_template(mem, &temp);
			}
			if (has_x[j] && has_lsn_100) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(100, REPLACE, x[j]);
				vy_mem_insert_template(mem, &temp);
			}
			if (has_x[j] && has_lsn_150) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(150, REPLACE, x[j]);
				vy_mem_insert_template(mem, &temp);
			}
		}

		struct vy_mem_iterator itr;
		struct vy_mem_iterator_stat stats = {0, {0, 0}};
		struct vy_read_view rv;
		rv.vlsn = 100;
		const struct vy_read_view *prv = &rv;
		vy_mem_iterator_open(&itr, &stats, mem,
				     direct ? ITER_GE : ITER_LE, select_key,
				     &prv, NULL);
		struct tuple *t;
		bool stop = false;
		int rc = itr.base.iface->next_key(&itr.base, &t, &stop);
		assert(rc == 0);
		size_t j = 0;
		while (t != NULL) {
			if (j >= expected_count) {
				wrong_output = true;
				break;
			}
			uint32_t uval = INT32_MAX;
			tuple_field_u32(t, 0, &uval);
			int val = uval;
			if (val != expected_values[j] ||
			    vy_stmt_lsn(t) != expected_lsns[j]) {
				wrong_output = true;
				break;
			}
			j++;
			if (direct && val >= middle_value)
				break;
			else if(!direct && val <= middle_value)
				break;
			rc = itr.base.iface->next_key(&itr.base, &t, &stop);
			assert(rc == 0);
		}
		if (t == NULL && j != expected_count)
			wrong_output = true;
		if (wrong_output) {
			i_fail = i;
		}


		for (size_t j = 0; j < x_count; j++) {
			if (add_x[j]) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(100, REPLACE, x[j]);
				vy_mem_insert_template(mem, &temp);
			}
			if (add_x[j] && has_lsn_150) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(150, REPLACE, x[j]);
				vy_mem_insert_template(mem, &temp);
			}
		}

		expected_count = 0;
		if (direct) {
			for (size_t j = 0; j < x_count; j++) {
				if ((x[j] > restore_on_value) &&
				    ((has_x[j] && has_lsn_100) || add_x[j])) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 100;
					expected_count++;
				} else if ((x[j] >= restore_on_value) &&
					   (has_x[j] && has_lsn_50)) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 50;
					expected_count++;
				}
			}
		} else {
			for (size_t k = x_count; k > 0; k--) {
				size_t j = k - 1;
				if ((x[j] < restore_on_value_reverse) &&
				    ((has_x[j] && has_lsn_100) || add_x[j])) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 100;
					expected_count++;
				} else if ((x[j] <= restore_on_value_reverse) &&
					   (has_x[j] && has_lsn_50)) {
					expected_values[expected_count] = x[j];
					expected_lsns[expected_count] = 50;
					expected_count++;
				}
			}
		}

		if (direct)
			rc = itr.base.iface->restore(&itr.base, restore_on_key,
						     &t, &stop);
		else
			rc = itr.base.iface->restore(&itr.base,
						     restore_on_key_reverse,
						     &t, &stop);

		j = 0;
		while (t != NULL) {
			if (j >= expected_count) {
				wrong_output = true;
				break;
			}
			uint32_t uval = INT32_MAX;
			tuple_field_u32(t, 0, &uval);
			int val = uval;
			if (val != expected_values[j] ||
			    vy_stmt_lsn(t) != expected_lsns[j]) {
				wrong_output = true;
				break;
			}
			j++;
			rc = itr.base.iface->next_key(&itr.base, &t, &stop);
			assert(rc == 0);
		}
		if (j != expected_count)
			wrong_output = true;
		if (wrong_output) {
			i_fail = i;
			break;
		}
		itr.base.iface->cleanup(&itr.base);
		itr.base.iface->close(&itr.base);

		vy_mem_delete(mem);
		lsregion_gc(&lsregion, 2);
	}

	ok(!wrong_output, "check wrong_output %llu", (long long)i_fail);

	/* Clean up */
	tuple_unref(select_key);
	tuple_unref(restore_on_key);
	tuple_unref(restore_on_key_reverse);

	tuple_format_unref(format);
	lsregion_destroy(&lsregion);
	box_key_def_delete(key_def);

	fiber_gc();

	check_plan();

	footer();
}

static void
test_iterator_restore_cases()
{
	header();

	plan(3);

	/* Create key_def */
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	assert(key_def != NULL);

	/* Create format */
	struct tuple_format *format = tuple_format_new(&vy_tuple_format_vtab,
						       &key_def, 1, 0);
	assert(format != NULL);
	tuple_format_ref(format);

	/* Create lsregion */
	struct lsregion lsregion;
	struct slab_cache *slab_cache = cord_slab_cache();
	lsregion_create(&lsregion, slab_cache->arena);

	{
		/* most complex case - LE with different keys and lsns in mem */
		struct vy_mem *mem = create_test_mem(&lsregion, key_def);
		for (int i = 1; i <= 3; i++) {
			for (int64_t lsn = 50; lsn <= 250; lsn += 50) {
				const struct vy_stmt_template temp =
					STMT_TEMPLATE(lsn, REPLACE, i);
				vy_mem_insert_template(mem, &temp);
			}
		}

		struct vy_mem_iterator itr;
		struct vy_mem_iterator_stat stats = {0, {0, 0}};
		struct vy_read_view rv;
		rv.vlsn = 200;
		const struct vy_read_view *prv = &rv;
		struct tuple *select_key = vy_stmt_new_select(format, "", 0);
		vy_mem_iterator_open(&itr, &stats, mem, ITER_LE, select_key,
				     &prv, NULL);

		struct tuple *t;
		for (int i = 0; i < 3; i++) {
			bool stop;
			int rc = itr.base.iface->next_key(&itr.base, &t, &stop);
			assert(rc == 0 && t != NULL);
		}
		uint32_t val;
		tuple_field_u32(t, 0, &val);
		ok(vy_stmt_lsn(t) == 200 && val == 1, "next_key");

		for (int i = 0; i < 2; i++) {
			int rc = itr.base.iface->next_lsn(&itr.base, &t);
			assert(rc == 0 && t != NULL);
		}
		tuple_field_u32(t, 0, &val);
		ok(vy_stmt_lsn(t) == 100 && val == 1, "next_lsn");

		const struct vy_stmt_template temp =
			STMT_TEMPLATE(100, REPLACE, 4);
		vy_mem_insert_template(mem, &temp);

		char data[16];
		char *end = data;
		end = mp_encode_array(end, 1);
		end = mp_encode_uint(end, 3);
		struct tuple *last = vy_stmt_new_replace(format, data, end);
		vy_stmt_set_lsn(last, 150);

		bool stop;
		int rc = itr.base.iface->restore(&itr.base, last, &t, &stop);
		tuple_field_u32(t, 0, &val);
		ok(vy_stmt_lsn(t) == 100 && val == 3, "restore");

		itr.base.iface->cleanup(&itr.base);
		itr.base.iface->close(&itr.base);

		vy_mem_delete(mem);
		lsregion_gc(&lsregion, 2);

		tuple_unref(last);
		tuple_unref(select_key);
	}

	/* Clean up */

	tuple_format_unref(format);
	lsregion_destroy(&lsregion);
	box_key_def_delete(key_def);

	fiber_gc();


	check_plan();

	footer();
}

int
main(int argc, char *argv[])
{
	vy_iterator_C_test_init(0);

	test_basic();
	test_iterator_initial_restore();
	test_iterator_initial_restore_on_key();
	test_iterator_restore_after_insertion();
	test_iterator_restore_cases();

	vy_iterator_C_test_finish();
	return 0;
}
