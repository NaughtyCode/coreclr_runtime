// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

typedef dn_simdhash_t dn_simdhash_ght_t;

typedef void         (*dn_simdhash_ght_destroy_func) (void * data);
typedef unsigned int (*dn_simdhash_ght_hash_func)    (const void * key);
typedef int32_t      (*dn_simdhash_ght_equal_func)   (const void * a, const void * b);

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

dn_simdhash_ght_t *
dn_simdhash_ght_new (
	dn_simdhash_ght_hash_func hash_func, dn_simdhash_ght_equal_func key_equal_func,
	uint32_t capacity, dn_allocator_t *allocator
);

dn_simdhash_ght_t *
dn_simdhash_ght_new_full (
	dn_simdhash_ght_hash_func hash_func, dn_simdhash_ght_equal_func key_equal_func,
	dn_simdhash_ght_destroy_func key_destroy_func, dn_simdhash_ght_destroy_func value_destroy_func,
	uint32_t capacity, dn_allocator_t *allocator
);

// compatible with g_hash_table_insert_replace
void
dn_simdhash_ght_insert_replace (
	dn_simdhash_ght_t *hash,
	void * key, void * value,
	int32_t overwrite_key
);

// faster and simpler to use than dn_simdhash_ght_try_get_value, use it wisely
void *
dn_simdhash_ght_get_value_or_default (
	dn_simdhash_ght_t *hash, void * key
);

dn_simdhash_add_result
dn_simdhash_ght_try_insert (
	dn_simdhash_ght_t *hash,
	void *key, void *value,
	dn_simdhash_insert_mode mode
);

#ifdef __cplusplus
}
#endif // __cplusplus

// compatibility shims for the g_hash_table_ versions in glib.h
#define dn_simdhash_ght_insert(h,k,v)  dn_simdhash_ght_insert_replace ((h),(k),(v),0)
#define dn_simdhash_ght_replace(h,k,v) dn_simdhash_ght_insert_replace ((h),(k),(v),1)
#define dn_simdhash_ght_add(h,k)       dn_simdhash_ght_insert_replace ((h),(k),(k),1)
