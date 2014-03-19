/* ifchange.c - functions to call the interface change daemon
 *
 * Copyright (c) 2004-2014 Nicholas J. Kain <njkain at gmail dot com>
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

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

#include "options.h"
#include "ndhc.h"
#include "dhcp.h"
#include "options.h"
#include "arp.h"
#include "log.h"
#include "io.h"
#include "strl.h"
#include "ifchange.h"

static struct dhcpmsg cfg_packet; // Copy of the current configuration packet.

static int ifcmd_raw(char *buf, size_t buflen, char *optname,
                     char *optdata, ssize_t optlen)
{
    if (!optdata)
        return -1;
    if (buflen < strlen(optname) + optlen + 3)
        return -1;
    if (optlen > INT_MAX || optlen < 0)
        return -1;
    int ioptlen = (int)optlen;
    ssize_t olen = snprintf(buf, buflen, "%s:%.*s;",
                            optname, ioptlen, optdata);
    if (olen < 0 || (size_t)olen >= buflen)
        return -2;
    return olen;
}

static int ifchd_cmd_bytes(char *buf, size_t buflen, char *optname,
                           uint8_t *optdata, ssize_t optlen)
{
    return ifcmd_raw(buf, buflen, optname, (char *)optdata, optlen);
}

static int ifchd_cmd_u8(char *buf, size_t buflen, char *optname,
                        uint8_t *optdata, ssize_t optlen)
{
    if (!optdata || optlen < 1)
        return -1;
    char numbuf[16];
    uint8_t c = optdata[0];
    ssize_t olen = snprintf(numbuf, sizeof numbuf, "%c", c);
    if (olen < 0 || (size_t)olen >= sizeof numbuf)
        return -1;
    return ifcmd_raw(buf, buflen, optname, numbuf, strlen(numbuf));
}

static int ifchd_cmd_u16(char *buf, size_t buflen, char *optname,
                        uint8_t *optdata, ssize_t optlen)
{
    if (!optdata || optlen < 2)
        return -1;
    char numbuf[16];
    uint16_t v;
    memcpy(&v, optdata, 2);
    v = ntohs(v);
    ssize_t olen = snprintf(numbuf, sizeof numbuf, "%hu", v);
    if (olen < 0 || (size_t)olen >= sizeof numbuf)
        return -1;
    return ifcmd_raw(buf, buflen, optname, numbuf, strlen(numbuf));
}

static int ifchd_cmd_s32(char *buf, size_t buflen, char *optname,
                        uint8_t *optdata, ssize_t optlen)
{
    if (!optdata || optlen < 4)
        return -1;
    char numbuf[16];
    int32_t v;
    memcpy(&v, optdata, 4);
    v = ntohl(v);
    ssize_t olen = snprintf(numbuf, sizeof numbuf, "%d", v);
    if (olen < 0 || (size_t)olen >= sizeof numbuf)
        return -1;
    return ifcmd_raw(buf, buflen, optname, numbuf, strlen(numbuf));
}

static int ifchd_cmd_ip(char *buf, size_t buflen, char *optname,
                        uint8_t *optdata, ssize_t optlen)
{
    if (!optdata || optlen < 4)
        return -1;
    char ipbuf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, optdata, ipbuf, sizeof ipbuf)) {
        log_warning("%s: (%s) inet_ntop failed: %s",
                    client_config.interface, __func__, strerror(errno));
        return -1;
    }
    return ifcmd_raw(buf, buflen, optname, ipbuf, strlen(ipbuf));
}

static int ifchd_cmd_iplist(char *out, size_t outlen, char *optname,
                            uint8_t *optdata, ssize_t optlen)
{
    char buf[2048];
    char ipbuf[INET_ADDRSTRLEN];
    size_t bufoff = 0;
    size_t optoff = 0;

    if (!optdata || optlen < 4)
        return -1;

    if (!inet_ntop(AF_INET, optdata + optoff, ipbuf, sizeof ipbuf)) {
        log_warning("%s: (%s) inet_ntop failed: %s",
                    client_config.interface, __func__, strerror(errno));
        return -1;
    }
    ssize_t wc = snprintf(buf + bufoff, sizeof buf, "%s:%s", optname, ipbuf);
    if (wc < 0 || (size_t)wc >= sizeof buf)
        return -1;
    optoff += 4;
    bufoff += wc;

    while (optlen - optoff >= 4) {
        if (!inet_ntop(AF_INET, optdata + optoff, ipbuf, sizeof ipbuf)) {
            log_warning("%s: (%s) inet_ntop failed: %s",
                        client_config.interface, __func__, strerror(errno));
            return -1;
        }
        wc = snprintf(buf + bufoff, sizeof buf, ",%s", ipbuf);
        if (wc < 0 || (size_t)wc >= sizeof buf)
            return -1;
        optoff += 4;
        bufoff += wc;
    }

    wc = snprintf(buf + bufoff, sizeof buf, ";");
    if (wc < 0 || (size_t)wc >= sizeof buf)
        return -1;
    return ifcmd_raw(out, outlen, optname, buf, strlen(buf));
}

#define CMD_ROUTER        "routr"
#define CMD_IP4SET        "ip4"
#define CMD_DNS           "dns"
#define CMD_LPRSVR        "lpr"
#define CMD_NTPSVR        "ntp"
#define CMD_WINS          "wins"
#define CMD_HOSTNAME      "host"
#define CMD_DOMAIN        "dom"
#define CMD_TIMEZONE      "tzone"
#define CMD_MTU           "mtu"
#define CMD_IPTTL         "ipttl"
#define CMD_NULL          "NULL"
#define IFCHD_SW_CMD(x, y) case DCODE_##x: \
                           optname = CMD_##x; \
                           dofn = ifchd_cmd_##y; \
                           break
static int ifchd_cmd(char *buf, size_t buflen, uint8_t *optdata,
                     ssize_t optlen, uint8_t code)
{
    int (*dofn)(char *, size_t, char *, uint8_t *, ssize_t);
    char *optname;
    switch (code) {
        IFCHD_SW_CMD(ROUTER, ip);
        IFCHD_SW_CMD(DNS, iplist);
        IFCHD_SW_CMD(LPRSVR, iplist);
        IFCHD_SW_CMD(NTPSVR, iplist);
        IFCHD_SW_CMD(WINS, iplist);
        IFCHD_SW_CMD(HOSTNAME, bytes);
        IFCHD_SW_CMD(DOMAIN, bytes);
        IFCHD_SW_CMD(TIMEZONE, s32);
        IFCHD_SW_CMD(MTU, u16);
        IFCHD_SW_CMD(IPTTL, u8);
    default:
        log_line("Invalid option code (%c) for ifchd cmd.", code);
        return -1;
    }
    return dofn(buf, buflen, optname, optdata, optlen);
}
#undef IFCHD_SW_CMD

static void pipewrite(struct client_state_t *cs, const char *buf, size_t count)
{
    cs->ifchWorking = 1;
    if (safe_write(pToIfchW, buf, count) == -1) {
        log_error("pipewrite: write failed: %s", strerror(errno));
        return;
    }
    log_line("Sent to ifchd: %s", buf);
}

void ifchange_deconfig(struct client_state_t *cs)
{
    char buf[256];

    if (cs->ifDeconfig)
        return;
    cs->ifDeconfig = 1;

    snprintf(buf, sizeof buf, "ip4:0.0.0.0,255.255.255.255;");
    log_line("Resetting %s IP configuration.", client_config.interface);
    pipewrite(cs, buf, strlen(buf));

    memset(&cfg_packet, 0, sizeof cfg_packet);
}

static size_t send_client_ip(char *out, size_t olen, struct dhcpmsg *packet)
{
    uint8_t optdata[MAX_DOPT_SIZE], olddata[MAX_DOPT_SIZE];
    char ipb[4*INET_ADDRSTRLEN], ip[INET_ADDRSTRLEN], sn[INET_ADDRSTRLEN],
        bc[INET_ADDRSTRLEN];
    ssize_t optlen, oldlen;
    bool change_ipaddr = false;
    bool have_subnet = false;
    bool change_subnet = false;
    bool have_bcast = false;
    bool change_bcast = false;

    if (memcmp(&packet->yiaddr, &cfg_packet.yiaddr, sizeof packet->yiaddr))
        change_ipaddr = true;
    inet_ntop(AF_INET, &packet->yiaddr, ip, sizeof ip);

    optlen = get_dhcp_opt(packet, DCODE_SUBNET, optdata, sizeof optdata);
    if (optlen >= 4) {
        have_subnet = true;
        inet_ntop(AF_INET, optdata, sn, sizeof sn);
        oldlen = get_dhcp_opt(&cfg_packet, DCODE_SUBNET, olddata,
                              sizeof olddata);
        if (oldlen != optlen || memcmp(optdata, olddata, optlen))
            change_subnet = true;
    }

    optlen = get_dhcp_opt(packet, DCODE_BROADCAST, optdata, sizeof optdata);
    if (optlen >= 4) {
        have_bcast = true;
        inet_ntop(AF_INET, optdata, bc, sizeof bc);
        oldlen = get_dhcp_opt(&cfg_packet, DCODE_BROADCAST, olddata,
                              sizeof olddata);
        if (oldlen != optlen || memcmp(optdata, olddata, optlen))
            change_bcast = true;
    }

    // Nothing to change.
    if (!change_ipaddr && !change_subnet && !change_bcast)
        return 0;

    if (!have_subnet) {
        static char snClassC[] = "255.255.255.0";
        log_line("Server did not send a subnet mask.  Assuming class C (255.255.255.0).");
        memcpy(sn, snClassC, sizeof snClassC);
    }

    if (have_bcast) {
        snprintf(ipb, sizeof ipb, "ip4:%s,%s,%s;", ip, sn, bc);
    } else {
        snprintf(ipb, sizeof ipb, "ip4:%s,%s;", ip, sn);
    }

    strnkcat(out, ipb, olen);
    return strlen(ipb);
}

static size_t send_cmd(char *out, size_t olen, struct dhcpmsg *packet,
                       uint8_t code)
{
    char buf[2048];
    uint8_t optdata[MAX_DOPT_SIZE], olddata[MAX_DOPT_SIZE];
    ssize_t optlen, oldlen;

    if (!packet)
        return 0;

    memset(buf, '\0', sizeof buf);
    optlen = get_dhcp_opt(packet, code, optdata, sizeof optdata);
    if (!optlen)
        return 0;
    oldlen = get_dhcp_opt(&cfg_packet, code, olddata, sizeof olddata);
    if (oldlen == optlen && !memcmp(optdata, olddata, optlen))
        return 0;
    int r = ifchd_cmd(buf, sizeof buf, optdata, optlen, code);
    if (r == -1)
        return 0;
    else if (r < -1) {
        log_warning("Error happened generating ifch cmd string.");
        return 0;
    }
    strnkcat(out, buf, olen);
    return strlen(buf);
}

void ifchange_bind(struct client_state_t *cs, struct dhcpmsg *packet)
{
    char buf[2048];
    int tbs = 0;

    if (!packet)
        return;

    memset(buf, 0, sizeof buf);
    tbs |= send_client_ip(buf, sizeof buf, packet);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_ROUTER);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_DNS);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_HOSTNAME);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_DOMAIN);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_MTU);
    tbs |= send_cmd(buf, sizeof buf, packet, DCODE_WINS);
    if (tbs)
        pipewrite(cs, buf, strlen(buf));

    cs->ifDeconfig = 0;
    memcpy(&cfg_packet, packet, sizeof cfg_packet);
}
