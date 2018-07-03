/*
 * Copyright (C) 2015 Martine Lenders <mlenders@inf.fu-berlin.de>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @{
 *
 * @file
 */

#include <inttypes.h>
#include <stdbool.h>

#include "rbuf.h"
#include "net/ipv6.h"
#include "net/ipv6/hdr.h"
#include "net/gnrc.h"
#include "net/gnrc/sixlowpan.h"
#include "net/gnrc/sixlowpan/frag.h"
#include "net/sixlowpan.h"
#include "thread.h"
#include "xtimer.h"
#include "utlist.h"

#define ENABLE_DEBUG    (0)
#include "debug.h"

/* estimated fragment payload size to determinate RBUF_INT_SIZE, default to
 * MAC payload size - fragment header. */
#ifndef GNRC_SIXLOWPAN_FRAG_SIZE
/* assuming 64-bit source/destination address, source PAN ID omitted */
#define GNRC_SIXLOWPAN_FRAG_SIZE (104 - 5)
#endif

#ifndef RBUF_INT_SIZE
/* same as ((int) ceil((double) N / D)) */
#define DIV_CEIL(N, D) (((N) + (D) - 1) / (D))
#define RBUF_INT_SIZE (DIV_CEIL(IPV6_MIN_MTU, GNRC_SIXLOWPAN_FRAG_SIZE) * RBUF_SIZE)
#endif

static rbuf_int_t rbuf_int[RBUF_INT_SIZE];

static rbuf_t rbuf[RBUF_SIZE];

static char l2addr_str[3 * IEEE802154_LONG_ADDRESS_LEN];

static xtimer_t _gc_timer;
static msg_t _gc_timer_msg = { .type = GNRC_SIXLOWPAN_MSG_FRAG_GC_RBUF };

/* ------------------------------------
 * internal function definitions
 * ------------------------------------*/
/* checks whether start and end overlaps, but not identical to, given interval i */
static inline bool _rbuf_int_overlap_partially(rbuf_int_t *i, uint16_t start, uint16_t end);
/* gets a free entry from interval buffer */
static rbuf_int_t *_rbuf_int_get_free(void);
/* remove entry from reassembly buffer */
static void _rbuf_rem(rbuf_t *entry);
/* update interval buffer of entry */
static bool _rbuf_update_ints(rbuf_t *entry, uint16_t offset, size_t frag_size);
/* gets an entry identified by its tupel */
static rbuf_t *_rbuf_get(const void *src, size_t src_len,
                         const void *dst, size_t dst_len,
                         size_t size, uint16_t tag);

