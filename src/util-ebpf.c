/* Copyright (C) 2018 Open Information Security Foundation
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
 * \ingroup afppacket
 *
 * @{
 */

/**
 * \file
 *
 * \author Eric Leblond <eric@regit.org>
 *
 * eBPF utility
 *
 */

#define PCAP_DONT_INCLUDE_PCAP_BPF_H 1
#define SC_PCAP_DONT_INCLUDE_PCAP_H 1

#include "suricata-common.h"
#include "flow-bypass.h"

#ifdef HAVE_PACKET_EBPF

#include <sys/time.h>
#include <sys/resource.h>

#include "util-ebpf.h"
#include "util-cpu.h"
#include "util-device.h"

#include "device-storage.h"
#include "flow-storage.h"
#include "flow.h"
#include "flow-hash.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <net/if.h>
#include "config.h"

#define BPF_MAP_MAX_COUNT 16

#define BYPASSED_FLOW_TIMEOUT   60

static int g_livedev_storage_id = -1;
static int g_flow_storage_id = -1;

struct bpf_map_item {
    char * name;
    int fd;
};

struct bpf_maps_info {
    struct bpf_map_item array[BPF_MAP_MAX_COUNT];
    SC_ATOMIC_DECLARE(uint64_t, ipv4_hash_count);
    SC_ATOMIC_DECLARE(uint64_t, ipv6_hash_count);
    int last;
};

typedef struct BypassedIfaceList_ { 
    LiveDevice *dev;
    struct BypassedIfaceList_ *next;
} BypassedIfaceList;

static void BpfMapsInfoFree(void *bpf)
{
    struct bpf_maps_info *bpfinfo = (struct bpf_maps_info *)bpf;
    int i;
    for (i = 0; i < bpfinfo->last; i ++) {
        if (bpfinfo->array[i].name) {
            SCFree(bpfinfo->array[i].name);
        }
    }
    SCFree(bpfinfo);
}

static void BypassedListFree(void *ifl)
{
    BypassedIfaceList *mifl = (BypassedIfaceList *)ifl;
    BypassedIfaceList *nifl;
    while (mifl) {
        nifl = mifl->next;
        SCFree(mifl);
        mifl = nifl;
    }
}

static void EBPFDeleteKey(int fd, void *key)
{
    bpf_map_delete_elem(fd, key);
}

static struct bpf_maps_info *EBPFGetBpfMap(const char *iface)
{
    LiveDevice *livedev = LiveGetDevice(iface);
    if (livedev == NULL)
        return NULL;
    void *data = LiveDevGetStorageById(livedev, g_livedev_storage_id);

    return (struct bpf_maps_info *)data;
}

/**
 * Get file descriptor of a map in the scope of a interface
 *
 * \param iface the interface where the map need to be looked for
 * \param name the name of the map
 * \return the file descriptor or -1 in case of error
 */
int EBPFGetMapFDByName(const char *iface, const char *name)
{
    int i;

    if (iface == NULL || name == NULL)
        return -1;
    struct bpf_maps_info *bpf_maps = EBPFGetBpfMap(iface);
    if (bpf_maps == NULL)
        return -1;

    for (i = 0; i < BPF_MAP_MAX_COUNT; i++) {
        if (!bpf_maps->array[i].name)
            continue;
        if (!strcmp(bpf_maps->array[i].name, name)) {
            SCLogDebug("Got fd %d for eBPF map '%s'", bpf_maps->array[i].fd, name);
            return bpf_maps->array[i].fd;
        }
    }
    return -1;
}

/** 
 * Load a section of an eBPF file
 *
 * This function loads a section inside an eBPF and return
 * via the parameter val the file descriptor that will be used to
 * inject the eBPF code into the kernel via a syscall.
 *
 * \param path the path of the eBPF file to load
 * \param section the section in the eBPF file to load
 * \param val a pointer to an integer that will be the file desc
 * \return -1 in case of error and 0 in case of success
 */
