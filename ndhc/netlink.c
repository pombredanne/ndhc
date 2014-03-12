/* netlink.c - netlink physical link notification handling and info retrieval
 *
 * Copyright (c) 2011 Nicholas J. Kain <njkain at gmail dot com>
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

#include <arpa/inet.h>
#include <assert.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

#include "netlink.h"
#include "log.h"
#include "nl.h"
#include "state.h"

static int nlrtattr_assign(struct nlattr *attr, int type, void *data)
{
    struct nlattr **tb = data;
    if (type >= IFLA_MAX)
        return 0;
    tb[type] = attr;
    return 0;
}

static void get_if_index_and_mac(const struct nlmsghdr *nlh,
                                 struct ifinfomsg *ifm)
{
    struct nlattr *tb[IFLA_MAX] = {0};
    nl_attr_parse(nlh, sizeof *ifm, nlrtattr_assign, tb);
    if (!tb[IFLA_IFNAME])
        return;
    if (!strncmp(client_config.interface,
                 nlattr_get_data(tb[IFLA_IFNAME]),
                 sizeof client_config.interface)) {
        client_config.ifindex = ifm->ifi_index;
        if (!tb[IFLA_ADDRESS])
            suicide("FATAL: Adapter %s lacks a hardware address.");
        int maclen = nlattr_get_len(tb[IFLA_ADDRESS]) - 4;
        if (maclen != 6)
            suicide("FATAL: Adapter hardware address length should be 6, but is %u.",
                    maclen);

        const unsigned char *mac =
            (unsigned char *)nlattr_get_data(tb[IFLA_ADDRESS]);
        log_line("%s hardware address %x:%x:%x:%x:%x:%x",
                 client_config.interface,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        memcpy(client_config.arp, mac, 6);
    }
}

static int nl_process_msgs(const struct nlmsghdr *nlh, void *data)
{
    struct ifinfomsg *ifm = nlmsg_get_data(nlh);
    struct client_state_t *cs = data;

    switch(nlh->nlmsg_type) {
        case RTM_NEWLINK:
            if (!client_config.ifindex)
                get_if_index_and_mac(nlh, ifm);
            if (ifm->ifi_index != client_config.ifindex)
                break;
            // IFF_UP corresponds to ifconfig down or ifconfig up.
            if (ifm->ifi_flags & IFF_UP) {
                // IFF_RUNNING is the hardware carrier.
                if (ifm->ifi_flags & IFF_RUNNING) {
                    if (cs->ifsPrevState != IFS_UP) {
                        cs->ifsPrevState = IFS_UP;
                        ifup_action(cs);
                    }
                } else if (cs->ifsPrevState != IFS_DOWN) {
                    // Interface configured, but no hardware carrier.
                    cs->ifsPrevState = IFS_DOWN;
                    ifnocarrier_action(cs);
                }
            } else if (cs->ifsPrevState != IFS_SHUT) {
                // User shut down the interface.
                cs->ifsPrevState = IFS_SHUT;
                ifdown_action(cs);
            }
            break;
        case RTM_DELLINK:
            if (ifm->ifi_index != client_config.ifindex)
                break;
            if (cs->ifsPrevState != IFS_REMOVED) {
                cs->ifsPrevState = IFS_REMOVED;
                log_line("Interface removed.  Exiting.");
                exit(EXIT_SUCCESS);
            }
            break;
        default:
            break;
    }
    return 1;
}

void handle_nl_message(struct client_state_t *cs)
{
    char nlbuf[8192];
    ssize_t ret;
    assert(cs->nlFd != -1);
    do {
        ret = nl_recv_buf(cs->nlFd, nlbuf, sizeof nlbuf);
        if (ret == -1)
            break;
        if (nl_foreach_nlmsg(nlbuf, ret, cs->nlPortId, nl_process_msgs, cs)
            == -1)
            break;
    } while (ret > 0);
}

static int nl_sendgetlink(struct client_state_t *cs)
{
    char nlbuf[8192];
    struct nlmsghdr *nlh = (struct nlmsghdr *)nlbuf;
    ssize_t r;

    memset(nlbuf, 0, sizeof nlbuf);
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof (struct rtattr));
    nlh->nlmsg_type = RTM_GETLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    nlh->nlmsg_seq = time(NULL);

    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
    };
retry_sendto:
    r = sendto(cs->nlFd, nlbuf, nlh->nlmsg_len, 0,
               (struct sockaddr *)&addr, sizeof addr);
    if (r < 0) {
        if (errno == EINTR)
            goto retry_sendto;
        else {
            log_warning("%s: (%s) netlink sendto socket failed: %s",
                        client_config.interface, __func__, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int nl_getifdata(struct client_state_t *cs)
{
    if (nl_sendgetlink(cs))
        return -1;

    for (int pr = 0; !pr;) {
        pr = poll(&((struct pollfd){.fd=cs->nlFd,.events=POLLIN}), 1, -1);
        if (pr == 1)
            handle_nl_message(cs);
        else if (pr == -1)
            suicide("nl: poll failed");
    }
    return 0;
}

