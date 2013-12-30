/* Copyright (C) 2013 Open Information Security Foundation
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
 * \ingroup httplayer
 *
 * @{
 */

/**
 * \file
 *
 * \author Eric Leblond <eric@regit.org>
 *
 * This file provides a memory handling for the HTTP protocol support.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "conf.h"
#include "util-mem.h"
#include "util-misc.h"

#include "app-layer-htp-mem.h"

uint64_t htp_config_memcap = 0;

SC_ATOMIC_DECLARE(uint64_t, htp_memuse);
SC_ATOMIC_DECLARE(uint64_t, htp_memcap);

void HTPParseMemcap()
{
    char *conf_val;

    /** set config values for memcap, prealloc and hash_size */
    if ((ConfGet("app-layer.protocols.http.memcap", &conf_val)) == 1)
    {
        if (ParseSizeStringU64(conf_val, &htp_config_memcap) < 0) {
            SCLogError(SC_ERR_SIZE_PARSE, "Error parsing http.memcap "
                       "from conf file - %s.  Killing engine",
                       conf_val);
            exit(EXIT_FAILURE);
        }
    } else {
        /* default to unlimited */
        htp_config_memcap = 0;
    }
}

void HTPIncrMemuse(uint64_t size)
{
    (void) SC_ATOMIC_ADD(htp_memuse, size);
    return;
}

void HTPDecrMemuse(uint64_t size)
{
    (void) SC_ATOMIC_SUB(htp_memuse, size);
    return;
}

void HTPMemuseCounter(ThreadVars *tv,  TcpReassemblyThreadCtx *trt)
{
    uint64_t tmpval = SC_ATOMIC_GET(htp_memuse);
    SCPerfCounterSetUI64(trt->counter_htp_memuse, tv->sc_perf_pca, tmpval);
    tmpval = SC_ATOMIC_GET(htp_memcap);
    SCPerfCounterSetUI64(trt->counter_htp_memcap, tv->sc_perf_pca, tmpval);
    return;
}
/**
 *  \brief Check if alloc'ing "size" would mean we're over memcap
 *
 *  \retval 1 if in bounds
 *  \retval 0 if not in bounds
 */
int HTPCheckMemcap(uint64_t size)
{
    if (htp_config_memcap == 0 || size + SC_ATOMIC_GET(htp_memuse) <= htp_config_memcap)
        return 1;
    (void) SC_ATOMIC_ADD(htp_memcap, 1);
    return 0;
}

void *HTPMalloc(size_t size)
{
    void *ptr = NULL;

    if (HTPCheckMemcap((uint32_t)size) == 0)
        return NULL;

    ptr = SCMalloc(size);

    if (unlikely(ptr == NULL))
        return NULL;

    HTPIncrMemuse((uint64_t)size);

    return ptr;
}

void *HTPRealloc(void *ptr, size_t orig_size, size_t size)
{
    void *rptr = NULL;

    if (HTPCheckMemcap((uint32_t)(size - orig_size)) == 0)
        return NULL;

    rptr = SCRealloc(ptr, size);
    if (rptr == NULL)
        return NULL;

    HTPIncrMemuse((uint64_t)(size - orig_size));

    return rptr;
}

void HTPFree(void *ptr, size_t size)
{
    SCFree(ptr);

    HTPDecrMemuse((uint64_t)size);
}


/**
 * @}
 */
