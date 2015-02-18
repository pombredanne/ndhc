/* arp.h - functions to call the interface change daemon
 *
 * Copyright (c) 2010-2015 Nicholas J. Kain <njkain at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef ARP_H_
#define ARP_H_

#include <stdbool.h>
#include <stdint.h>
#include <net/if_arp.h>
#include "ndhc.h"
#include "dhcp.h"

struct arpMsg {
    // Ethernet header
    uint8_t  h_dest[6];     // 00 destination ether addr
    uint8_t  h_source[6];   // 06 source ether addr
    uint16_t h_proto;       // 0c packet type ID field

    // ARP packet
    uint16_t htype;         // 0e hardware type (must be ARPHRD_ETHER)
    uint16_t ptype;         // 10 protocol type (must be ETH_P_IP)
    uint8_t  hlen;          // 12 hardware address length (must be 6)
    uint8_t  plen;          // 13 protocol address length (must be 4)
    uint16_t operation;     // 14 ARP opcode
    uint8_t  smac[6];       // 16 sender's hardware address
    uint8_t  sip4[4];       // 1c sender's IP address
    uint8_t  dmac[6];       // 20 target's hardware address
    uint8_t  dip4[4];       // 26 target's IP address
    uint8_t  pad[18];       // 2a pad for min. ethernet payload (60 bytes)
};

extern int arp_probe_wait;
extern int arp_probe_num;
extern int arp_probe_min;
extern int arp_probe_max;

typedef enum {
    AS_NONE = 0,        // Nothing to react to wrt ARP
    AS_COLLISION_CHECK, // Checking to see if another host has our IP before
                        // accepting a new lease.
    AS_GW_CHECK,        // Seeing if the default GW still exists on the local
                        // segment after the hardware link was lost.
    AS_GW_QUERY,        // Finding the default GW MAC address.
    AS_DEFENSE,         // Defending our IP address (RFC5227)
    AS_MAX,
} arp_state_t;

typedef enum {
    ASEND_COLLISION_CHECK,
    ASEND_GW_PING,
    ASEND_ANNOUNCE,
    ASEND_MAX,
} arp_send_t;

struct arp_stats {
    long long ts;
    int count;
};

struct arp_data {
    struct dhcpmsg dhcp_packet;   // Used only for AS_COLLISION_CHECK
    struct arpMsg reply;
    struct arp_stats send_stats[ASEND_MAX];
    long long wake_ts[AS_MAX];
    long long last_conflict_ts;   // TS of the last conflicting ARP seen.
    long long arp_check_start_ts; // TS of when we started the
                                  // AS_COLLISION_CHECK state.
    size_t reply_offset;
    unsigned int total_conflicts; // Total number of address conflicts on
                                  // the interface.  Never decreases.
    int gw_check_initpings;       // Initial count of ASEND_GW_PING when
                                  // AS_GW_CHECK was entered.
    uint16_t probe_wait_time;     // Time to wait for a COLLISION_CHECK reply
                                  // (in ms?).
    bool using_bpf:1;             // Is a BPF installed on the ARP socket?
    bool relentless_def:1;        // Don't give up defense no matter what.
    bool router_replied:1;
    bool server_replied:1;
};

void arp_reply_clear(void);

int arp_packet_get(struct client_state_t cs[static 1]);

void set_arp_relentless_def(bool v);
void arp_reset_send_stats(void);
void arp_close_fd(struct client_state_t cs[static 1]);
int arp_check(struct client_state_t cs[static 1],
              struct dhcpmsg packet[static 1]);
int arp_gw_check(struct client_state_t cs[static 1]);
int arp_set_defense_mode(struct client_state_t cs[static 1]);
int arp_gw_failed(struct client_state_t cs[static 1]);

int arp_do_collision_check(struct client_state_t cs[static 1]);
int arp_collision_timeout(struct client_state_t cs[static 1], long long nowts);
int arp_do_defense(struct client_state_t cs[static 1]);
int arp_defense_timeout(struct client_state_t cs[static 1], long long nowts);
int arp_do_gw_query(struct client_state_t cs[static 1]);
int arp_gw_query_timeout(struct client_state_t cs[static 1], long long nowts);
int arp_do_gw_check(struct client_state_t cs[static 1]);
int arp_gw_check_timeout(struct client_state_t cs[static 1], long long nowts);

// No action needs to be taken.
#define ARPR_OK 0
// There was no conflict with another host.
#define ARPR_FREE 1
// Another host already has our assigned address.
#define ARPR_CONFLICT -1
// The operation couldn't complete because of an error such as rfkill.
#define ARPR_FAIL -2


// There is no new packet.
#define ARPP_NONE 0
// We have a pending packet.
#define ARPP_HAVE 1

long long arp_get_wake_ts(void);

#endif /* ARP_H_ */
