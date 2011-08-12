#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "fermi.h"
#include "rld.h"
#include "kvec.h"

double cputime();
double rssmem();

typedef struct {
	rlditr_t itr;
	int c;
	int64_t l;
} rlditr2_t;

typedef struct {
	int32_t *gap;
	kvec_t(int64_t) a;
} gaparr_t;

#define GAP_MAX INT32_MAX
#define MSG_SIZE 10000000

static gaparr_t *compute_gap_array(const rld_t *e0, const rld_t *e1)
{
	gaparr_t *g;
	uint64_t k, l, *ok, *ol, i, j, x;
	uint64_t n_processed = 1;
	int c = 0;
	double t = cputime();
	g = calloc(1, sizeof(gaparr_t));
	ok = alloca(8 * e0->asize);
	ol = alloca(8 * e0->asize);
	g->gap = calloc(e0->mcnt[0], 4);
	x = e1->mcnt[1];
	k = l = --x; // get the last sentinel of e1
	j = i = e0->mcnt[1] - 1; // to modify gap[j]
	++g->gap[j];
	for (;;) {
		rld_rank2a(e1, k - 1, l, ok, ol);
		for (c = 0; c < e1->asize; ++c)
			if (ok[c] < ol[c]) break;
		if (c == 0) {
			j = e0->mcnt[1] - 1;
			k = l = --x;
			if (x == (uint64_t)-1) break;
		} else {
			j = e0->cnt[c] + rld_rank11(e0, i, c) - 1;
			k = l = e1->cnt[c] + ok[c];
		}
		if (g->gap[j] < 0) { // the actualy value is stored in g->a
			++g->a.a[-g->gap[j] - 1];
		} else if (g->gap[j] == GAP_MAX) { // put the value in g->a
			kv_push(int64_t, g->a, 1ll + GAP_MAX);
			g->gap[j] = -(int64_t)g->a.n;
		} else ++g->gap[j]; // simply increase by 1
		i = j;
		if (++n_processed % MSG_SIZE == 0 && fm_verbose >= 3)
			fprintf(stderr, "[M::%s] processed %lld million symbols in %.3f seconds (peak memory: %.3f MB).\n", __func__,
					(long long)n_processed / 1000000, cputime() - t, rssmem());
	}
	return g;
}
// This is a clever version of rld_enc(): adjacent runs are guaranteed to be different.
inline void rld_enc2(rld_t *e, rlditr2_t *itr2, int64_t l, int c)
{
	if (l == 0) return;
	if (itr2->c != c) {
		if (itr2->l) rld_enc(e, &itr2->itr, itr2->l, itr2->c);
		itr2->l = l; itr2->c = c;
	} else itr2->l += l;
}
// take k symbols from e0 and write it to e; l0 number of c0 symbols are pending before writing
static inline void dec_enc(rld_t *e, rlditr2_t *itr, const rld_t *e0, rlditr_t *itr0, int64_t *l0, int *c0, int64_t k)
{
	if (*l0 >= k) { // there are more pending symbols
		rld_enc2(e, itr, k, *c0);
		*l0 -= k; // l0-k symbols remains
	} else { // use up all pending symbols
		int c = -1; // to please gcc
		int64_t l;
		rld_enc2(e, itr, *l0, *c0); // write all pending symbols
		k -= *l0;
		for (; k > 0; k -= l) { // we always go into this loop because l0<k
			l = rld_dec(e0, itr0, &c);
			assert(l); // the e0 stream should not be finished
			rld_enc2(e, itr, k < l? k : l, c);
		}
		*l0 = -k; *c0 = c;
	}
}

rld_t *fm_merge_array(rld_t *e0, rld_t *e1, const char *fn)
{
	gaparr_t *gap;
	uint64_t i, k;
	rlditr_t itr0, itr1;
	rlditr2_t itr;
	rld_t *e;
	int c0, c1;
	int64_t l0, l1;

	gap = compute_gap_array(e0, e1);
	e = rld_init(e0->asize, e0->sbits, fn);
	rld_itr_init(e, &itr.itr, 0);
	itr.l = l0 = l1 = 0; itr.c = c0 = c1 = -1;
	rld_itr_init(e0, &itr0, 0);
	rld_itr_init(e1, &itr1, 0);
	for (i = k = 0; i < e0->mcnt[0]; ++i) {
		int64_t g = gap->gap[i] < 0? gap->a.a[-gap->gap[i] - 1] : gap->gap[i];
		if (g) {
			//printf("gap[%lld]=%lld\n", i, g);
			dec_enc(e, &itr, e0, &itr0, &l0, &c0, k + 1); // first write symbols from the first index
			dec_enc(e, &itr, e1, &itr1, &l1, &c1, g); // then from the second
			k = 0;
		} else ++k;
	}
	if (k) dec_enc(e, &itr, e0, &itr0, &l0, &c0, k); // write the remaining symbols from the first index
	assert(l0 == 0 && l1 == 0); // both e0 and e1 stream should be finished
	rld_enc(e, &itr.itr, itr.l, itr.c); // write the remaining symbols in the iterator
	free(gap->gap); free(gap->a.a); free(gap);
	if (fn) { // if data are written to a file, deallocate e0 and e1 to save memory
		rld_destroy(e0);
		if (e1 != e0) rld_destroy(e1);
	}
	rld_enc_finish(e, &itr.itr); // this will load the full index in memory
	return e;
}

