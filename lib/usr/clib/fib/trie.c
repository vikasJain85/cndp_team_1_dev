/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018 Vladimir Medvedkin <medvedkinv@gmail.com>
 * Copyright (c) 2019-2023 Intel Corporation
 */

#include <stdint.h>              // for uint8_t, uint64_t, uint32_t, uint...
#include <stdlib.h>              // for free, calloc
#include <stdio.h>               // for NULL
#include <cne_log.h>             // for CNE_ASSERT
#include <cne_rib6.h>            // for cne_rib6_copy_addr, cne_rib6_get_nh
#include "private_fib6.h"        // for CNE_FIB6_IPV6_ADDR_SIZE, cne_fib6...
#include <cne_fib6.h>            // for cne_fib6_conf, cne_fib6_conf::(an...
#include <errno.h>               // for EINVAL, ENOSPC, ENOENT

#include "trie.h"
#include "cne_branch_prediction.h"        // for unlikely

struct cne_fib6;
struct cne_rib6;

#ifdef CC_TRIE_AVX512_SUPPORT

#include "trie_avx512.h"

#endif /* CC_TRIE_AVX512_SUPPORT */

#define TRIE_NAMESIZE 64

enum edge { LEDGE, REDGE };

static inline cne_fib6_lookup_fn_t
get_scalar_fn(enum cne_fib_trie_nh_sz nh_sz)
{
    switch (nh_sz) {
    case CNE_FIB6_TRIE_2B:
        return cne_trie_lookup_bulk_2b;
    case CNE_FIB6_TRIE_4B:
        return cne_trie_lookup_bulk_4b;
    case CNE_FIB6_TRIE_8B:
        return cne_trie_lookup_bulk_8b;
    default:
        return NULL;
    }
}

static inline cne_fib6_lookup_fn_t
get_vector_fn(enum cne_fib_trie_nh_sz nh_sz)
{
#ifdef CC_TRIE_AVX512_SUPPORT
    if ((cne_cpu_get_flag_enabled(CNE_CPUFLAG_AVX512F) <= 0) ||
        (cne_vect_get_max_simd_bitwidth() < CNE_VECT_SIMD_512))
        return NULL;
    switch (nh_sz) {
    case CNE_FIB6_TRIE_2B:
        return cne_trie_vec_lookup_bulk_2b;
    case CNE_FIB6_TRIE_4B:
        return cne_trie_vec_lookup_bulk_4b;
    case CNE_FIB6_TRIE_8B:
        return cne_trie_vec_lookup_bulk_8b;
    default:
        return NULL;
    }
#else
    CNE_SET_USED(nh_sz);
#endif
    return NULL;
}

cne_fib6_lookup_fn_t
trie_get_lookup_fn(void *p, enum cne_fib6_lookup_type type)
{
    enum cne_fib_trie_nh_sz nh_sz;
    cne_fib6_lookup_fn_t ret_fn;
    struct cne_trie_tbl *dp = p;

    if (dp == NULL)
        return NULL;

    nh_sz = dp->nh_sz;

    switch (type) {
    case CNE_FIB6_LOOKUP_TRIE_SCALAR:
        return get_scalar_fn(nh_sz);
    case CNE_FIB6_LOOKUP_TRIE_VECTOR_AVX512:
        return get_vector_fn(nh_sz);
    case CNE_FIB6_LOOKUP_DEFAULT:
        ret_fn = get_vector_fn(nh_sz);
        return (ret_fn != NULL) ? ret_fn : get_scalar_fn(nh_sz);
    default:
        return NULL;
    }
    return NULL;
}

static void
write_to_dp(void *ptr, uint64_t val, enum cne_fib_trie_nh_sz size, int n)
{
    int i;
    uint16_t *ptr16 = (uint16_t *)ptr;
    uint32_t *ptr32 = (uint32_t *)ptr;
    uint64_t *ptr64 = (uint64_t *)ptr;

    switch (size) {
    case CNE_FIB6_TRIE_2B:
        for (i = 0; i < n; i++)
            ptr16[i] = (uint16_t)val;
        break;
    case CNE_FIB6_TRIE_4B:
        for (i = 0; i < n; i++)
            ptr32[i] = (uint32_t)val;
        break;
    case CNE_FIB6_TRIE_8B:
        for (i = 0; i < n; i++)
            ptr64[i] = (uint64_t)val;
        break;
    }
}