int EBPFLoadFile(const char *iface, const char *path, const char * section,
                 int *val, uint8_t flags)
{
    int err, pfd;
    bool found = false;
    struct bpf_object *bpfobj = NULL;
    struct bpf_program *bpfprog = NULL;
    struct bpf_map *map = NULL;

    if (iface == NULL)
        return -1;
    LiveDevice *livedev = LiveGetDevice(iface);
    if (livedev == NULL)
        return -1;

    if (! path) {
        SCLogError(SC_ERR_INVALID_VALUE, "No file defined to load eBPF from");
        return -1;
    }

    /* Sending the eBPF code to the kernel requires a large amount of
     * locked memory so we set it to unlimited to avoid a ENOPERM error */
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &r) != 0) {
        SCLogError(SC_ERR_MEM_ALLOC, "Unable to lock memory: %s (%d)",
                   strerror(errno), errno);
        return -1;
    }

    /* Open the eBPF file and parse it */
    bpfobj = bpf_object__open(path);
    long error = libbpf_get_error(bpfobj);
    if (error) {
        char err_buf[128];
        libbpf_strerror(error, err_buf,
                        sizeof(err_buf));
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Unable to load eBPF objects in '%s': %s",
                   path, err_buf);
        return -1;
    }

    /* Let's check that our section is here */
    bpf_object__for_each_program(bpfprog, bpfobj) {
        const char *title = bpf_program__title(bpfprog, 0);
        if (!strcmp(title, section)) {
            if (flags & EBPF_SOCKET_FILTER) {
                bpf_program__set_socket_filter(bpfprog);
            } else {
                bpf_program__set_xdp(bpfprog);
            }
            found = true;
            break;
        }
    }

    if (found == false) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "No section '%s' in '%s' file. Will not be able to use the file",
                   section,
                   path);
        return -1;
    }

    err = bpf_object__load(bpfobj);
    if (err < 0) {
        if (err == -EPERM) {
            SCLogError(SC_ERR_MEM_ALLOC,
                    "Permission issue when loading eBPF object: "
                    "%s (%d)",
                    strerror(err),
                    err);
        } else {
            char buf[129];
            libbpf_strerror(err, buf, sizeof(buf));
            SCLogError(SC_ERR_INVALID_VALUE,
                    "Unable to load eBPF object: %s (%d)",
                    buf,
                    err);
        }
        return -1;
    }

    /* Kernel and userspace are sharing data via map. Userspace access to the
     * map via a file descriptor. So we need to store the map to fd info. For
     * that we use bpf_maps_info:: */
    struct bpf_maps_info *bpf_map_data = SCCalloc(1, sizeof(*bpf_map_data));
    if (bpf_map_data == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Can't allocate bpf map array");
        return -1;
    }
    SC_ATOMIC_INIT(bpf_map_data->ipv4_hash_count);
    SC_ATOMIC_INIT(bpf_map_data->ipv6_hash_count);

    /* Store the maps in bpf_maps_info:: */
    bpf_map__for_each(map, bpfobj) {
        if (bpf_map_data->last == BPF_MAP_MAX_COUNT) {
            SCLogError(SC_ERR_NOT_SUPPORTED, "Too many BPF maps in eBPF files");
            break;
        }
        SCLogDebug("Got a map '%s' with fd '%d'", bpf_map__name(map), bpf_map__fd(map));
        bpf_map_data->array[bpf_map_data->last].fd = bpf_map__fd(map);
        bpf_map_data->array[bpf_map_data->last].name = SCStrdup(bpf_map__name(map));
        if (!bpf_map_data->array[bpf_map_data->last].name) {
            SCLogError(SC_ERR_MEM_ALLOC, "Unable to duplicate map name");
            BpfMapsInfoFree(bpf_map_data);
            return -1;
        }
        bpf_map_data->last++;
    }

    /* Attach the bpf_maps_info to the LiveDevice via the device storage */
    LiveDevSetStorageById(livedev, g_livedev_storage_id, bpf_map_data);

    /* Finally we get the file descriptor for our eBPF program. We will use
     * the fd to attach the program to the socket (eBPF case) or to the device
     * (XDP case). */
    pfd = bpf_program__fd(bpfprog);
    if (pfd == -1) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Unable to find %s section", section);
        return -1;
    }

    *val = pfd;
    return 0;
}

/**
 * Attach a XDP program identified by its file descriptor to a device
 * 
 * \param iface the name of interface
 * \param fd the eBPF/XDP program file descriptor
 * \param a flag to pass to attach function mostly used to set XDP mode 
 * \return -1 in case of error, 0 if success
 */
