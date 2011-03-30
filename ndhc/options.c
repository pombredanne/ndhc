/*
 * options.c -- DHCP server option packet tools
 * Rewrite by Russ Dill <Russ.Dill@asu.edu> July 2001
 * Fixes and hardening: Nicholas J. Kain <njkain at gmail dot com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"

#include "log.h"
#include "malloc.h"

enum {
    OPT_CODE = 0,
    OPT_LEN  = 1,
    OPT_DATA = 2
};

/* supported options are easily added here */
struct dhcp_option options[] = {
    /* name[10]     flags                                   code */
    {"subnet"   ,   OPTION_IP,                              0x01},
    {"timezone" ,   OPTION_S32,                             0x02},
    {"router"   ,   OPTION_IP,                              0x03},
    {"timesvr"  ,   OPTION_IP,                              0x04},
    {"namesvr"  ,   OPTION_IP,                              0x05},
    {"dns"      ,   OPTION_IP,                              0x06},
    {"logsvr"   ,   OPTION_IP,                              0x07},
    {"cookiesvr",   OPTION_IP,                              0x08},
    {"lprsvr"   ,   OPTION_IP,                              0x09},
    {"hostname" ,   OPTION_STRING,                          0x0c},
    {"bootsize" ,   OPTION_U16,                             0x0d},
    {"domain"   ,   OPTION_STRING,                          0x0f},
    {"swapsvr"  ,   OPTION_IP,                              0x10},
    {"rootpath" ,   OPTION_STRING,                          0x11},
    {"ipttl"    ,   OPTION_U8,                              0x17},
    {"mtu"      ,   OPTION_U16,                             0x1a},
    {"broadcast",   OPTION_IP,                              0x1c},
    {"ntpsrv"   ,   OPTION_IP,                              0x2a},
    {"wins"     ,   OPTION_IP,                              0x2c},
    {"requestip",   OPTION_IP,                              0x32},
    {"lease"    ,   OPTION_U32,                             0x33},
    {"dhcptype" ,   OPTION_U8,                              0x35},
    {"serverid" ,   OPTION_IP,                              0x36},
    {"message"  ,   OPTION_STRING,                          0x38},
    {"maxsize"  ,   OPTION_U16,                             0x39},
    {"tftp"     ,   OPTION_STRING,                          0x42},
    {"bootfile" ,   OPTION_STRING,                          0x43},
    {""         ,   0x00,                                   0x00}
};

/* Lengths of the different option types */
int option_lengths[] = {
	[OPTION_IP] =       4,
	[OPTION_IP_PAIR] =  8,
	[OPTION_BOOLEAN] =  1,
	[OPTION_STRING] =   1,
	[OPTION_U8] =       1,
	[OPTION_U16] =      2,
	[OPTION_S16] =      2,
	[OPTION_U32] =      4,
	[OPTION_S32] =      4
};

size_t sizeof_option(unsigned char code, size_t datalen)
{
	if (code == DHCP_PADDING || code == DHCP_END)
		return 1;
	return 2 + datalen;
}

// optdata can be NULL
size_t set_option(unsigned char *buf, size_t buflen, unsigned char code,
				  unsigned char *optdata, size_t datalen)
{
	if (!optdata)
		datalen = 0;
	if (code == DHCP_PADDING || code == DHCP_END) {
		if (buflen < 1)
			return 0;
		buf[0] = code;
		return 1;
	}

	if (datalen > 255 || buflen < 2 + datalen)
		return 0;
	buf[0] = code;
	buf[1] = datalen;
	memcpy(buf + 2, optdata, datalen);
	return 2 + datalen;
}

unsigned char *alloc_option(unsigned char code, unsigned char *optdata,
							size_t datalen)
{
	unsigned char *ret;
	size_t len = sizeof_option(code, datalen);
	ret = xmalloc(len);
	set_option(ret, len, code, optdata, datalen);
	return ret;
}

// This is tricky -- the data must be prefixed by one byte indicating the
// type of ARP MAC address (1 for ethernet) or 0 for a purely symbolic
// identifier.
unsigned char *alloc_dhcp_client_id_option(unsigned char type,
										   unsigned char *idstr, size_t idstrlen)
{
	unsigned char data[idstrlen + 1];
	data[0] = type;
	memcpy(data + 1, idstr, idstrlen);
	return alloc_option(DHCP_CLIENT_ID, data, sizeof data);
}