static void
tbl8_pool_init(struct cne_trie_tbl *dp)
{
    uint32_t i;

    /* put entire range of indexes to the tbl8 pool */
    for (i = 0; i < dp->number_tbl8s; i++)
        dp->tbl8_pool[i] = i;

    dp->tbl8_pool_pos = 0;
}

/*
 * Get an index of a free tbl8 from the pool
 */
static inline int32_t
tbl8_get(struct cne_trie_tbl *dp)
{
    if (dp->tbl8_pool_pos == dp->number_tbl8s)
        /* no more free tbl8 */
        return -ENOSPC;

    /* next index */
    return dp->tbl8_pool[dp->tbl8_pool_pos++];
}

/*
 * Put an index of a free tbl8 back to the pool
 */
static inline void
tbl8_put(struct cne_trie_tbl *dp, uint32_t tbl8_ind)
{
    dp->tbl8_pool[--dp->tbl8_pool_pos] = tbl8_ind;
}

static int
tbl8_alloc(struct cne_trie_tbl *dp, uint64_t nh)
{
    int64_t tbl8_idx;
    uint8_t *tbl8_ptr;

    tbl8_idx = tbl8_get(dp);
    if (tbl8_idx < 0)
        return tbl8_idx;
    tbl8_ptr = get_tbl_p_by_idx(dp->tbl8, tbl8_idx * TRIE_TBL8_GRP_NUM_ENT, dp->nh_sz);
    /*Init tbl8 entries with nexthop from tbl24*/
    write_to_dp((void *)tbl8_ptr, nh, dp->nh_sz, TRIE_TBL8_GRP_NUM_ENT);
    return tbl8_idx;
}

static void
tbl8_recycle(struct cne_trie_tbl *dp, void *par, uint64_t tbl8_idx)
{
    uint32_t i;
    uint64_t nh;
    uint16_t *ptr16;
    uint32_t *ptr32;
    uint64_t *ptr64;

    switch (dp->nh_sz) {
    case CNE_FIB6_TRIE_2B:
        ptr16 = &((uint16_t *)dp->tbl8)[tbl8_idx * TRIE_TBL8_GRP_NUM_ENT];
        nh    = *ptr16;
        if (nh & TRIE_EXT_ENT)
            return;
        for (i = 1; i < TRIE_TBL8_GRP_NUM_ENT; i++) {
            if (nh != ptr16[i])
                return;
        }
        write_to_dp(par, nh, dp->nh_sz, 1);
        for (i = 0; i < TRIE_TBL8_GRP_NUM_ENT; i++)
            ptr16[i] = 0;
        break;
    case CNE_FIB6_TRIE_4B:
        ptr32 = &((uint32_t *)dp->tbl8)[tbl8_idx * TRIE_TBL8_GRP_NUM_ENT];
        nh    = *ptr32;
        if (nh & TRIE_EXT_ENT)
            return;
        for (i = 1; i < TRIE_TBL8_GRP_NUM_ENT; i++) {
            if (nh != ptr32[i])
                return;
        }
        write_to_dp(par, nh, dp->nh_sz, 1);
        for (i = 0; i < TRIE_TBL8_GRP_NUM_ENT; i++)
            ptr32[i] = 0;
        break;
    case CNE_FIB6_TRIE_8B:
        ptr64 = &((uint64_t *)dp->tbl8)[tbl8_idx * TRIE_TBL8_GRP_NUM_ENT];
        nh    = *ptr64;
        if (nh & TRIE_EXT_ENT)
            return;
        for (i = 1; i < TRIE_TBL8_GRP_NUM_ENT; i++) {
            if (nh != ptr64[i])
                return;
        }
        write_to_dp(par, nh, dp->nh_sz, 1);
        for (i = 0; i < TRIE_TBL8_GRP_NUM_ENT; i++)
            ptr64[i] = 0;
        break;
    }
    tbl8_put(dp, tbl8_idx);
}

