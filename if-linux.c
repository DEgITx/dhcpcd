/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <asm/types.h> /* Needed for 2.4 kernels */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>

#include <errno.h>
#include <ctype.h>
#include <fnmatch.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Support older kernels */
#ifndef IFLA_WIRELESS
# define IFLA_WIRELSSS (IFLFA_MASTER + 1)
#endif

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "net.h"

#define BUFFERLEN 256

static void (*nl_carrier)(const char *) = NULL;
static void (*nl_add)(const char *) = NULL;
static void (*nl_remove)(const char *) = NULL;

int
open_link_socket(void)
{
	int fd;
	struct sockaddr_nl nl;

	if ((fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) == -1)
		return -1;
	memset(&nl, 0, sizeof(nl));
	nl.nl_family = AF_NETLINK;
	nl.nl_groups = RTMGRP_LINK;
	if (bind(fd, (struct sockaddr *)&nl, sizeof(nl)) == -1)
		return -1;
	set_cloexec(fd);
	return fd;
}

static int
get_netlink(int fd, int flags,
	    int (*callback)(struct nlmsghdr *))
{
	char *buffer = NULL;
	ssize_t bytes;
	struct nlmsghdr *nlm;
	int r = -1;

	buffer = xzalloc(sizeof(char) * BUFFERLEN);
	for (;;) {
		bytes = recv(fd, buffer, BUFFERLEN, flags);
		if (bytes == -1) {
			if (errno == EAGAIN) {
				r = 0;
				goto eexit;
			}
			if (errno == EINTR)
				continue;
			goto eexit;
		}
		for (nlm = (struct nlmsghdr *)buffer;
		     NLMSG_OK(nlm, (size_t)bytes);
		     nlm = NLMSG_NEXT(nlm, bytes))
		{
			r = callback(nlm);
			if (r != 0)
				goto eexit;
		}
	}

eexit:
	free(buffer);
	return r;
}

static int
err_netlink(struct nlmsghdr *nlm)
{
	struct nlmsgerr *err;
	int l;

	if (nlm->nlmsg_type != NLMSG_ERROR)
		return 0;
	l = nlm->nlmsg_len - sizeof(*nlm);
	if ((size_t)l < sizeof(*err)) {
		errno = EBADMSG;
		return -1;
	}
	err = (struct nlmsgerr *)NLMSG_DATA(nlm);
	if (err->error == 0)
		return l;
	errno = -err->error;
	return -1;
}

