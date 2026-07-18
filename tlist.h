typedef struct tlist tlist;

#define TLIST_INTERNAL static

#ifndef TLIST_API
#define TLIST_API
#endif

#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif

TLIST_INTERNAL int tlist_mrand(unsigned *seed)
{
	return ((*seed =
		 (*seed + 1) * 1103515245 + 12345 - 1) + 1) & 0x7fffffff;
}

typedef struct item *pitem;
struct item {
	unsigned prior, cnt;
	pitem l, r;
};

TLIST_INTERNAL unsigned tlist_cnt(pitem it)
{
	return it ? it->cnt : 0;
}

TLIST_INTERNAL void tlist_upd_cnt(pitem it)
{
	if (it)
		it->cnt = tlist_cnt(it->l) + tlist_cnt(it->r) + 1;
}

TLIST_INTERNAL void tlist_merge(pitem * t, pitem l, pitem r)
{
	if (!l || !r)
		*t = l ? l : r;
	else if (l->prior > r->prior)
		tlist_merge(&l->r, l->r, r), *t = l;
	else
		tlist_merge(&r->l, l, r->l), *t = r;
	tlist_upd_cnt(*t);
}

TLIST_INTERNAL void tlist_split(pitem t, pitem * l, pitem * r, unsigned key,
				unsigned add)
{
	if (!t) {
		*l = *r = 0;
		return;
	}
	unsigned cur_key = add + tlist_cnt(t->l);
	if (key <= cur_key)
		tlist_split(t->l, l, &t->l, key, add), *r = t;
	else
		tlist_split(t->r, &t->r, r, key, add + 1 + tlist_cnt(t->l)),
		    *l = t;
	tlist_upd_cnt(t);
}

TLIST_INTERNAL pitem tlist_getitem(pitem t, unsigned idx, unsigned add)
{
	if (!t)
		return t;
	unsigned ls = tlist_cnt(t->l), cur_key = add + ls;
	if (cur_key == idx)
		return t;
	if (cur_key < idx)
		return tlist_getitem(t->r, idx, add + 1 + ls);
	else
		return tlist_getitem(t->l, idx, add);
}

TLIST_INTERNAL void tlist_insert_item(pitem * t, pitem n, unsigned idx)
{
	pitem t1, t2;
	tlist_split(*t, &t1, &t2, idx, 0);
	tlist_merge(t, t1, n);
	tlist_merge(t, *t, t2);
}

TLIST_INTERNAL void tlist_remove(pitem * t, unsigned idx, unsigned add)
{
	pitem n;
	if (!(*t))
		return;
	unsigned cur_key = add + tlist_cnt((*t)->l), new_add = cur_key + 1;
	unsigned lk = UINT_MAX, rk = UINT_MAX;
	if ((*t)->l)
		lk = tlist_cnt((*t)->l->l) + add;
	if ((*t)->r)
		rk = tlist_cnt((*t)->r->l) + new_add;
	if (cur_key == idx) {
		tlist_merge(t, (*t)->l, (*t)->r);
	} else if (lk == idx) {
		tlist_merge(&n, (*t)->l->l, (*t)->l->r);
		(*t)->l = n;
		tlist_upd_cnt(*t);
	} else if (rk == idx) {
		tlist_merge(&n, (*t)->r->l, (*t)->r->r);
		(*t)->r = n;
		tlist_upd_cnt(*t);
	} else if (cur_key < idx) {
		tlist_remove(&(*t)->r, idx, new_add);
		tlist_upd_cnt(*t);
	} else {
		tlist_remove(&(*t)->l, idx, add);
		tlist_upd_cnt(*t);
	}
}

TLIST_INTERNAL pitem tlist_new_item(void *value, unsigned valsz, unsigned *seed)
{
	pitem n = malloc(sizeof(struct item) + valsz);
	if (!n)
		return n;
	memcpy(n + 1, value, valsz);
	n->prior = tlist_mrand(seed);
	n->cnt = 1;
	n->l = n->r = 0;
	return n;
}

struct tlist {
	unsigned seed;
	unsigned itemsize;
	pitem root;
};

TLIST_API struct tlist *tlist_new(unsigned itemsize)
{
	struct tlist *new = malloc(sizeof(struct tlist));
	if (!new)
		return 0;
	new->seed = 385 - 1;
	new->itemsize = itemsize;
	new->root = 0;
	return new;
}

TLIST_INTERNAL void *tlist_data(pitem it)
{
	return it + 1;
}

TLIST_API size_t tlist_getsize(struct tlist *t)
{
	return tlist_cnt(t->root);
}

TLIST_API void *tlist_get(struct tlist *t, size_t idx)
{
	if (idx >= tlist_cnt(t->root))
		return 0;
	return tlist_data(tlist_getitem(t->root, idx, 0));
}

TLIST_API int tlist_insert(struct tlist *t, size_t idx, void *value)
{
	if (idx > tlist_cnt(t->root))
		return 0;
	pitem new = tlist_new_item(value, t->itemsize, &t->seed);
	if (!new)
		return 0;
	tlist_insert_item(&t->root, new, idx);
	return 1;
}

TLIST_API int tlist_insert_sorted(struct tlist *t, void *value,
			       int (*cmp)(const void *, const void *))
{
	size_t n = tlist_getsize(t);
	size_t i;
	for (i = 0; i < n; i++) {
		void *cur = tlist_get(t, i);
		if (cmp(value, cur) < 0)
			break;
	}
	return tlist_insert(t, i, value);
}

TLIST_INTERNAL int tlist_delete_impl(struct tlist *t, size_t idx)
{
	if (idx >= tlist_cnt(t->root))
		return 0;
	pitem it = tlist_getitem(t->root, idx, 0);
	tlist_remove(&t->root, idx, 0);
	free(it);
	return 1;
}

TLIST_API int tlist_delete(struct tlist *t, size_t idx)
{
	return tlist_delete_impl(t, idx);
}

TLIST_API void tlist_free_items(struct tlist *t)
{
	while (tlist_cnt(t->root))
		tlist_delete_impl(t, 0);
}

TLIST_API void *tlist_free(struct tlist *t)
{
	tlist_free_items(t);
	free(t);
	return 0;
}

#undef TLIST_INTERNAL