void rbuf_add(gnrc_netif_hdr_t *netif_hdr, gnrc_pktsnip_t *pkt,
              size_t frag_size, size_t offset)
{
    rbuf_t *entry;
    /* cppcheck-suppress variableScope
     * (reason: cppcheck is clearly wrong here) */
    unsigned int data_offset = 0;
    size_t original_size = frag_size;
    sixlowpan_frag_t *frag = pkt->data;
    rbuf_int_t *ptr;
    uint8_t *data = ((uint8_t *)pkt->data) + sizeof(sixlowpan_frag_t);

    rbuf_gc();
    entry = _rbuf_get(gnrc_netif_hdr_get_src_addr(netif_hdr), netif_hdr->src_l2addr_len,
                      gnrc_netif_hdr_get_dst_addr(netif_hdr), netif_hdr->dst_l2addr_len,
                      byteorder_ntohs(frag->disp_size) & SIXLOWPAN_FRAG_SIZE_MASK,
                      byteorder_ntohs(frag->tag));

    if (entry == NULL) {
        DEBUG("6lo rbuf: reassembly buffer full.\n");
        return;
    }

    ptr = entry->ints;

    /* dispatches in the first fragment are ignored */
    if (offset == 0) {
        if (data[0] == SIXLOWPAN_UNCOMP) {
            data++;             /* skip 6LoWPAN dispatch */
            frag_size--;
        }
#ifdef MODULE_GNRC_SIXLOWPAN_IPHC
        else if (sixlowpan_iphc_is(data)) {
            size_t iphc_len, nh_len = 0;
            iphc_len = gnrc_sixlowpan_iphc_decode(&entry->super.pkt, pkt,
                                                  entry->super.pkt->size,
                                                  sizeof(sixlowpan_frag_t),
                                                  &nh_len);
            if (iphc_len == 0) {
                DEBUG("6lo rfrag: could not decode IPHC dispatch\n");
                gnrc_pktbuf_release(entry->super.pkt);
                _rbuf_rem(entry);
                return;
            }
            data += iphc_len;       /* take remaining data as data */
            frag_size -= iphc_len;  /* and reduce frag size by IPHC dispatch length */
            /* but add IPv6 header + next header lengths */
            frag_size += sizeof(ipv6_hdr_t) + nh_len;
            /* start copying after IPv6 header and next headers */
            data_offset += sizeof(ipv6_hdr_t) + nh_len;
        }
#endif
    }
    else {
        data++; /* FRAGN header is one byte longer (offset) */
    }

    if ((offset + frag_size) > entry->super.pkt->size) {
        DEBUG("6lo rfrag: fragment too big for resulting datagram, discarding datagram\n");
        gnrc_pktbuf_release(entry->super.pkt);
        _rbuf_rem(entry);
        return;
    }

    /* If the fragment overlaps another fragment and differs in either the size
     * or the offset of the overlapped fragment, discards the datagram
     * https://tools.ietf.org/html/rfc4944#section-5.3 */
    while (ptr != NULL) {
        if (_rbuf_int_overlap_partially(ptr, offset, offset + frag_size - 1)) {
            DEBUG("6lo rfrag: overlapping intervals, discarding datagram\n");
            gnrc_pktbuf_release(entry->super.pkt);
            _rbuf_rem(entry);

            /* "A fresh reassembly may be commenced with the most recently
             * received link fragment"
             * https://tools.ietf.org/html/rfc4944#section-5.3 */
            rbuf_add(netif_hdr, pkt, original_size, offset);

            return;
        }

        ptr = ptr->next;
    }

    if (_rbuf_update_ints(entry, offset, frag_size)) {
        DEBUG("6lo rbuf: add fragment data\n");
        entry->super.current_size += (uint16_t)frag_size;
        memcpy(((uint8_t *)entry->super.pkt->data) + offset + data_offset, data,
               frag_size - data_offset);
    }

    if (entry->super.current_size == entry->super.pkt->size) {
        gnrc_pktsnip_t *netif = gnrc_netif_hdr_build(entry->super.src,
                                                     entry->super.src_len,
                                                     entry->super.dst,
                                                     entry->super.dst_len);

        if (netif == NULL) {
            DEBUG("6lo rbuf: error allocating netif header\n");
            gnrc_pktbuf_release(entry->super.pkt);
            _rbuf_rem(entry);
            return;
        }

        /* copy the transmit information of the latest fragment into the newly
         * created header to have some link_layer information. The link_layer
         * info of the previous fragments is discarded.
         */
        gnrc_netif_hdr_t *new_netif_hdr = netif->data;
        new_netif_hdr->if_pid = netif_hdr->if_pid;
        new_netif_hdr->flags = netif_hdr->flags;
        new_netif_hdr->lqi = netif_hdr->lqi;
        new_netif_hdr->rssi = netif_hdr->rssi;
        LL_APPEND(entry->super.pkt, netif);
        gnrc_sixlowpan_dispatch_recv(entry->super.pkt, NULL, 0);
        _rbuf_rem(entry);
    }
}

static inline bool _rbuf_int_overlap_partially(rbuf_int_t *i, uint16_t start, uint16_t end)
{
    /* start and ends are both inclusive, so using <= for both */
    return ((i->start <= end) && (start <= i->end)) && /* overlaps */
        ((start != i->start) || (end != i->end)); /* not identical */
}

static rbuf_int_t *_rbuf_int_get_free(void)
{
    for (unsigned int i = 0; i < RBUF_INT_SIZE; i++) {
        if (rbuf_int[i].end == 0) { /* start must be smaller than end anyways*/
            return rbuf_int + i;
        }
    }

    return NULL;
}

static void _rbuf_rem(rbuf_t *entry)
{
    while (entry->ints != NULL) {
        rbuf_int_t *next = entry->ints->next;

        entry->ints->start = 0;
        entry->ints->end = 0;
        entry->ints->next = NULL;
        entry->ints = next;
    }

    entry->super.pkt = NULL;
}

static bool _rbuf_update_ints(rbuf_t *entry, uint16_t offset, size_t frag_size)
{
    rbuf_int_t *new;
    uint16_t end = (uint16_t)(offset + frag_size - 1);

    new = _rbuf_int_get_free();

    if (new == NULL) {
        DEBUG("6lo rfrag: no space left in rbuf interval buffer.\n");
        return false;
    }

    new->start = offset;
    new->end = end;

    DEBUG("6lo rfrag: add interval (%" PRIu16 ", %" PRIu16 ") to entry (%s, ",
          new->start, new->end, gnrc_netif_addr_to_str(entry->super.src,
                                                       entry->super.src_len,
                                                       l2addr_str));
    DEBUG("%s, %u, %u)\n", gnrc_netif_addr_to_str(entry->super.dst,
                                                  entry->super.dst_len,
                                                  l2addr_str),
          (unsigned)entry->super.pkt->size, entry->super.tag);

    LL_PREPEND(entry->ints, new);

    return true;
}