static int
link_netlink(struct nlmsghdr *nlm)
{
	int len;
	struct rtattr *rta;
	struct ifinfomsg *ifi;
	char ifn[IF_NAMESIZE + 1];

	if (nlm->nlmsg_type != RTM_NEWLINK && nlm->nlmsg_type != RTM_DELLINK)
		return 0;
	len = nlm->nlmsg_len - sizeof(*nlm);
	if ((size_t)len < sizeof(*ifi)) {
		errno = EBADMSG;
		return -1;
	}
	ifi = NLMSG_DATA(nlm);
	if (ifi->ifi_flags & IFF_LOOPBACK)
		return 1;
	rta = (struct rtattr *) ((char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
	len = NLMSG_PAYLOAD(nlm, sizeof(*ifi));
	*ifn = '\0';
	while (RTA_OK(rta, len)) {
		switch (rta->rta_type) {
		case IFLA_WIRELESS:
			/* Ignore wireless messages */
			if (nlm->nlmsg_type == RTM_NEWLINK &&
			    ifi->ifi_change == 0)
				return 1;
			break;
		case IFLA_IFNAME:
			strlcpy(ifn, RTA_DATA(rta), sizeof(ifn));
			break;
		}
		rta = RTA_NEXT(rta, len);
	}
	if (nlm->nlmsg_type == RTM_NEWLINK) {
		if (ifi->ifi_change == ~0U) {
			if (nl_add)
				nl_add(ifn);
		} else {
			if (nl_carrier)
				nl_carrier(ifn);
		}
	} else {
		if (nl_remove)
			nl_remove(ifn);
	}
	return 1;
}

int
manage_link(int fd,
	    void (*if_carrier)(const char *),
	    void (*if_add)(const char *),
	    void (*if_remove)(const char *))
{
	nl_carrier = if_carrier;
	nl_add = if_add;
	nl_remove = if_remove;
	return get_netlink(fd, MSG_DONTWAIT, &link_netlink);
}

static int
send_netlink(struct nlmsghdr *hdr)
{
	int fd, r;
	struct sockaddr_nl nl;
	struct iovec iov;
	struct msghdr msg;
	static unsigned int seq;

	if ((fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) == -1)
		return -1;
	memset(&nl, 0, sizeof(nl));
	nl.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&nl, sizeof(nl)) == -1) {
		close(fd);
		return -1;
	}
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = hdr;
	iov.iov_len = hdr->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &nl;
	msg.msg_namelen = sizeof(nl);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	/* Request a reply */
	hdr->nlmsg_flags |= NLM_F_ACK;
	hdr->nlmsg_seq = ++seq;

	if (sendmsg(fd, &msg, 0) != -1)
		r = get_netlink(fd, 0, &err_netlink);
	else
		r = -1;
	close(fd);
	return r;
}

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *)(((ptrdiff_t)(nmsg))+NLMSG_ALIGN((nmsg)->nlmsg_len)))

static int
add_attr_l(struct nlmsghdr *n, unsigned int maxlen, int type,
	   const void *data, int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
		errno = ENOBUFS;
		return -1;
	}

	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

	return 0;
}

static int
add_attr_32(struct nlmsghdr *n, unsigned int maxlen, int type, uint32_t data)
{
	int len = RTA_LENGTH(sizeof(data));
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + len > maxlen) {
		errno = ENOBUFS;
		return -1;
	}

	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), &data, sizeof(data));
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + len;

	return 0;
}

struct nlma
{
	struct nlmsghdr hdr;
	struct ifaddrmsg ifa; 
	char buffer[64];
};

struct nlmr
{
	struct nlmsghdr hdr;
	struct rtmsg rt;
	char buffer[256];
};

int
if_address(const struct interface *iface,
	   const struct in_addr *address, const struct in_addr *netmask,
	   const struct in_addr *broadcast, int action)
{
	struct nlma *nlm;
	int retval = 0;

	nlm = xzalloc(sizeof(*nlm));
	nlm->hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	nlm->hdr.nlmsg_flags = NLM_F_REQUEST;
	if (action >= 0) {
		nlm->hdr.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
		nlm->hdr.nlmsg_type = RTM_NEWADDR;
	} else
		nlm->hdr.nlmsg_type = RTM_DELADDR;
	if (!(nlm->ifa.ifa_index = if_nametoindex(iface->name))) {
		free(nlm);
		errno = ENODEV;
		return -1;
	}
	nlm->ifa.ifa_family = AF_INET;
	nlm->ifa.ifa_prefixlen = inet_ntocidr(*netmask);
	/* This creates the aliased interface */
	add_attr_l(&nlm->hdr, sizeof(*nlm), IFA_LABEL,
		   iface->name, strlen(iface->name) + 1);
	add_attr_l(&nlm->hdr, sizeof(*nlm), IFA_LOCAL,
		   &address->s_addr, sizeof(address->s_addr));
	if (action >= 0)
		add_attr_l(&nlm->hdr, sizeof(*nlm), IFA_BROADCAST,
			   &broadcast->s_addr, sizeof(broadcast->s_addr));

	if (send_netlink(&nlm->hdr) == -1)
		retval = -1;
	free(nlm);
	return retval;
}

