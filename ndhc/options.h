/* options.h */
#ifndef OPTIONS_H_
#define OPTIONS_H_

#include "packet.h"

#define TYPE_MASK	0x0F

enum {
	OPTION_IP=1,
	OPTION_IP_PAIR,
	OPTION_STRING,
	OPTION_BOOLEAN,
	OPTION_U8,
	OPTION_U16,
	OPTION_S16,
	OPTION_U32,
	OPTION_S32
};

#define OPTION_REQ	0x10 /* have the client request this option */
#define OPTION_LIST	0x20 /* There can be a list of 1 or more of these */

struct dhcp_option {
	char name[10];
	char flags;
	unsigned char code;
};

extern struct dhcp_option options[];
extern int option_lengths[];

uint8_t *get_option(struct dhcpMessage *packet, int code);
int end_option(unsigned char *optionptr);
int add_option_string(unsigned char *optionptr, unsigned char *string);
int add_simple_option(unsigned char *optionptr, unsigned char code, uint32_t data);
struct option_set *find_option(struct option_set *opt_list, char code);

#endif