int EBPFSetupXDP(const char *iface, int fd, uint8_t flags)
{
#ifdef HAVE_PACKET_XDP
    unsigned int ifindex = if_nametoindex(iface);
    if (ifindex == 0) {
        SCLogError(SC_ERR_INVALID_VALUE,
                "Unknown interface '%s'", iface);
        return -1;
    }
    int err = bpf_set_link_xdp_fd(ifindex, fd, flags);
    if (err != 0) {
        char buf[129];
        libbpf_strerror(err, buf, sizeof(buf));
        SCLogError(SC_ERR_INVALID_VALUE, "Unable to set XDP on '%s': %s (%d)",
                iface, buf, err);
        return -1;
    }
#endif
    return 0;
}

static int EBPFUpdateFlowForKey(struct flows_stats *flowstats, FlowKey *flow_key,
                               uint32_t hash, uint64_t pkts_cnt, uint64_t bytes_cnt)
{
    Flow *f = FlowGetExistingtFlowFromHash(flow_key, hash);
    if (f != NULL) {
        SCLogDebug("bypassed flow found %d -> %d, doing accounting",
                    f->sp, f->dp);
        if (flow_key->sp == f->sp) {
            if (pkts_cnt != f->todstpktcnt) {
                f->todstpktcnt = pkts_cnt;
                f->todstbytecnt = bytes_cnt;
                /* interval based so no meaning to update the millisecond.
                 * Let's keep it fast and simple */
                f->lastts.tv_sec = time(NULL);
            }
        } else {
            if (pkts_cnt != f->tosrcpktcnt) {
                f->tosrcpktcnt = pkts_cnt;
                f->tosrcbytecnt = bytes_cnt;
                f->lastts.tv_sec = time(NULL);
            }
        }
        FLOWLOCK_UNLOCK(f);
        return 0;
    } else {
        /* Flow has disappeared so it has been removed due to timeout.
         * We discard the flow and do accounting */
        SCLogDebug("No flow for %d -> %d", flow_key->sp, flow_key->dp);
        SCLogDebug("Dead with pkts %lu bytes %lu", pkts_cnt, bytes_cnt);
        flowstats->count++;
        flowstats->packets += pkts_cnt;
        flowstats->bytes += bytes_cnt;
        return pkts_cnt;
    }
}

/**
 * Bypassed flows cleaning for IPv4
 *
 * This function iterates on all the flows of the IPv4 table
 * looking for timeouted flow to delete from the flow table.
 */
static int EBPFForEachFlowV4Table(LiveDevice *dev, const char *name,
                                  struct flows_stats *flowstats,
                                  struct timespec *ctime,
                                  struct ebpf_timeout_config *tcfg)
{
    int mapfd = EBPFGetMapFDByName(dev->dev, name);
    struct flowv4_keys key = {}, next_key;
    int found = 0;
    unsigned int i;
    uint64_t hash_cnt = 0;

    if (tcfg->cpus_count == 0) {
        SCLogWarning(SC_ERR_INVALID_VALUE, "CPU count should not be 0");
        return 0;
    }

    while (bpf_map_get_next_key(mapfd, &key, &next_key) == 0) {
        uint64_t pkts_cnt = 0;
        uint64_t bytes_cnt = 0;
        hash_cnt++;
        /* We use a per CPU structure so we will get a array of values. But if nr_cpus
         * is 1 then we have a global hash. */
        struct pair values_array[tcfg->cpus_count];
        memset(values_array, 0, sizeof(values_array));
        int res = bpf_map_lookup_elem(mapfd, &key, values_array);
        if (res < 0) {
            SCLogDebug("no entry in v4 table for %d -> %d", key.port16[0], key.port16[1]);
            SCLogDebug("errno: (%d) %s", errno, strerror(errno));
            key = next_key;
            continue;
        }
        for (i = 0; i < tcfg->cpus_count; i++) {
            /* let's start accumulating value so we can compute the counters */
            SCLogDebug("%d: Adding pkts %lu bytes %lu", i,
                       values_array[i].packets, values_array[i].bytes);
            pkts_cnt += values_array[i].packets;
            bytes_cnt += values_array[i].bytes;
        }
        /* Get the corresponding Flow in the Flow table to compare and update
         * its counters and lastseen if needed */
        FlowKey flow_key;
        if (tcfg->mode == AFP_MODE_XDP_BYPASS) {
            flow_key.sp = ntohs(key.port16[0]);
            flow_key.dp = ntohs(key.port16[1]);
        } else {
            flow_key.sp = key.port16[0];
            flow_key.dp = key.port16[1];
        }
        flow_key.src.family = AF_INET;
        flow_key.src.addr_data32[0] = key.src;
        flow_key.src.addr_data32[1] = 0;
        flow_key.src.addr_data32[2] = 0;
        flow_key.src.addr_data32[3] = 0;
        flow_key.dst.family = AF_INET;
        flow_key.dst.addr_data32[0] = key.dst;
        flow_key.dst.addr_data32[1] = 0;
        flow_key.dst.addr_data32[2] = 0;
        flow_key.dst.addr_data32[3] = 0;
        flow_key.vlan_id[0] = key.vlan_id[0];
        flow_key.vlan_id[1] = key.vlan_id[1];
        flow_key.proto = key.ip_proto;
        flow_key.recursion_level = 0;
        pkts_cnt = EBPFUpdateFlowForKey(flowstats, &flow_key, values_array[0].hash,
                                        pkts_cnt, bytes_cnt);
        if (pkts_cnt > 0) {
            SC_ATOMIC_ADD(dev->bypassed, pkts_cnt);
            EBPFDeleteKey(mapfd, &key);
            found = 1;
        }
        key = next_key;
    }