#define BYTE_SIZE 8
static inline uint32_t
get_idx(const uint8_t *ip, uint32_t prev_idx, int bytes, int first_byte)
{
    int i;
    uint32_t idx = 0;
    uint8_t bitshift;

    for (i = first_byte; i < (first_byte + bytes) && i < 16; i++) {
        bitshift = (int8_t)(((first_byte + bytes - 1) - i) * BYTE_SIZE);
        idx |= ip[i] << bitshift;
    }
    return (prev_idx * TRIE_TBL8_GRP_NUM_ENT) + idx;
}

static inline uint64_t
get_val_by_p(void *p, uint8_t nh_sz)
{
    uint64_t val = 0;

    switch (nh_sz) {
    case CNE_FIB6_TRIE_2B:
        val = *(uint16_t *)p;
        break;
    case CNE_FIB6_TRIE_4B:
        val = *(uint32_t *)p;
        break;
    case CNE_FIB6_TRIE_8B:
        val = *(uint64_t *)p;
        break;
    }
    return val;
}

/*
 * recursively recycle tbl8's
 */
static void
recycle_root_path(struct cne_trie_tbl *dp, const uint8_t *ip_part, uint8_t common_tbl8, void *prev)
{
    void *p;
    uint64_t val;

    val = get_val_by_p(prev, dp->nh_sz);
    if (unlikely((val & TRIE_EXT_ENT) != TRIE_EXT_ENT))
        return;

    if (common_tbl8 != 0) {
        p = get_tbl_p_by_idx(dp->tbl8, (val >> 1) * TRIE_TBL8_GRP_NUM_ENT + *ip_part, dp->nh_sz);
        recycle_root_path(dp, ip_part + 1, common_tbl8 - 1, p);
    }
    tbl8_recycle(dp, prev, val >> 1);
}

static inline int
build_common_root(struct cne_trie_tbl *dp, const uint8_t *ip, int common_bytes, void **tbl)
{
    void *tbl_ptr = NULL;
    uint64_t *cur_tbl;
    uint64_t val;
    int i, j, idx, prev_idx = 0;

    cur_tbl = dp->tbl24;
    for (i = 3, j = 0; i <= common_bytes; i++) {
        idx     = get_idx(ip, prev_idx, i - j, j);
        val     = get_tbl_val_by_idx(cur_tbl, idx, dp->nh_sz);
        tbl_ptr = get_tbl_p_by_idx(cur_tbl, idx, dp->nh_sz);
        if ((val & TRIE_EXT_ENT) != TRIE_EXT_ENT) {
            idx = tbl8_alloc(dp, val);
            if (unlikely(idx < 0))
                return idx;
            write_to_dp(tbl_ptr, (idx << 1) | TRIE_EXT_ENT, dp->nh_sz, 1);
            prev_idx = idx;
        } else
            prev_idx = val >> 1;

        j       = i;
        cur_tbl = dp->tbl8;
    }
    *tbl = get_tbl_p_by_idx(cur_tbl, prev_idx * TRIE_TBL8_GRP_NUM_ENT, dp->nh_sz);
    return 0;
}

static int
write_edge(struct cne_trie_tbl *dp, const uint8_t *ip_part, uint64_t next_hop, int len,
           enum edge edge, void *ent)
{
    uint64_t val = next_hop << 1;
    int tbl8_idx;
    int ret = 0;
    void *p;

