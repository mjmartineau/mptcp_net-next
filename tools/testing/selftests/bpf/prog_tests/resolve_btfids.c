// SPDX-License-Identifier: GPL-2.0

#include <linux/err.h>
#include <string.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>
#include <linux/btf.h>
#include <linux/kernel.h>
#include <linux/btf_ids.h>
#include "test_progs.h"

static int duration;

struct symbol {
	const char	*name;
	int		 type;
	int		 id;
};

struct symbol test_symbols[] = {
	{ "unused",  BTF_KIND_UNKN,     0 },
	{ "S",       BTF_KIND_TYPEDEF, -1 },
	{ "T",       BTF_KIND_TYPEDEF, -1 },
	{ "U",       BTF_KIND_TYPEDEF, -1 },
	{ "S",       BTF_KIND_STRUCT,  -1 },
	{ "U",       BTF_KIND_UNION,   -1 },
	{ "func",    BTF_KIND_FUNC,    -1 },
};

BTF_ID_LIST(test_list)
BTF_ID_UNUSED
BTF_ID(typedef, S)
BTF_ID(typedef, T)
BTF_ID(typedef, U)
BTF_ID(struct,  S)
BTF_ID(union,   U)
BTF_ID(func,    func)

static int
__resolve_symbol(struct btf *btf, int type_id)
{
	const struct btf_type *type;
	const char *str;
	unsigned int i;

	type = btf__type_by_id(btf, type_id);
	if (!type) {
		PRINT_FAIL("Failed to get type for ID %d\n", type_id);
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(test_symbols); i++) {
		if (test_symbols[i].id != -1)
			continue;

		if (BTF_INFO_KIND(type->info) != test_symbols[i].type)
			continue;

		str = btf__name_by_offset(btf, type->name_off);
		if (!str) {
			PRINT_FAIL("Failed to get name for BTF ID %d\n", type_id);
			return -1;
		}

		if (!strcmp(str, test_symbols[i].name))
			test_symbols[i].id = type_id;
	}

	return 0;
}

static int resolve_symbols(void)
{
	struct btf *btf;
	int type_id;
	__u32 nr;

	btf = btf__parse_elf("btf_data.o", NULL);
	if (CHECK(libbpf_get_error(btf), "resolve",
		  "Failed to load BTF from btf_data.o\n"))
		return -1;

	nr = btf__get_nr_types(btf);

	for (type_id = 1; type_id <= nr; type_id++) {
		if (__resolve_symbol(btf, type_id))
			break;
	}

	btf__free(btf);
	return 0;
}

int test_resolve_btfids(void)
{
	unsigned int i;
	int ret = 0;

	if (resolve_symbols())
		return -1;

	/* Check BTF_ID_LIST(test_list) IDs */
	for (i = 0; i < ARRAY_SIZE(test_symbols) && !ret; i++) {
		ret = CHECK(test_list[i] != test_symbols[i].id,
			    "id_check",
			    "wrong ID for %s (%d != %d)\n", test_symbols[i].name,
			    test_list[i], test_symbols[i].id);
	}

	return ret;
}