#include "khash.h"
#include "ksort.h"
KSORT_INIT_GENERIC(uint64_t)
#define h64_eq(a, b) ((a)>>32 == (b)>>32)
#define h64_hash(a) ((a)>>32)
KHASH_INIT(h64, uint64_t, char, 0, h64_hash, h64_eq)

#define BLOCK_BITS 16
#define BLOCK_MASK ((1u<<BLOCK_BITS) - 1)
#define BLOCK_SHIFT (64 - BLOCK_BITS)
#define BLOCK_CMASK ((1ll<<BLOCK_SHIFT) - 1)

typedef struct {
	int n;
	khash_t(h64) **h;
} gaphash_t;

static inline void insert_to_hash(gaphash_t *h, uint64_t j)
{
	khint_t k;
	int ret;
	khash_t(h64) *g = h->h[j>>BLOCK_BITS];
	k = kh_put(h64, g, (j&BLOCK_MASK)<<BLOCK_SHIFT|1, &ret);
	if (ret == 0) ++kh_key(g, k); // when the key is present, the key in the hash table will not be overwritten by put()
}

static gaphash_t *compute_gap_hash(const rld_t *e0, const rld_t *e1)
{
	gaphash_t *h;
	uint64_t k, l, *ok, *ol, i, j, x;
	int c = 0;
	uint64_t n_processed = 1;
	double t = cputime();
	h = calloc(1, sizeof(gaphash_t));
	h->n = (e0->mcnt[0] + BLOCK_MASK) >> BLOCK_BITS;
	h->h = malloc(h->n * sizeof(void*));
	for (i = 0; (int)i < h->n; ++i)
		h->h[i] = kh_init(h64);
	ok = alloca(8 * e0->asize);
	ol = alloca(8 * e0->asize);
	x = e1->mcnt[1];
	k = l = --x; // get the last sentinel of e1
	j = i = e0->mcnt[1] - 1; // to modify gap[j]
	insert_to_hash(h, j);
	for (;;) {
		rld_rank2a(e1, k - 1, l, ok, ol);
		for (c = 0; c < e1->asize; ++c)
			if (ok[c] < ol[c]) break;
		if (c == 0) {
			j = e0->mcnt[1] - 1;
			k = l = --x;
			if (x == (uint64_t)-1) break;
		} else {
			j = e0->cnt[c] + rld_rank11(e0, i, c) - 1;
			k = l = e1->cnt[c] + ok[c];
		}
		insert_to_hash(h, j);
		i = j;
		if (++n_processed % MSG_SIZE == 0 && fm_verbose >= 3)
			fprintf(stderr, "[M::%s] processed %lld million symbols in %.3f seconds (peak memory: %.3f MB).\n", __func__,
					(long long)n_processed / 1000000, cputime() - t, rssmem());
	}
	return h;
}

rld_t *fm_merge_hash(rld_t *e0, rld_t *e1, const char *fn)
{
	gaphash_t *gap;
	rlditr_t itr0, itr1;
	rlditr2_t itr;
	rld_t *e;
	khint_t k, l;
	int i, c0, c1;
	int64_t l0, l1, last = -1;

	gap = compute_gap_hash(e0, e1);
	e = rld_init(e0->asize, e0->sbits, fn);
	rld_itr_init(e, &itr.itr, 0);
	itr.l = l0 = l1 = 0; itr.c = c0 = c1 = -1;
	rld_itr_init(e0, &itr0, 0);
	rld_itr_init(e1, &itr1, 0);
	for (i = 0; i < gap->n; ++i) {
		khash_t(h64) *h = gap->h[i];
		for (l = 0, k = kh_begin(h); k < kh_end(h); ++k)
			if (kh_exist(h, k))
				h->keys[l++] = kh_key(h, k);
		assert(l == kh_size(h));
		free(h->flags);
		h->flags = 0;
		ks_introsort(uint64_t, kh_size(h), h->keys);
		for (k = 0; k < kh_size(h); ++k) {
			uint64_t x = (uint64_t)i<<BLOCK_BITS | h->keys[k]>>BLOCK_SHIFT;
			//printf("gap[%lld]=%lld\n", x, h->keys[k]&BLOCK_CMASK);
			dec_enc(e, &itr, e0, &itr0, &l0, &c0, x - last);
			dec_enc(e, &itr, e1, &itr1, &l1, &c1, h->keys[k]&BLOCK_CMASK);
			last = x;
		}
		kh_destroy(h64, h);
	}
	if (last != e0->mcnt[0] - 1)
		dec_enc(e, &itr, e0, &itr0, &l0, &c0, e0->mcnt[0] - 1 - last);
	assert(l0 == 0 && l1 == 0); // both e0 and e1 stream should be finished
	rld_enc(e, &itr.itr, itr.l, itr.c); // write the remaining symbols in the iterator
	free(gap->h); free(gap);
	if (fn) { // if data are written to a file, deallocate e0 and e1 to save memory
		rld_destroy(e0);
		if (e1 != e0) rld_destroy(e1);
	}
	rld_enc_finish(e, &itr.itr); // this will load the full index in memory
	return e;
}