    if (len != 0) {
        val = get_val_by_p(ent, dp->nh_sz);
        if ((val & TRIE_EXT_ENT) == TRIE_EXT_ENT)
            tbl8_idx = val >> 1;
        else {
            tbl8_idx = tbl8_alloc(dp, val);
            if (tbl8_idx < 0)
                return tbl8_idx;
            val = (tbl8_idx << 1) | TRIE_EXT_ENT;
        }
        p   = get_tbl_p_by_idx(dp->tbl8, (tbl8_idx * TRIE_TBL8_GRP_NUM_ENT) + *ip_part, dp->nh_sz);
        ret = write_edge(dp, ip_part + 1, next_hop, len - 1, edge, p);
        if (ret < 0)
            return ret;
        if (edge == LEDGE) {
            write_to_dp((uint8_t *)p + (1 << dp->nh_sz), next_hop << 1, dp->nh_sz,
                        UINT8_MAX - *ip_part);
        } else {
            write_to_dp(get_tbl_p_by_idx(dp->tbl8, tbl8_idx * TRIE_TBL8_GRP_NUM_ENT, dp->nh_sz),
                        next_hop << 1, dp->nh_sz, *ip_part);
        }
        tbl8_recycle(dp, &val, tbl8_idx);
    }

    write_to_dp(ent, val, dp->nh_sz, 1);
    return ret;
}

#define IPV6_MAX_IDX (CNE_FIB6_IPV6_ADDR_SIZE - 1)
#define TBL24_BYTES  3
#define TBL8_LEN     (CNE_FIB6_IPV6_ADDR_SIZE - TBL24_BYTES)

static int
install_to_dp(struct cne_trie_tbl *dp, const uint8_t *ledge, const uint8_t *r, uint64_t next_hop)
{
    void *common_root_tbl;
    void *ent;
    int ret;
    int i;
    int common_bytes;
    int llen, rlen;
    uint8_t redge[16];

    /* decrement redge by 1*/
    cne_rib6_copy_addr(redge, r);
    for (i = 15; i >= 0; i--) {
        redge[i]--;
        if (redge[i] != 0xff)
            break;
    }

    for (common_bytes = 0; common_bytes < 15; common_bytes++) {
        if (ledge[common_bytes] != redge[common_bytes])
            break;
    }

    ret = build_common_root(dp, ledge, common_bytes, &common_root_tbl);
    if (unlikely(ret != 0))
        return ret;
    /*first uncommon tbl8 byte idx*/
    uint8_t first_tbl8_byte = CNE_MAX(common_bytes, TBL24_BYTES);

    for (i = IPV6_MAX_IDX; i > first_tbl8_byte; i--) {
        if (ledge[i] != 0)
            break;
    }

    llen = i - first_tbl8_byte + (common_bytes < 3);

    for (i = IPV6_MAX_IDX; i > first_tbl8_byte; i--) {
        if (redge[i] != UINT8_MAX)
            break;
    }
    rlen = i - first_tbl8_byte + (common_bytes < 3);

    /*first noncommon byte*/
    uint8_t first_byte_idx = (common_bytes < 3) ? 0 : common_bytes;
    uint8_t first_idx_len  = (common_bytes < 3) ? 3 : 1;

    uint32_t left_idx  = get_idx(ledge, 0, first_idx_len, first_byte_idx);
    uint32_t right_idx = get_idx(redge, 0, first_idx_len, first_byte_idx);

    ent = get_tbl_p_by_idx(common_root_tbl, left_idx, dp->nh_sz);
    ret = write_edge(dp, &ledge[first_tbl8_byte + !(common_bytes < 3)], next_hop, llen, LEDGE, ent);
    if (ret < 0)
        return ret;

    if (right_idx > left_idx + 1) {
        ent = get_tbl_p_by_idx(common_root_tbl, left_idx + 1, dp->nh_sz);
        write_to_dp(ent, next_hop << 1, dp->nh_sz, right_idx - (left_idx + 1));
    }
    ent = get_tbl_p_by_idx(common_root_tbl, right_idx, dp->nh_sz);
    ret =
        write_edge(dp, &redge[first_tbl8_byte + !((common_bytes < 3))], next_hop, rlen, REDGE, ent);
    if (ret < 0)
        return ret;

    uint8_t common_tbl8 = (common_bytes < TBL24_BYTES) ? 0 : common_bytes - (TBL24_BYTES - 1);
    ent                 = get_tbl24_p(dp, ledge, dp->nh_sz);
    recycle_root_path(dp, ledge + TBL24_BYTES, common_tbl8, ent);
    return 0;
}