    struct bpf_maps_info *bpfdata = LiveDevGetStorageById(dev, g_livedev_storage_id);
    if (bpfdata) {
        SC_ATOMIC_SET(bpfdata->ipv4_hash_count, hash_cnt);
    }

    return found;
}

/**
 * Bypassed flows cleaning for IPv6
 *
 * This function iterates on all the flows of the IPv4 table
 * looking for timeouted flow to delete from the flow table.
 */
static int EBPFForEachFlowV6Table(LiveDevice *dev, const char *name,
                                  struct flows_stats *flowstats,
                                  struct timespec *ctime,
                                  struct ebpf_timeout_config *tcfg)
{
    int mapfd = EBPFGetMapFDByName(dev->dev, name);
    struct flowv6_keys key = {}, next_key;
    int found = 0;
    unsigned int i;
    uint64_t hash_cnt = 0;

    if (tcfg->cpus_count == 0) {
        SCLogWarning(SC_ERR_INVALID_VALUE, "CPU count should not be 0");
        return 0;
    }

    while (bpf_map_get_next_key(mapfd, &key, &next_key) == 0) {
        uint64_t pkts_cnt = 0;
        uint64_t bytes_cnt = 0;
        hash_cnt++;
        /* We use a per CPU structure so we will get a array of values. But if nr_cpus
         * is 1 then we have a global hash. */
        struct pair values_array[tcfg->cpus_count];
        memset(values_array, 0, sizeof(values_array));
        int res = bpf_map_lookup_elem(mapfd, &key, values_array);
        if (res < 0) {
            SCLogDebug("no entry in v4 table for %d -> %d", key.port16[0], key.port16[1]);
            key = next_key;
            continue;
        }
        for (i = 0; i < tcfg->cpus_count; i++) {
            /* let's start accumulating value so we can compute the counters */
            SCLogDebug("%d: Adding pkts %lu bytes %lu", i,
                       values_array[i].packets, values_array[i].bytes);
            pkts_cnt += values_array[i].packets;
            bytes_cnt += values_array[i].bytes;
        }
        /* Get the corresponding Flow in the Flow table to compare and update
         * its counters  and lastseen if needed */
        FlowKey flow_key;
        if (tcfg->mode == AFP_MODE_XDP_BYPASS) {
            flow_key.sp = ntohs(key.port16[0]);
            flow_key.dp = ntohs(key.port16[1]);
        } else {
            flow_key.sp = key.port16[0];
            flow_key.dp = key.port16[1];
        }
        flow_key.src.family = AF_INET6;
        flow_key.src.addr_data32[0] = key.src[0];
        flow_key.src.addr_data32[1] = key.src[1];
        flow_key.src.addr_data32[2] = key.src[2];
        flow_key.src.addr_data32[3] = key.src[3];
        flow_key.dst.family = AF_INET6;
        flow_key.dst.addr_data32[0] = key.dst[0];
        flow_key.dst.addr_data32[1] = key.dst[1];
        flow_key.dst.addr_data32[2] = key.dst[2];
        flow_key.dst.addr_data32[3] = key.dst[3];
        flow_key.vlan_id[0] = key.vlan_id[0];
        flow_key.vlan_id[1] = key.vlan_id[1];
        flow_key.proto = key.ip_proto;
        flow_key.recursion_level = 0;
        pkts_cnt = EBPFUpdateFlowForKey(flowstats, &flow_key, values_array[0].hash,
                                        pkts_cnt, bytes_cnt);
        if (pkts_cnt > 0) {
            SC_ATOMIC_ADD(dev->bypassed, pkts_cnt);
            EBPFDeleteKey(mapfd, &key);
            found = 1;
        }
        key = next_key;
    }

