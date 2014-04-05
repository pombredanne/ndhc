/* ndhc.c - DHCP client
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/prctl.h>
#include <net/if.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include "nk/log.h"
#include "nk/privilege.h"
#include "nk/pidfile.h"
#include "nk/io.h"
#include "nk/copy_cmdarg.h"

#include "ndhc.h"
#include "ndhc-defines.h"
#include "seccomp.h"
#include "state.h"
#include "options.h"
#include "dhcp.h"
#include "sys.h"
#include "ifchange.h"
#include "arp.h"
#include "nl.h"
#include "netlink.h"
#include "leasefile.h"
#include "ifset.h"
#include "ifchd.h"
#include "duiaid.h"
#include "sockd.h"

struct client_state_t cs = {
    .ifchWorking = 0,
    .ifDeconfig = 0,
    .init = 1,
    .epollFd = -1,
    .signalFd = -1,
    .listenFd = -1,
    .arpFd = -1,
    .nlFd = -1,
    .nlPortId = -1,
    .routerArp = "\0\0\0\0\0\0",
    .serverArp = "\0\0\0\0\0\0",
};

struct client_config_t client_config = {
    .interface = "eth0",
    .arp = "\0\0\0\0\0\0",
    .clientid_len = 0,
    .metric = 0,
    .foreground = 1,
};

static void show_usage(void)
{
    printf(
"ndhc " NDHC_VERSION ", dhcp client.  Licensed under 2-clause BSD.\n"
"Copyright (C) 2004-2014 Nicholas J. Kain\n"
"Usage: ndhc [OPTIONS]\n\n"
"  -c, --clientid=CLIENTID         Client identifier\n"
"  -h, --hostname=HOSTNAME         Client hostname\n"
"  -V, --vendorid=VENDORID         Client vendor identification string\n"
"  -b, --background                Fork to background if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -p, --pidfile=FILE              File where the ndhc pid will be written\n"
"  -P, --ifch-pidfile=FILE         File where the ndhc-ifch pid will be written\n"
"  -i, --interface=INTERFACE       Interface to use (default: eth0)\n"
"  -n, --now                       Exit with failure if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -q, --quit                      Quit after obtaining lease\n"
"  -r, --request=IP                IP address to request (default: none)\n"
"  -u, --user=USER                 Change ndhc privileges to this user\n"
"  -U, --ifch-user=USER            Change ndhc-ifch privileges to this user\n"
"  -C, --chroot=DIR                Chroot to this directory\n"
"  -s, --state-dir=DIR             State storage dir (default: /etc/ndhc)\n"
#ifdef ENABLE_SECCOMP_FILTER
"  -S, --seccomp-enforce           Enforce seccomp syscall restrictions\n"
#endif
"  -d, --relentless-defense        Never back off in defending IP against\n"
"                                  conflicting hosts (servers only)\n"
"  -w, --arp-probe-wait            Time to delay before first ARP probe\n"
"  -W, --arp-probe-num             Number of ARP probes before lease is ok\n"
"  -m, --arp-probe-min             Min ms to wait for ARP response\n"
"  -M, --arp-probe-max             Max ms to wait for ARP response\n"
"  -t, --gw-metric                 Route metric for default gw (default: 0)\n"
"  -R, --resolve-conf=FILE         Path to resolv.conf or equivalent\n"
"  -H, --dhcp-hostname             Allow DHCP to set machine hostname\n"
"  -v, --version                   Display version\n"
           );
    exit(EXIT_SUCCESS);
}

static void setup_signals_ndhc(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        suicide("sigprocmask failed");
    if (cs.signalFd >= 0) {
        epoll_del(cs.epollFd, cs.signalFd);
        close(cs.signalFd);
    }
    cs.signalFd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (cs.signalFd < 0)
        suicide("signalfd failed");
    epoll_add(cs.epollFd, cs.signalFd);
}

static void signal_dispatch(void)
{
    int t;
    size_t off = 0;
    struct signalfd_siginfo si = {0};
  again:
    t = read(cs.signalFd, (char *)&si + off, sizeof si - off);
    if (t < 0) {
        if (t == EAGAIN || t == EWOULDBLOCK || t == EINTR)
            goto again;
        else
            suicide("signalfd read error");
    }
    if (off + (unsigned)t < sizeof si)
        off += t;
    switch (si.ssi_signo) {
        case SIGUSR1:
            force_renew_action(&cs);
            break;
        case SIGUSR2:
            force_release_action(&cs);
            break;
        case SIGPIPE:
            log_line("ndhc-master: IPC pipe closed.  Exiting.");
            exit(EXIT_SUCCESS);
            break;
        case SIGCHLD:
            suicide("ndhc-master: Subprocess terminated unexpectedly.  Exiting.");
            break;
        case SIGTERM:
            log_line("Received SIGTERM.  Exiting gracefully.");
            exit(EXIT_SUCCESS);
            break;
        default:
            break;
    }
}

static int is_string_hwaddr(char *str, size_t slen)
{
    if (slen == 17 && str[2] == ':' && str[5] == ':' && str[8] == ':' &&
        str[11] == ':' && str[14] == ':' &&
        isxdigit(str[0]) && isxdigit(str[1]) && isxdigit(str[3]) &&
        isxdigit(str[4]) && isxdigit(str[6]) && isxdigit(str[7]) &&
        isxdigit(str[9]) && isxdigit(str[10]) && isxdigit(str[12]) &&
        isxdigit(str[13]) && isxdigit(str[15]) && isxdigit(str[16])
        )
        return 1;
    return 0;
}

static int get_clientid_string(char *str, size_t slen)
{
    if (!slen)
        return -1;
    if (!is_string_hwaddr(str, slen)) {
        client_config.clientid[0] = 0;
        memcpy(&client_config.clientid + 1, str,
               min_size_t(slen, sizeof client_config.clientid - 1));
        client_config.clientid_len = slen + 1;
        return 0;
    }

    uint8_t mac[6];
    for (size_t i = 0; i < sizeof mac; ++i)
        mac[i] = strtol(str+i*3, NULL, 16);
    client_config.clientid[0] = 1; // Ethernet MAC type
    memcpy(&client_config.clientid + 1, mac, sizeof mac);
    client_config.clientid_len = 7;
    return 1;
}

static void fail_if_state_dir_dne(void)
{
    if (strlen(state_dir) == 0)
        suicide("state_dir path is empty; it must be specified");
    struct stat st;
    if (stat(state_dir, &st) < 0)
        suicide("failed to stat state_dir path '%s': %s",
                state_dir, strerror(errno));
    if (!S_ISDIR(st.st_mode))
        suicide("state_dir path '%s' does not specify a directory", state_dir);
}

static void handle_ifch_message(void)
{
    char c;
    int r = safe_read(pToNdhcR, &c, sizeof c);
    if (r == 0) {
        // Remote end hung up.
        exit(EXIT_SUCCESS);
    } else if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        suicide("%s: (%s) error reading from ifch -> ndhc pipe: %s",
                client_config.interface, __func__, strerror(errno));
    }

    if (c == '+')
        cs.ifchWorking = 0;
}

#define NDHC_NUM_EP_FDS 4
static void do_ndhc_work(void)
{
    struct epoll_event events[NDHC_NUM_EP_FDS];
    long long nowts;
    int timeout;

    cs.epollFd = epoll_create1(0);
    if (cs.epollFd == -1)
        suicide("epoll_create1 failed");

    if (enforce_seccomp_ndhc())
        log_line("ndhc seccomp filter cannot be installed");

    setup_signals_ndhc();

    epoll_add(cs.epollFd, cs.nlFd);
    epoll_add(cs.epollFd, pToNdhcR);
    set_listen_raw(&cs);
    nowts = curms();
    goto jumpstart;

    for (;;) {
        int r = epoll_wait(cs.epollFd, events, NDHC_NUM_EP_FDS, timeout);
        if (r == -1) {
            if (errno == EINTR)
                continue;
            else
                suicide("epoll_wait failed");
        }
        for (int i = 0; i < r; ++i) {
            int fd = events[i].data.fd;
            if (fd == cs.signalFd)
                signal_dispatch();
            else if (fd == cs.listenFd)
                handle_packet(&cs);
            else if (fd == cs.arpFd)
                handle_arp_response(&cs);
            else if (fd == cs.nlFd)
                handle_nl_message(&cs);
            else if (fd == pToNdhcR)
                handle_ifch_message();
            else
                suicide("epoll_wait: unknown fd");
        }

        for (;;) {
            nowts = curms();
            long long arp_wake_ts = arp_get_wake_ts();
            long long dhcp_wake_ts = dhcp_get_wake_ts();
            if (arp_wake_ts == -1) {
                if (dhcp_wake_ts != -1) {
                    timeout = dhcp_wake_ts - nowts;
                    if (timeout < 0)
                        timeout = 0;
                } else
                    timeout = -1;
            } else {
                // If dhcp_wake_ts is -1 then we want to sleep anyway.
                timeout = (arp_wake_ts < dhcp_wake_ts ?
                           arp_wake_ts : dhcp_wake_ts) - nowts;
                if (timeout < 0)
                    timeout = 0;
            }

            if (!timeout) {
jumpstart:
                timeout_action(&cs, nowts);
            } else
                break;
        }
    }
}

char state_dir[PATH_MAX] = "/etc/ndhc";
char chroot_dir[PATH_MAX] = "";
char resolv_conf_d[PATH_MAX] = "";
static char pidfile[PATH_MAX] = PID_FILE_DEFAULT;
static uid_t ndhc_uid = 0;
static gid_t ndhc_gid = 0;

int pToNdhcR;
int pToNdhcW;
int pToIfchR;
int pToIfchW;

int psToNdhcR;
int psToNdhcW;
int pToSockdR;
int pToSockdW;

static void create_ifch_ipc_pipes(void) {
    int niPipe[2];
    int inPipe[2];

    if (pipe2(niPipe, 0))
        suicide("FATAL - can't create ndhc -> ndhc-ifch pipe: %s",
                strerror(errno));
    if (fcntl(niPipe[0], F_SETFL, fcntl(niPipe[0], F_GETFL) | O_NONBLOCK) < 0)
        suicide("FATAL - failed to set ndhc -> ndhc-ifch read-side nonblocking: %s",
                strerror(errno));
    pToNdhcR = niPipe[0];
    pToNdhcW = niPipe[1];
    if (pipe2(inPipe, 0))
        suicide("FATAL - can't create ndhc-ifch -> ndhc pipe: %s",
                strerror(errno));
    if (fcntl(inPipe[0], F_SETFL, fcntl(inPipe[0], F_GETFL) | O_NONBLOCK) < 0)
        suicide("FATAL - failed to set ndhc-ifch -> ndhc read-side nonblocking: %s",
                strerror(errno));
    pToIfchR = inPipe[0];
    pToIfchW = inPipe[1];
}

static void create_sockd_ipc_pipes(void) {
    int nsPipe[2];
    int snPipe[2];

    if (pipe2(nsPipe, 0))
        suicide("FATAL - can't create ndhc -> ndhc-sockd pipe: %s",
                strerror(errno));
    psToNdhcR = nsPipe[0];
    psToNdhcW = nsPipe[1];
    if (pipe2(snPipe, 0))
        suicide("FATAL - can't create ndhc-sockd -> ndhc pipe: %s",
                strerror(errno));
    if (fcntl(snPipe[0], F_SETFL, fcntl(snPipe[0], F_GETFL) | O_NONBLOCK) < 0)
        suicide("FATAL - failed to set ndhc-sockd -> ndhc read-side nonblocking: %s",
                strerror(errno));
    pToSockdR = snPipe[0];
    pToSockdW = snPipe[1];
}

static void spawn_ifch(void)
{
    create_ifch_ipc_pipes();
    pid_t ifch_pid = fork();
    if (ifch_pid == 0) {
        close(pToNdhcR);
        close(pToIfchW);
        // Don't share the RNG state with the master process.
        nk_random_u32_init(&cs.rnd32_state);
        ifch_main();
    } else if (ifch_pid > 0) {
        close(pToIfchR);
        close(pToNdhcW);
    } else
        suicide("failed to fork ndhc-ifch: %s", strerror(errno));
}

static void spawn_sockd(void)
{
    create_sockd_ipc_pipes();
    pid_t sockd_pid = fork();
    if (sockd_pid == 0) {
        close(psToNdhcR);
        close(pToSockdW);
        // Don't share the RNG state with the master process.
        nk_random_u32_init(&cs.rnd32_state);
        sockd_main();
    } else if (sockd_pid > 0) {
        close(pToSockdR);
        close(psToNdhcW);
    } else
        suicide("failed to fork ndhc-sockd: %s", strerror(errno));
}

static void ndhc_main(void) {
    prctl(PR_SET_NAME, "ndhc: master");
    log_line("ndhc client " NDHC_VERSION " started on interface [%s].",
             client_config.interface);

    if ((cs.nlFd = nl_open(NETLINK_ROUTE, RTMGRP_LINK, &cs.nlPortId)) < 0)
        suicide("%s: failed to open netlink socket", __func__);

    if (client_config.foreground && !client_config.background_if_no_lease) {
        if (file_exists(pidfile, "w") == -1)
            suicide("%s: can't open pidfile '%s' for write!",
                    __func__, pidfile);
        write_pid(pidfile);
    }

    open_leasefile();

    nk_set_chroot(chroot_dir);
    memset(chroot_dir, '\0', sizeof chroot_dir);
    nk_set_uidgid(ndhc_uid, ndhc_gid, NULL);

    if (cs.ifsPrevState != IFS_UP)
        ifchange_deconfig(&cs);

    do_ndhc_work();
}

void background(void)
{
    static char called;
    if (!called) {
        called = 1;  // Do not fork again.
        if (daemon(0, 0) == -1) {
            perror("fork");
            exit(EXIT_SUCCESS);
        }
    }
    if (file_exists(pidfile, "w") == -1) {
        log_warning("Cannot open pidfile for write!");
    } else
        write_pid(pidfile);
}

static void parse_program_options(int argc, char *argv[])
{
    static const struct option arg_options[] = {
        {"clientid",           required_argument,  0, 'c'},
        {"background",         no_argument,        0, 'b'},
        {"pidfile",            required_argument,  0, 'p'},
        {"ifch-pidfile",       required_argument,  0, 'P'},
        {"hostname",           required_argument,  0, 'h'},
        {"interface",          required_argument,  0, 'i'},
        {"now",                no_argument,        0, 'n'},
        {"quit",               no_argument,        0, 'q'},
        {"request",            required_argument,  0, 'r'},
        {"vendorid",           required_argument,  0, 'V'},
        {"user",               required_argument,  0, 'u'},
        {"ifch-user",          required_argument,  0, 'U'},
        {"sockd-user",         required_argument,  0, 'D'},
        {"chroot",             required_argument,  0, 'C'},
        {"state-dir",          required_argument,  0, 's'},
        {"seccomp-enforce",    no_argument,        0, 'S'},
        {"relentless-defense", no_argument,        0, 'd'},
        {"arp-probe-wait",     required_argument,  0, 'w'},
        {"arp-probe-num",      required_argument,  0, 'W'},
        {"arp-probe-min",      required_argument,  0, 'm'},
        {"arp-probe-max",      required_argument,  0, 'M'},
        {"gw-metric",          required_argument,  0, 't'},
        {"resolv-conf",        required_argument,  0, 'R'},
        {"dhcp-set-hostname",  no_argument,        0, 'H'},
        {"version",            no_argument,        0, 'v'},
        {"help",               no_argument,        0, '?'},
        {0, 0, 0, 0}
    };

    while (1) {
        int c;
        c = getopt_long(argc, argv, "c:bp:P:h:i:nqr:V:u:U:D:C:s:Sdw:W:m:M:t:R:Hv?",
                        arg_options, NULL);
        if (c == -1) break;

        switch (c) {
            case 'c':
                get_clientid_string(optarg, strlen(optarg));
                break;
            case 'b':
                client_config.background_if_no_lease = 1;
                gflags_detach = 1;
                break;
            case 'p':
                copy_cmdarg(pidfile, optarg, sizeof pidfile, "pidfile");
                break;
            case 'P':
                copy_cmdarg(pidfile_ifch, optarg, sizeof pidfile_ifch,
                            "ifch-pidfile");
                break;
            case 'h':
                copy_cmdarg(client_config.hostname, optarg,
                            sizeof client_config.hostname, "hostname");
                break;
            case 'i':
                copy_cmdarg(client_config.interface, optarg,
                            sizeof client_config.interface, "interface");
                break;
            case 'n':
                client_config.abort_if_no_lease = 1;
                break;
            case 'q':
                client_config.quit_after_lease = 1;
                break;
            case 'r':
                cs.clientAddr = inet_addr(optarg);
                break;
            case 'u':
                if (nk_uidgidbyname(optarg, &ndhc_uid, &ndhc_gid))
                    suicide("invalid ndhc user '%s' specified", optarg);
                break;
            case 'U':
                if (nk_uidgidbyname(optarg, &ifch_uid, &ifch_gid))
                    suicide("invalid ifch user '%s' specified", optarg);
                break;
            case 'D':
                if (nk_uidgidbyname(optarg, &sockd_uid, &sockd_gid))
                    suicide("invalid sockd user '%s' specified", optarg);
                break;
            case 'C':
                copy_cmdarg(chroot_dir, optarg, sizeof chroot_dir, "chroot");
                break;
            case 's':
                copy_cmdarg(state_dir, optarg, sizeof state_dir, "state-dir");
                break;
            case 'S':
                seccomp_enforce = true;
                break;
            case 'd':
                arp_relentless_def = 1;
                break;
            case 'w':
            case 'W': {
                int t = atoi(optarg);
                if (t < 0)
                    break;
                if (c == 'w')
                    arp_probe_wait = t;
                else
                    arp_probe_num = t;
                break;
            }
            case 'm':
            case 'M': {
                int t = atoi(optarg);
                if (c == 'm')
                    arp_probe_min = t;
                else
                    arp_probe_max = t;
                if (arp_probe_min > arp_probe_max) {
                    t = arp_probe_max;
                    arp_probe_max = arp_probe_min;
                    arp_probe_min = t;
                }
                break;
            }
            case 'v':
                printf("ndhc %s, dhcp client.\n", NDHC_VERSION);
                printf("Copyright (c) 2004-2014 Nicholas J. Kain\n"
                       "All rights reserved.\n\n"
                       "Redistribution and use in source and binary forms, with or without\n"
                       "modification, are permitted provided that the following conditions are met:\n\n"
                       "- Redistributions of source code must retain the above copyright notice,\n"
                       "  this list of conditions and the following disclaimer.\n"
                       "- Redistributions in binary form must reproduce the above copyright notice,\n"
                       "  this list of conditions and the following disclaimer in the documentation\n"
                       "  and/or other materials provided with the distribution.\n\n"
                       "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
                       "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
                       "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\n"
                       "ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE\n"
                       "LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\n"
                       "CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
                       "SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\n"
                       "INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\n"
                       "CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\n"
                       "ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\n"
                       "POSSIBILITY OF SUCH DAMAGE.\n");
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                copy_cmdarg(client_config.vendor, optarg,
                            sizeof client_config.vendor, "vendorid");
                break;
            case 't': {
                char *p;
                long mt = strtol(optarg, &p, 10);
                if (p == optarg)
                    suicide("gw-metric arg '%s' isn't a valid number", optarg);
                if (mt > INT_MAX)
                    suicide("gw-metric arg '%s' is too large", optarg);
                if (mt < 0)
                    mt = 0;
                client_config.metric = (int)mt;
                break;
            }
            case 'R':
                copy_cmdarg(resolv_conf_d, optarg, sizeof resolv_conf_d,
                            "resolv-conf");
                break;
            case 'H':
                allow_hostname = 1;
                break;
            default:
                show_usage();
        }
    }
}

int main(int argc, char *argv[])
{
    parse_program_options(argc, argv);

    nk_random_u32_init(&cs.rnd32_state);

    if (getuid())
        suicide("I need to be started as root.");
    if (!strncmp(chroot_dir, "", sizeof chroot_dir))
        suicide("No chroot path is specified.  Refusing to run.");
    fail_if_state_dir_dne();

    if (nl_getifdata() < 0)
        suicide("failed to get interface MAC or index");

    get_clientid(&cs, &client_config);

    switch (perform_ifup()) {
    case 1: cs.ifsPrevState = IFS_UP;
    case 0: break;
    default: suicide("failed to set the interface to up state");
    }

    spawn_ifch();
    spawn_sockd();
    ndhc_main();
    exit(EXIT_SUCCESS);
}