int
if_route(const struct interface *iface,
	 const struct in_addr *destination, const struct in_addr *netmask,
	 const struct in_addr *gateway, int metric, int action)
{
	struct nlmr *nlm;
	unsigned int ifindex;
	int retval = 0;


	if (!(ifindex = if_nametoindex(iface->name))) {
		errno = ENODEV;
		return -1;
	}

	nlm = xzalloc(sizeof(*nlm));
	nlm->hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	nlm->hdr.nlmsg_type = RTM_NEWROUTE;
	if (action == 0)
		nlm->hdr.nlmsg_flags = NLM_F_REPLACE;
	else if (action  == 1)
		nlm->hdr.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	else
		nlm->hdr.nlmsg_type = RTM_DELROUTE;
	nlm->hdr.nlmsg_flags |= NLM_F_REQUEST;
	nlm->rt.rtm_family = AF_INET;
	nlm->rt.rtm_table = RT_TABLE_MAIN;

	if (action == -1 || action == -2)
		nlm->rt.rtm_scope = RT_SCOPE_NOWHERE;
	else {
		nlm->hdr.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
		/* We only change route metrics for kernel routes */
		nlm->rt.rtm_protocol = action ? RTPROT_BOOT : RTPROT_KERNEL;
		if (gateway->s_addr == INADDR_ANY)
			nlm->rt.rtm_scope = RT_SCOPE_LINK;
		else
			nlm->rt.rtm_scope = RT_SCOPE_UNIVERSE;
		nlm->rt.rtm_type = RTN_UNICAST;
	}

	nlm->rt.rtm_dst_len = inet_ntocidr(*netmask);
	add_attr_l(&nlm->hdr, sizeof(*nlm), RTA_DST,
		   &destination->s_addr, sizeof(destination->s_addr));
	if (action != 1) {
		add_attr_l(&nlm->hdr, sizeof(*nlm), RTA_PREFSRC,
			   &iface->addr.s_addr, sizeof(iface->addr.s_addr));
	}
	add_attr_l(&nlm->hdr, sizeof(*nlm), RTA_GATEWAY,
		   &gateway->s_addr, sizeof(gateway->s_addr));

	add_attr_32(&nlm->hdr, sizeof(*nlm), RTA_OIF, ifindex);
	add_attr_32(&nlm->hdr, sizeof(*nlm), RTA_PRIORITY, metric);

	if (send_netlink(&nlm->hdr) == -1)
		retval = -1;
	free(nlm);
	return retval;
}

struct interface *
discover_interfaces(int argc, char * const *argv)
{
	FILE *f;
	char *buffer = NULL, *p;
	size_t len = 0, ln = 0, n;
	struct interface *ifs = NULL, *ifp, *ifl;

	if ((f = fopen("/proc/net/dev", "r"))) {
		while (get_line(&buffer, &len, f)) {
			if (++ln < 2)
				continue;
			p = buffer;
			while (isspace((unsigned int)*p))
				p++;
			n = strcspn(p, ": \t");
			p[n]= '\0';
			ifl = NULL;
			for (ifp = ifs; ifp; ifp = ifp->next) {
				if (strcmp(ifp->name, p) == 0)
					break;
				ifl = ifp;
			}
			if (ifp)
				continue;
			if (argc > 0) {
				for (n = 0; n < argc; n++)
					if (strcmp(argv[n], p) == 0)
						break;
				if (n == argc)
					continue;
			} else {
				for (n = 0; n < ifdc; n++)
					if (!fnmatch(ifdv[n], ifr->ifr_name, 0))
						break;
				if (n < ifdc)
					continue;
				for (n = 0; n < ifac; n++)
					if (!fnmatch(ifav[n], ifr->ifr_name, 0))
						break;
				if (ifac && n == ifac)
					continue;
			}
			if ((ifp = init_interface(p))) {
				if (ifl)
					ifl->next = ifp; 
				else
					ifs = ifp;
			}
		}
		fclose(f);
		free(buffer);
	}
	return ifs;
}