    struct bpf_maps_info *bpfdata = LiveDevGetStorageById(dev, g_livedev_storage_id);
    if (bpfdata) {
        SC_ATOMIC_SET(bpfdata->ipv6_hash_count, hash_cnt);
    }
    return found;
}

/**
 * Flow timeout checking function
 *
 * This function is called by the Flow bypass manager to trigger removal
 * of entries in the kernel/userspace flow table if needed.
 *
 */
int EBPFCheckBypassedFlowTimeout(struct flows_stats *bypassstats,
                                        struct timespec *curtime,
                                        void *data)
{
    struct flows_stats local_bypassstats = { 0, 0, 0};
    int ret = 0;
    int tcount = 0;
    LiveDevice *ldev = NULL, *ndev;
    struct ebpf_timeout_config *cfg = (struct ebpf_timeout_config *)data;

    if (cfg == NULL) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Programming error, contact developer");
        return 0;
    }

    while(LiveDeviceForEach(&ldev, &ndev)) {
        tcount = EBPFForEachFlowV4Table(ldev, "flow_table_v4",
                                        &local_bypassstats, curtime,
                                        cfg);
        if (tcount) {
            bypassstats->count = local_bypassstats.count;
            bypassstats->packets = local_bypassstats.packets ;
            bypassstats->bytes = local_bypassstats.bytes;
            ret = 1;
        }
        memset(&local_bypassstats, 0, sizeof(local_bypassstats));
        tcount = EBPFForEachFlowV6Table(ldev, "flow_table_v6",
                                        &local_bypassstats, curtime,
                                        cfg);
        if (tcount) {
            bypassstats->count += local_bypassstats.count;
            bypassstats->packets += local_bypassstats.packets ;
            bypassstats->bytes += local_bypassstats.bytes;
            ret = 1;
        }
    }
    return ret;
}

#ifdef BUILD_UNIX_SOCKET
TmEcode EBPFGetBypassedStats(json_t *cmd, json_t *answer, void *data)
{
    LiveDevice *ldev = NULL, *ndev;

    json_t *ifaces = NULL;
    while(LiveDeviceForEach(&ldev, &ndev)) {
        struct bpf_maps_info *bpfdata = LiveDevGetStorageById(ldev, g_livedev_storage_id);
        if (bpfdata) {
            uint64_t ipv4_hash_count = SC_ATOMIC_GET(bpfdata->ipv4_hash_count);
            uint64_t ipv6_hash_count = SC_ATOMIC_GET(bpfdata->ipv6_hash_count);
            json_t *iface = json_object();
            if (ifaces == NULL) {
                ifaces = json_object();
                if (ifaces == NULL) {
                    json_object_set_new(answer, "message",
                            json_string("internal error at json object creation"));
                    return TM_ECODE_FAILED;
                }
            }
            json_object_set_new(iface, "ipv4_count", json_integer(ipv4_hash_count));
            json_object_set_new(iface, "ipv6_count", json_integer(ipv6_hash_count));
            json_object_set_new(ifaces, ldev->dev, iface);
        }
    }
    if (ifaces) {
        json_object_set_new(answer, "message", ifaces);
        SCReturnInt(TM_ECODE_OK);
    }

    json_object_set_new(answer, "message",
                        json_string("No interface using eBPF bypass"));
    SCReturnInt(TM_ECODE_FAILED);
}
#endif

void EBPFRegisterExtension(void)
{
    g_livedev_storage_id = LiveDevStorageRegister("bpfmap", sizeof(void *), NULL, BpfMapsInfoFree);
    g_flow_storage_id = FlowStorageRegister("bypassedlist", sizeof(void *), NULL, BypassedListFree);
}


#ifdef HAVE_PACKET_XDP

static uint32_t g_redirect_iface_cpu_counter = 0;