void rbuf_gc(void)
{
    uint32_t now_usec = xtimer_now_usec();
    unsigned int i;

    for (i = 0; i < RBUF_SIZE; i++) {
        /* since pkt occupies pktbuf, aggressivly collect garbage */
        if ((rbuf[i].super.pkt != NULL) &&
              ((now_usec - rbuf[i].arrival) > RBUF_TIMEOUT)) {
            DEBUG("6lo rfrag: entry (%s, ",
                  gnrc_netif_addr_to_str(rbuf[i].super.src,
                                         rbuf[i].super.src_len,
                                         l2addr_str));
            DEBUG("%s, %u, %u) timed out\n",
                  gnrc_netif_addr_to_str(rbuf[i].super.dst,
                                         rbuf[i].super.dst_len,
                                         l2addr_str),
                  (unsigned)rbuf[i].super.pkt->size, rbuf[i].super.tag);

            gnrc_pktbuf_release(rbuf[i].super.pkt);
            _rbuf_rem(&(rbuf[i]));
        }
    }
}

static inline void _set_rbuf_timeout(void)
{
    xtimer_set_msg(&_gc_timer, RBUF_TIMEOUT, &_gc_timer_msg, sched_active_pid);
}

static rbuf_t *_rbuf_get(const void *src, size_t src_len,
                         const void *dst, size_t dst_len,
                         size_t size, uint16_t tag)
{
    rbuf_t *res = NULL, *oldest = NULL;
    uint32_t now_usec = xtimer_now_usec();

    for (unsigned int i = 0; i < RBUF_SIZE; i++) {
        /* check first if entry already available */
        if ((rbuf[i].super.pkt != NULL) && (rbuf[i].super.pkt->size == size) &&
            (rbuf[i].super.tag == tag) && (rbuf[i].super.src_len == src_len) &&
            (rbuf[i].super.dst_len == dst_len) &&
            (memcmp(rbuf[i].super.src, src, src_len) == 0) &&
            (memcmp(rbuf[i].super.dst, dst, dst_len) == 0)) {
            DEBUG("6lo rfrag: entry %p (%s, ", (void *)(&rbuf[i]),
                  gnrc_netif_addr_to_str(rbuf[i].super.src,
                                         rbuf[i].super.src_len,
                                         l2addr_str));
            DEBUG("%s, %u, %u) found\n",
                  gnrc_netif_addr_to_str(rbuf[i].super.dst,
                                         rbuf[i].super.dst_len,
                                         l2addr_str),
                  (unsigned)rbuf[i].super.pkt->size, rbuf[i].super.tag);
            rbuf[i].arrival = now_usec;
            _set_rbuf_timeout();
            return &(rbuf[i]);
        }

        /* if there is a free spot: remember it */
        if ((res == NULL) && (rbuf[i].super.pkt == NULL)) {
            res = &(rbuf[i]);
        }

        /* remember oldest slot */
        /* note that xtimer_now will overflow in ~1.2 hours */
        if ((oldest == NULL) || (oldest->arrival - rbuf[i].arrival < UINT32_MAX / 2)) {
            oldest = &(rbuf[i]);
        }
    }

    /* entry not in buffer and no empty spot found */
    if (res == NULL) {
        assert(oldest != NULL);
        /* if oldest->pkt == NULL, res must not be NULL */
        assert(oldest->super.pkt != NULL);
        DEBUG("6lo rfrag: reassembly buffer full, remove oldest entry\n");
        gnrc_pktbuf_release(oldest->super.pkt);
        _rbuf_rem(oldest);
        res = oldest;
    }

    /* now we have an empty spot */

    res->super.pkt = gnrc_pktbuf_add(NULL, NULL, size, GNRC_NETTYPE_IPV6);
    if (res->super.pkt == NULL) {
        DEBUG("6lo rfrag: can not allocate reassembly buffer space.\n");
        return NULL;
    }

    *((uint64_t *)res->super.pkt->data) = 0;  /* clean first few bytes for later
                                               * look-ups */
    res->arrival = now_usec;
    memcpy(res->super.src, src, src_len);
    memcpy(res->super.dst, dst, dst_len);
    res->super.src_len = src_len;
    res->super.dst_len = dst_len;
    res->super.tag = tag;
    res->super.current_size = 0;

    DEBUG("6lo rfrag: entry %p (%s, ", (void *)res,
          gnrc_netif_addr_to_str(res->super.src, res->super.src_len,
                                 l2addr_str));
    DEBUG("%s, %u, %u) created\n",
          gnrc_netif_addr_to_str(res->super.dst, res->super.dst_len,
                                 l2addr_str), (unsigned)res->super.pkt->size,
          res->super.tag);

    _set_rbuf_timeout();

    return res;
}

/** @} */