/* Get an option with bounds checking (warning, result is not aligned) */
uint8_t* get_option(struct dhcpMessage *packet, int code)
{
	uint8_t *optionptr;
	int len, rem, overload = 0;
	enum {
		OPTION_FIELD = 0,
		FILE_FIELD	 = 1,
		SNAME_FIELD	 = 2
	};
	enum {
		FILE_FIELD101  = FILE_FIELD  * 0x101,
		SNAME_FIELD101 = SNAME_FIELD * 0x101,
	};

	/* option bytes: [code][len][data1][data2]..[dataLEN] */
	optionptr = packet->options;
	rem = sizeof packet->options;
	while (1) {
		if (rem <= 0) {
			log_warning("Bad packet, malformed option field.");
			return NULL;
		}
		if (optionptr[OPT_CODE] == DHCP_PADDING) {
			rem--;
			optionptr++;
			continue;
		}
		if (optionptr[OPT_CODE] == DHCP_END) {
			if ((overload & FILE_FIELD101) == FILE_FIELD) {
				/* can use packet->file, and didn't look at it yet */
				overload |= FILE_FIELD101; /* "we looked at it" */
				optionptr = packet->file;
				rem = sizeof packet->file;
				continue;
			}
			if ((overload & SNAME_FIELD101) == SNAME_FIELD) {
				/* can use packet->sname, and didn't look at it yet */
				overload |= SNAME_FIELD101; /* "we looked at it" */
				optionptr = packet->sname;
				rem = sizeof packet->sname;
				continue;
			}
			break;
		}
		len = 2 + optionptr[OPT_LEN];
		rem -= len;
		if (rem < 0)
			continue; /* complain and return NULL */

		if (optionptr[OPT_CODE] == code)
			return optionptr + OPT_DATA;

		if (optionptr[OPT_CODE] == DHCP_OPTION_OVERLOAD) {
			overload |= optionptr[OPT_DATA];
			/* fall through */
		}
		optionptr += len;
	}
	return NULL;
}

/* return the position of the 'end' option */
int end_option(uint8_t *optionptr)
{
	int i = 0;

	while (i < DHCP_OPTIONS_BUFSIZE && optionptr[i] != DHCP_END) {
		if (optionptr[i] != DHCP_PADDING)
			i += optionptr[i + OPT_LEN] + OPT_DATA - 1;
		i++;
	}
	return (i < DHCP_OPTIONS_BUFSIZE - 1 ? i : DHCP_OPTIONS_BUFSIZE - 1);
}


/* add an option string to the options (an option string contains an option
 * code, length, then data) */
int add_option_string(unsigned char *optionptr, unsigned char *string)
{
	int end = end_option(optionptr);

	/* end position + string length + option code/length + end option */
	if (end + string[OPT_LEN] + 2 + 1 >= DHCP_OPTIONS_BUFSIZE) {
		log_error("Option 0x%02x did not fit into the packet!",
				  string[OPT_CODE]);
		return 0;
	}
	memcpy(optionptr + end, string, string[OPT_LEN] + 2);
	optionptr[end + string[OPT_LEN] + 2] = DHCP_END;
	return string[OPT_LEN] + 2;
}

int add_simple_option(unsigned char *optionptr, unsigned char code,
					  uint32_t data)
{
	int i, length = 0;
	unsigned char option[2 + 4];

	for (i = 0; options[i].code; i++)
		if (options[i].code == code) {
			length = option_lengths[options[i].flags & TYPE_MASK];
		}

	log_line("aso(): code=0x%02x length=0x%02x", code, length);
	option[OPT_CODE] = code;
	option[OPT_LEN] = (unsigned char)length;

	if (!length) {
		log_error("Could not add option 0x%02x", code);
		return 0;
	} else if (length == 1) {
		uint8_t t = (uint8_t)data;
		memcpy(option + 2, &t, 1);
	} else if (length == 2) {
		uint16_t t = (uint16_t)data;
		memcpy(option + 2, &t, 2);
	} else if (length == 4) {
		uint32_t t = (uint32_t)data;
		memcpy(option + 2, &t, 4);
	}
	return add_option_string(optionptr, option);
}

/* find option 'code' in opt_list */
struct option_set *find_option(struct option_set *opt_list, char code)
{
	while (opt_list && opt_list->data[OPT_CODE] < code)
		opt_list = opt_list->next;

	if (opt_list && opt_list->data[OPT_CODE] == code)
		return opt_list;
	return NULL;
}

// List of options that will be sent on the parameter request list to the
// remote DHCP server.
static unsigned char req_opts[] = {
	DHCP_SUBNET,
	DHCP_ROUTER,
	DHCP_DNS_SERVER,
	DHCP_HOST_NAME,
	DHCP_DOMAIN_NAME,
	DHCP_BROADCAST,
	0x00
};

/* Add a paramater request list for stubborn DHCP servers.  Don't do bounds */
/* checking here because it goes towards the head of the packet. */
void add_requests(struct dhcpMessage *packet)
{
    int end = end_option(packet->options);
    int i, len = 0;

    packet->options[end + OPT_CODE] = DHCP_PARAM_REQ;
    for (i = 0; req_opts[i]; i++)
		packet->options[end + OPT_DATA + len++] = req_opts[i];
    packet->options[end + OPT_LEN] = len;
    packet->options[end + OPT_DATA + len] = DHCP_END;
}