static void
get_nxt_net(uint8_t *ip, uint8_t depth)
{
    int i;
    uint8_t part_depth;
    uint8_t prev_byte;

    for (i = 0, part_depth = depth; part_depth > 8; part_depth -= 8, i++)
        ;

    prev_byte = ip[i];
    ip[i] += 1 << (8 - part_depth);
    if (ip[i] < prev_byte) {
        while (i > 0) {
            ip[--i] += 1;
            if (ip[i] != 0)
                break;
        }
    }
}

static int
modify_dp(struct cne_trie_tbl *dp, struct cne_rib6 *rib, const uint8_t ip[CNE_FIB6_IPV6_ADDR_SIZE],
          uint8_t depth, uint64_t next_hop)
{
    struct cne_rib6_node *tmp = NULL;
    uint8_t ledge[CNE_FIB6_IPV6_ADDR_SIZE];
    uint8_t redge[CNE_FIB6_IPV6_ADDR_SIZE];
    int ret;
    uint8_t tmp_depth;

    if (next_hop > get_max_nh(dp->nh_sz))
        return -EINVAL;

    cne_rib6_copy_addr(ledge, ip);
    do {
        tmp = cne_rib6_get_nxt(rib, ip, depth, tmp, CNE_RIB6_GET_NXT_COVER);
        if (tmp != NULL) {
            cne_rib6_get_depth(tmp, &tmp_depth);
            if (tmp_depth == depth)
                continue;
            cne_rib6_get_ip(tmp, redge);
            if (cne_rib6_is_equal(ledge, redge)) {
                get_nxt_net(ledge, tmp_depth);
                continue;
            }
            ret = install_to_dp(dp, ledge, redge, next_hop);
            if (ret != 0)
                return ret;
            get_nxt_net(redge, tmp_depth);
            cne_rib6_copy_addr(ledge, redge);
        } else {
            cne_rib6_copy_addr(redge, ip);
            get_nxt_net(redge, depth);
            if (cne_rib6_is_equal(ledge, redge))
                break;
            ret = install_to_dp(dp, ledge, redge, next_hop);
            if (ret != 0)
                return ret;
        }
    } while (tmp);

    return 0;
}

