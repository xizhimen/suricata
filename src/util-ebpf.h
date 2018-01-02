/* Copyright (C) 2016-2018 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Eric Leblond <eric@regit.org>
 */

#ifndef __UTIL_EBPF_H__
#define __UTIL_EBPF_H__

#ifdef HAVE_PACKET_EBPF

#define XDP_FLAGS_UPDATE_IF_NOEXIST	(1U << 0)
#define XDP_FLAGS_SKB_MODE		(1U << 1)
#define XDP_FLAGS_DRV_MODE		(1U << 2)
#define XDP_FLAGS_HW_MODE		(1U << 3)

#include "flow-bypass.h"

struct flowv4_keys {
	__be32 src;
	__be32 dst;
	union {
		__be32 ports;
		__be16 port16[2];
	};
	__u32 ip_proto;
}  __attribute__((__aligned__(8)));

struct flowv6_keys {
    __be32 src[4];
    __be32 dst[4];
    union {
        __be32 ports;
        __be16 port16[2];
    };
    __u32 ip_proto;
}  __attribute__((__aligned__(8)));

struct pair {
    uint64_t time;
    uint64_t packets;
    uint64_t bytes;
} __attribute__((__aligned__(8)));

#define EBPF_SOCKET_FILTER  (1<<0)
#define EBPF_XDP_CODE       (1<<1)

int EBPFGetMapFDByName(const char *iface, const char *name);
int EBPFLoadFile(const char *iface, const char *path, const char * section,
                 int *val, uint8_t flags);
int EBPFSetupXDP(const char *iface, int fd, uint8_t flags);

int EBPFCheckBypassedFlowTimeout(struct flows_stats *bypassstats,
                                        struct timespec *curtime);

void EBPFRegisterExtension(void);

int EBPFAddCPUToMap(const char *iface, uint32_t i);

#endif

#endif