static int EBPFAddCPUToMap(const char *iface, uint32_t i)
{
    int cpumap = EBPFGetMapFDByName(iface, "cpu_map");
    uint32_t queue_size = 4096;
    int ret;

    if (cpumap < 0) {
        SCLogError(SC_ERR_AFP_CREATE, "Can't find cpu_map");
        return -1;
    }
    ret = bpf_map_update_elem(cpumap, &i, &queue_size, 0);
    if (ret) {
        SCLogError(SC_ERR_AFP_CREATE, "Create CPU entry failed (err:%d)", ret);
        return -1;
    }
    int cpus_available = EBPFGetMapFDByName(iface, "cpus_available");
    if (cpus_available < 0) {
        SCLogError(SC_ERR_AFP_CREATE, "Can't find cpus_available map");
        return -1;
    }

    ret = bpf_map_update_elem(cpus_available, &g_redirect_iface_cpu_counter, &i, 0);
    if (ret) {
        SCLogError(SC_ERR_AFP_CREATE, "Create CPU entry failed (err:%d)", ret);
        return -1;
    }
    return 0;
}

static void EBPFRedirectMapAddCPU(int i, void *data)
{
    if (EBPFAddCPUToMap(data, i) < 0) {
        SCLogError(SC_ERR_INVALID_VALUE,
                "Unable to add CPU %d to set", i);
    } else {
        g_redirect_iface_cpu_counter++;
    }
}

void EBPFBuildCPUSet(ConfNode *node, char *iface)
{
    uint32_t key0 = 0;
    int mapfd = EBPFGetMapFDByName(iface, "cpus_count");
    if (mapfd < 0) {
        SCLogError(SC_ERR_INVALID_VALUE,
                "Unable to find 'cpus_count' map");
        return;
    }
    g_redirect_iface_cpu_counter = 0;
    if (node == NULL) {
        bpf_map_update_elem(mapfd, &key0, &g_redirect_iface_cpu_counter,
                        BPF_ANY);
        return;
    }
    BuildCpusetWithCallback("xdp-cpu-redirect", node,
            EBPFRedirectMapAddCPU,
            iface);
    bpf_map_update_elem(mapfd, &key0, &g_redirect_iface_cpu_counter,
                        BPF_ANY);
}

int EBPFSetPeerIface(const char *iface, const char *out_iface)
{
    int mapfd = EBPFGetMapFDByName(iface, "tx_peer");
    if (mapfd < 0) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Unable to find 'tx_peer' map");
        return -1;
    }
    int intmapfd = EBPFGetMapFDByName(iface, "tx_peer_int");
    if (intmapfd < 0) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Unable to find 'tx_peer_int' map");
        return -1;
    }

    int key0 = 0;
    unsigned int peer_index = if_nametoindex(out_iface);
    if (peer_index == 0) {
        SCLogError(SC_ERR_INVALID_VALUE, "No iface '%s'", out_iface);
        return -1;
    }
    int ret = bpf_map_update_elem(mapfd, &key0, &peer_index, BPF_ANY);
    if (ret) {
        SCLogError(SC_ERR_AFP_CREATE, "Create peer entry failed (err:%d)", ret);
        return -1;
    }
    ret = bpf_map_update_elem(intmapfd, &key0, &peer_index, BPF_ANY);
    if (ret) {
        SCLogError(SC_ERR_AFP_CREATE, "Create peer entry failed (err:%d)", ret);
        return -1;
    }
    return 0;
}

/**
 * Bypass the flow on all ifaces it is seen on. This is used
 * in IPS mode.
 */

int EBPFUpdateFlow(Flow *f, Packet *p, void *data)
{
    BypassedIfaceList *ifl = (BypassedIfaceList *)FlowGetStorageById(f, g_flow_storage_id);
    if (ifl == NULL) {
        ifl = SCCalloc(1, sizeof(*ifl));
        if (ifl == NULL) {
            return 0;
        }
        ifl->dev = p->livedev;
        FlowSetStorageById(f, g_flow_storage_id, ifl);
        return 1;
    }
    /* Look for packet iface in the list */
    BypassedIfaceList *ldev = ifl;
    while (ldev) {
        if (p->livedev == ldev->dev) {
            return 1;
        }
        ldev = ldev->next;
    }
    /* Call bypass function if ever not in the list */ 
    p->BypassPacketsFlow(p);

    /* Add iface to the list */
    BypassedIfaceList *nifl = SCCalloc(1, sizeof(*nifl));
    if (nifl == NULL) {
        return 0;
    }
    nifl->dev = p->livedev;
    nifl->next = ifl;
    FlowSetStorageById(f, g_flow_storage_id, nifl);
    return 1;
}

#endif /* HAVE_PACKET_XDP */

#endif