int
trie_modify(struct cne_fib6 *fib, const uint8_t ip[CNE_FIB6_IPV6_ADDR_SIZE], uint8_t depth,
            uint64_t next_hop, int op)
{
    struct cne_trie_tbl *dp;
    struct cne_rib6 *rib;
    struct cne_rib6_node *tmp = NULL;
    struct cne_rib6_node *node;
    struct cne_rib6_node *parent;
    uint8_t ip_masked[CNE_FIB6_IPV6_ADDR_SIZE];
    int i, ret = 0;
    uint64_t par_nh, node_nh;
    uint8_t tmp_depth, depth_diff = 0, parent_depth = 24;

    if ((fib == NULL) || (ip == NULL) || (depth > CNE_FIB6_MAXDEPTH))
        return -EINVAL;

    dp = cne_fib6_get_dp(fib);
    CNE_ASSERT(dp);
    rib = cne_fib6_get_rib(fib);
    CNE_ASSERT(rib);

    for (i = 0; i < CNE_FIB6_IPV6_ADDR_SIZE; i++)
        ip_masked[i] = ip[i] & get_msk_part(depth, i);

    if (depth > 24) {
        tmp = cne_rib6_get_nxt(rib, ip_masked, CNE_ALIGN_FLOOR(depth, 8), NULL,
                               CNE_RIB6_GET_NXT_COVER);
        if (tmp == NULL) {
            tmp = cne_rib6_lookup(rib, ip);
            if (tmp != NULL) {
                cne_rib6_get_depth(tmp, &tmp_depth);
                parent_depth = CNE_MAX(tmp_depth, 24);
            }
            depth_diff = CNE_ALIGN_CEIL(depth, 8) - CNE_ALIGN_CEIL(parent_depth, 8);
            depth_diff = depth_diff >> 3;
        }
    }
    node = cne_rib6_lookup_exact(rib, ip_masked, depth);
    switch (op) {
    case CNE_FIB6_ADD:
        if (node != NULL) {
            cne_rib6_get_nh(node, &node_nh);
            if (node_nh == next_hop)
                return 0;
            ret = modify_dp(dp, rib, ip_masked, depth, next_hop);
            if (ret == 0)
                cne_rib6_set_nh(node, next_hop);
            return 0;
        }

        if ((depth > 24) && (dp->rsvd_tbl8s >= dp->number_tbl8s - depth_diff))
            return -ENOSPC;

        node = cne_rib6_insert(rib, ip_masked, depth);
        if (node == NULL)
            return -1;
        cne_rib6_set_nh(node, next_hop);
        parent = cne_rib6_lookup_parent(node);
        if (parent != NULL) {
            cne_rib6_get_nh(parent, &par_nh);
            if (par_nh == next_hop)
                return 0;
        }
        ret = modify_dp(dp, rib, ip_masked, depth, next_hop);
        if (ret != 0) {
            cne_rib6_remove(rib, ip_masked, depth);
            return ret;
        }

        dp->rsvd_tbl8s += depth_diff;
        return 0;
    case CNE_FIB6_DEL:
        if (node == NULL)
            return -ENOENT;

        parent = cne_rib6_lookup_parent(node);
        if (parent != NULL) {
            cne_rib6_get_nh(parent, &par_nh);
            cne_rib6_get_nh(node, &node_nh);
            if (par_nh != node_nh)
                ret = modify_dp(dp, rib, ip_masked, depth, par_nh);
        } else
            ret = modify_dp(dp, rib, ip_masked, depth, dp->def_nh);

        if (ret != 0)
            return ret;
        cne_rib6_remove(rib, ip, depth);

        dp->rsvd_tbl8s -= depth_diff;
        return 0;
    default:
        break;
    }
    return -EINVAL;
}

void *
trie_create(const char *name, struct cne_fib6_conf *conf)
{
    struct cne_trie_tbl *dp = NULL;
    uint64_t def_nh;
    uint32_t num_tbl8;
    enum cne_fib_trie_nh_sz nh_sz;

    if ((name == NULL) || (conf == NULL) || (conf->trie.nh_sz < CNE_FIB6_TRIE_2B) ||
        (conf->trie.nh_sz > CNE_FIB6_TRIE_8B) ||
        (conf->trie.num_tbl8 > get_max_nh(conf->trie.nh_sz)) || (conf->trie.num_tbl8 == 0) ||
        (conf->default_nh > get_max_nh(conf->trie.nh_sz)))
        return NULL;

    def_nh   = conf->default_nh;
    nh_sz    = conf->trie.nh_sz;
    num_tbl8 = conf->trie.num_tbl8;

    dp = calloc(1, sizeof(struct cne_trie_tbl) + TRIE_TBL24_NUM_ENT * (1 << nh_sz));
    if (dp == NULL)
        return dp;

    write_to_dp(&dp->tbl24, (def_nh << 1), nh_sz, 1 << 24);

    dp->tbl8 = calloc(1, TRIE_TBL8_GRP_NUM_ENT * (1ll << nh_sz) * (num_tbl8 + 1));
    if (dp->tbl8 == NULL) {
        free(dp);
        return NULL;
    }
    dp->def_nh       = def_nh;
    dp->nh_sz        = nh_sz;
    dp->number_tbl8s = num_tbl8;

    dp->tbl8_pool = calloc(1, sizeof(uint32_t) * dp->number_tbl8s);
    if (dp->tbl8_pool == NULL) {
        free(dp->tbl8);
        free(dp);
        return NULL;
    }

    tbl8_pool_init(dp);

    return dp;
}

void
trie_free(void *p)
{
    struct cne_trie_tbl *dp = (struct cne_trie_tbl *)p;

    free(dp->tbl8_pool);
    free(dp->tbl8);
    free(dp);
}
