/*
 * Copyright (c) 2004-2014 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1982, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <kern/debug.h>
#include <netinet/in_arp.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel_types.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <sys/mcache.h>
#include <sys/protosw.h>
#include <string.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/dlil.h>
#include <net/if_types.h>
#include <net/if_llreach.h>
#include <net/route.h>

#include <netinet/if_ether.h>
#include <netinet/in_var.h>
#include <kern/zalloc.h>

#define	CONST_LLADDR(s)	((const u_char*)((s)->sdl_data + (s)->sdl_nlen))

static const size_t MAX_HW_LEN = 10;

/*
 * Synchronization notes:
 *
 * The global list of ARP entries are stored in llinfo_arp; an entry
 * gets inserted into the list when the route is created and gets
 * removed from the list when it is deleted; this is done as part
 * of RTM_ADD/RTM_RESOLVE/RTM_DELETE in arp_rtrequest().
 *
 * Because rnh_lock and rt_lock for the entry are held during those
 * operations, the same locks (and thus lock ordering) must be used
 * elsewhere to access the relevant data structure fields:
 *
 * la_le.{le_next,le_prev}, la_rt
 *
 *	- Routing lock (rnh_lock)
 *
 * la_hold, la_asked, la_llreach, la_lastused, la_flags
 *
 *	- Routing entry lock (rt_lock)
 *
 * Due to the dependency on rt_lock, llinfo_arp has the same lifetime
 * as the route entry itself.  When a route is deleted (RTM_DELETE),
 * it is simply removed from the global list but the memory is not
 * freed until the route itself is freed.
 */
struct llinfo_arp {
	/*
	 * The following are protected by rnh_lock
	 */
	LIST_ENTRY(llinfo_arp) la_le;
	struct	rtentry *la_rt;
	/*
	 * The following are protected by rt_lock
	 */
	struct	mbuf *la_hold;		/* last packet until resolved/timeout */
	struct	if_llreach *la_llreach;	/* link-layer reachability record */
	u_int64_t la_lastused;		/* last used timestamp */
	u_int32_t la_asked;		/* # of requests sent */
	u_int32_t la_maxtries;		/* retry limit */
	uint32_t  la_flags;
#define LLINFO_RTRFAIL_EVTSENT		0x1 /* sent an ARP event */
};
static LIST_HEAD(, llinfo_arp) llinfo_arp;

static int arp_timeout_run;		/* arp_timeout is scheduled to run */
static void arp_timeout(void *);
static void arp_sched_timeout(struct timeval *);

static void arptfree(struct llinfo_arp *, void *);
static errno_t arp_lookup_route(const struct in_addr *, int,
    int, route_t *, unsigned int);
static int arp_getstat SYSCTL_HANDLER_ARGS;

static struct llinfo_arp *arp_llinfo_alloc(int);
static void arp_llinfo_free(void *);
static void arp_llinfo_purge(struct rtentry *);
static void arp_llinfo_get_ri(struct rtentry *, struct rt_reach_info *);
static void arp_llinfo_get_iflri(struct rtentry *, struct ifnet_llreach_info *);

static __inline void arp_llreach_use(struct llinfo_arp *);
static __inline int arp_llreach_reachable(struct llinfo_arp *);
static void arp_llreach_alloc(struct rtentry *, struct ifnet *, void *,
    unsigned int, boolean_t);

extern int tvtohz(struct timeval *);

static int arpinit_done;

SYSCTL_DECL(_net_link_ether);
SYSCTL_NODE(_net_link_ether, PF_INET, inet, CTLFLAG_RW|CTLFLAG_LOCKED, 0, "");

/* timer values */
static int arpt_prune = (5*60*1); /* walk list every 5 minutes */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, prune_intvl,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arpt_prune, 0, "");

static int arpt_keep = (20*60); /* once resolved, good for 20 more minutes */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, max_age,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arpt_keep, 0, "");

static int arpt_down = 20;	/* once declared down, don't send for 20 sec */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, host_down_time,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arpt_down, 0, "");

static int arp_llreach_base = (LL_BASE_REACHABLE / 1000); /* seconds */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, arp_llreach_base,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_llreach_base, LL_BASE_REACHABLE,
	"default ARP link-layer reachability max lifetime (in seconds)");

#define	ARP_UNICAST_LIMIT 5	/* # of probes until ARP refresh broadcast */
static u_int32_t arp_unicast_lim = ARP_UNICAST_LIMIT;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, arp_unicast_lim,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_unicast_lim, ARP_UNICAST_LIMIT,
	"number of unicast ARP refresh probes before using broadcast");

static u_int32_t arp_maxtries = 5;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, maxtries,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_maxtries, 0, "");

static int useloopback = 1;	/* use loopback interface for local traffic */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, useloopback,
	CTLFLAG_RW | CTLFLAG_LOCKED, &useloopback, 0, "");

static int arp_proxyall = 0;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, proxyall,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_proxyall, 0, "");

static int arp_sendllconflict = 0;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, sendllconflict,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_sendllconflict, 0, "");

static int log_arp_warnings = 0;	/* Thread safe: no accumulated state */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, log_arp_warnings,
	CTLFLAG_RW | CTLFLAG_LOCKED,
	&log_arp_warnings, 0,
	"log arp warning messages");

static int keep_announcements = 1;	/* Thread safe: no aging of state */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, keep_announcements,
	CTLFLAG_RW | CTLFLAG_LOCKED,
	&keep_announcements, 0,
	"keep arp announcements");

static int send_conflicting_probes = 1;	/* Thread safe: no accumulated state */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, send_conflicting_probes,
	CTLFLAG_RW | CTLFLAG_LOCKED,
	&send_conflicting_probes, 0,
	"send conflicting link-local arp probes");

static int arp_verbose;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, verbose,
	CTLFLAG_RW | CTLFLAG_LOCKED, &arp_verbose, 0, "");

struct arpstat arpstat;
SYSCTL_PROC(_net_link_ether_inet, OID_AUTO, stats,
	CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
	0, 0, arp_getstat, "S,arpstat",
	"ARP statistics (struct arpstat, net/if_arp.h)");

/* these are deprecated (read-only); use net.link.generic.system node instead */
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, apple_hwcksum_tx,
	CTLFLAG_RD | CTLFLAG_LOCKED, &hwcksum_tx, 0, "");

SYSCTL_INT(_net_link_ether_inet, OID_AUTO, apple_hwcksum_rx,
	CTLFLAG_RD | CTLFLAG_LOCKED, &hwcksum_rx, 0, "");

static struct zone *llinfo_arp_zone;
#define	LLINFO_ARP_ZONE_MAX	256		/* maximum elements in zone */
#define	LLINFO_ARP_ZONE_NAME	"llinfo_arp"	/* name for zone */

void
arp_init(void)
{
	VERIFY(!arpinit_done);

	LIST_INIT(&llinfo_arp);

	llinfo_arp_zone = zinit(sizeof (struct llinfo_arp),
	    LLINFO_ARP_ZONE_MAX * sizeof (struct llinfo_arp), 0,
	    LLINFO_ARP_ZONE_NAME);
	if (llinfo_arp_zone == NULL)
		panic("%s: failed allocating llinfo_arp_zone", __func__);

	zone_change(llinfo_arp_zone, Z_EXPAND, TRUE);
	zone_change(llinfo_arp_zone, Z_CALLERACCT, FALSE);

	arpinit_done = 1;
}

static struct llinfo_arp *
arp_llinfo_alloc(int how)
{
	struct llinfo_arp *la;

	la = (how == M_WAITOK) ? zalloc(llinfo_arp_zone) :
	    zalloc_noblock(llinfo_arp_zone);
	if (la != NULL)
		bzero(la, sizeof (*la));

	return (la);
}

static void
arp_llinfo_free(void *arg)
{
	struct llinfo_arp *la = arg;

	if (la->la_le.le_next != NULL || la->la_le.le_prev != NULL) {
		panic("%s: trying to free %p when it is in use", __func__, la);
		/* NOTREACHED */
	}

	/* Just in case there's anything there, free it */
	if (la->la_hold != NULL) {
		m_freem(la->la_hold);
		la->la_hold = NULL;
		arpstat.purged++;
	}

	/* Purge any link-layer info caching */
	VERIFY(la->la_rt->rt_llinfo == la);
	if (la->la_rt->rt_llinfo_purge != NULL)
		la->la_rt->rt_llinfo_purge(la->la_rt);

	zfree(llinfo_arp_zone, la);
}

static void
arp_llinfo_purge(struct rtentry *rt)
{
	struct llinfo_arp *la = rt->rt_llinfo;

	RT_LOCK_ASSERT_HELD(rt);
	VERIFY(rt->rt_llinfo_purge == arp_llinfo_purge && la != NULL);

	if (la->la_llreach != NULL) {
		RT_CONVERT_LOCK(rt);
		ifnet_llreach_free(la->la_llreach);
		la->la_llreach = NULL;
	}
	la->la_lastused = 0;
}

static void
arp_llinfo_get_ri(struct rtentry *rt, struct rt_reach_info *ri)
{
	struct llinfo_arp *la = rt->rt_llinfo;
	struct if_llreach *lr = la->la_llreach;

	if (lr == NULL) {
		bzero(ri, sizeof (*ri));
		ri->ri_rssi = IFNET_RSSI_UNKNOWN;
		ri->ri_lqm = IFNET_LQM_THRESH_OFF;
		ri->ri_npm = IFNET_NPM_THRESH_UNKNOWN;
	} else {
		IFLR_LOCK(lr);
		/* Export to rt_reach_info structure */
		ifnet_lr2ri(lr, ri);
		/* Export ARP send expiration (calendar) time */
		ri->ri_snd_expire =
		    ifnet_llreach_up2calexp(lr, la->la_lastused);
		IFLR_UNLOCK(lr);
	}
}

static void
arp_llinfo_get_iflri(struct rtentry *rt, struct ifnet_llreach_info *iflri)
{
	struct llinfo_arp *la = rt->rt_llinfo;
	struct if_llreach *lr = la->la_llreach;

	if (lr == NULL) {
		bzero(iflri, sizeof (*iflri));
		iflri->iflri_rssi = IFNET_RSSI_UNKNOWN;
		iflri->iflri_lqm = IFNET_LQM_THRESH_OFF;
		iflri->iflri_npm = IFNET_NPM_THRESH_UNKNOWN;
	} else {
		IFLR_LOCK(lr);
		/* Export to ifnet_llreach_info structure */
		ifnet_lr2iflri(lr, iflri);
		/* Export ARP send expiration (uptime) time */
		iflri->iflri_snd_expire =
		    ifnet_llreach_up2upexp(lr, la->la_lastused);
		IFLR_UNLOCK(lr);
	}
}

void
arp_llreach_set_reachable(struct ifnet *ifp, void *addr, unsigned int alen)
{
	/* Nothing more to do if it's disabled */
	if (arp_llreach_base == 0)
		return;

	ifnet_llreach_set_reachable(ifp, ETHERTYPE_IP, addr, alen);
}

static __inline void
arp_llreach_use(struct llinfo_arp *la)
{
	if (la->la_llreach != NULL)
		la->la_lastused = net_uptime();
}

static __inline int
arp_llreach_reachable(struct llinfo_arp *la)
{
	struct if_llreach *lr;
	const char *why = NULL;

	/* Nothing more to do if it's disabled; pretend it's reachable  */
	if (arp_llreach_base == 0)
		return (1);

	if ((lr = la->la_llreach) == NULL) {
		/*
		 * Link-layer reachability record isn't present for this
		 * ARP entry; pretend it's reachable and use it as is.
		 */
		return (1);
	} else if (ifnet_llreach_reachable(lr)) {
		/*
		 * Record is present, it's not shared with other ARP
		 * entries and a packet has recently been received
		 * from the remote host; consider it reachable.
		 */
		if (lr->lr_reqcnt == 1)
			return (1);

		/* Prime it up, if this is the first time */
		if (la->la_lastused == 0) {
			VERIFY(la->la_llreach != NULL);
			arp_llreach_use(la);
		}

		/*
		 * Record is present and shared with one or more ARP
		 * entries, and a packet has recently been received
		 * from the remote host.  Since it's shared by more
		 * than one IP addresses, we can't rely on the link-
		 * layer reachability alone; consider it reachable if
		 * this ARP entry has been used "recently."
		 */
		if (ifnet_llreach_reachable_delta(lr, la->la_lastused))
			return (1);

		why = "has alias(es) and hasn't been used in a while";
	} else {
		why = "haven't heard from it in a while";
	}

	if (arp_verbose > 1) {
		char tmp[MAX_IPv4_STR_LEN];
		u_int64_t now = net_uptime();

		log(LOG_DEBUG, "%s: ARP probe(s) needed for %s; "
		    "%s [lastused %lld, lastrcvd %lld] secs ago\n",
		    if_name(lr->lr_ifp), inet_ntop(AF_INET,
		    &SIN(rt_key(la->la_rt))->sin_addr, tmp, sizeof (tmp)), why,
		    (la->la_lastused ? (int64_t)(now - la->la_lastused) : -1),
		    (lr->lr_lastrcvd ? (int64_t)(now - lr->lr_lastrcvd) : -1));

	}
	return (0);
}

/*
 * Obtain a link-layer source cache entry for the sender.
 *
 * NOTE: This is currently only for ARP/Ethernet.
 */
static void
arp_llreach_alloc(struct rtentry *rt, struct ifnet *ifp, void *addr,
    unsigned int alen, boolean_t solicited)
{
	VERIFY(rt->rt_expire == 0 || rt->rt_rmx.rmx_expire != 0);
	VERIFY(rt->rt_expire != 0 || rt->rt_rmx.rmx_expire == 0);

	if (arp_llreach_base != 0 && rt->rt_expire != 0 &&
	    !(rt->rt_ifp->if_flags & IFF_LOOPBACK) &&
	    ifp->if_addrlen == IF_LLREACH_MAXLEN &&	/* Ethernet */
	    alen == ifp->if_addrlen) {
		struct llinfo_arp *la = rt->rt_llinfo;
		struct if_llreach *lr;
		const char *why = NULL, *type = "";

		/* Become a regular mutex, just in case */
		RT_CONVERT_LOCK(rt);

		if ((lr = la->la_llreach) != NULL) {
			type = (solicited ? "ARP reply" : "ARP announcement");
			/*
			 * If target has changed, create a new record;
			 * otherwise keep existing record.
			 */
			IFLR_LOCK(lr);
			if (bcmp(addr, lr->lr_key.addr, alen) != 0) {
				IFLR_UNLOCK(lr);
				/* Purge any link-layer info caching */
				VERIFY(rt->rt_llinfo_purge != NULL);
				rt->rt_llinfo_purge(rt);
				lr = NULL;
				why = " for different target HW address; "
				    "using new llreach record";
			} else {
				lr->lr_probes = 0;	/* reset probe count */
				IFLR_UNLOCK(lr);
				if (solicited) {
					why = " for same target HW address; "
					    "keeping existing llreach record";
				}
			}
		}

		if (lr == NULL) {
			lr = la->la_llreach = ifnet_llreach_alloc(ifp,
			    ETHERTYPE_IP, addr, alen, arp_llreach_base);
			if (lr != NULL) {
				lr->lr_probes = 0;	/* reset probe count */
				if (why == NULL)
					why = "creating new llreach record";
			}
		}

		/* Bump up retry ceiling to accomodate unicast retries */
		if (lr != NULL)
			la->la_maxtries = arp_maxtries + arp_unicast_lim;

		if (arp_verbose > 1 && lr != NULL && why != NULL) {
			char tmp[MAX_IPv4_STR_LEN];

			log(LOG_DEBUG, "%s: %s%s for %s\n", if_name(ifp),
			    type, why, inet_ntop(AF_INET,
			    &SIN(rt_key(rt))->sin_addr, tmp, sizeof (tmp)));
		}
	}
}

struct arptf_arg {
	int draining;
	uint32_t killed;
	uint32_t aging;
	uint32_t sticky;
	uint32_t found;
};

/*
 * Free an arp entry.
 */
static void
arptfree(struct llinfo_arp *la, void *arg)
{
	struct arptf_arg *ap = arg;
	struct rtentry *rt = la->la_rt;

	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);

	/* rnh_lock acquired by caller protects rt from going away */
	RT_LOCK(rt);

	VERIFY(rt->rt_expire == 0 || rt->rt_rmx.rmx_expire != 0);
	VERIFY(rt->rt_expire != 0 || rt->rt_rmx.rmx_expire == 0);

	ap->found++;
	if (rt->rt_expire == 0 || (rt->rt_flags & RTF_STATIC)) {
		ap->sticky++;
		/* ARP entry is permanent? */
		if (rt->rt_expire == 0) {
			RT_UNLOCK(rt);
			return;
		}
	}

	/* ARP entry hasn't expired and we're not draining? */
	if (!ap->draining && rt->rt_expire > net_uptime()) {
		RT_UNLOCK(rt);
		ap->aging++;
		return;
	}

	if (rt->rt_refcnt > 0) {
		/*
		 * ARP entry has expired, with outstanding refcnt.
		 * If we're not draining, force ARP query to be
		 * generated next time this entry is used.
		 */
		if (!ap->draining) {
			struct sockaddr_dl *sdl = SDL(rt->rt_gateway);
			if (sdl != NULL)
				sdl->sdl_alen = 0;
			la->la_asked = 0;
			rt->rt_flags &= ~RTF_REJECT;
		}
		RT_UNLOCK(rt);
	} else if (!(rt->rt_flags & RTF_STATIC)) {
		/*
		 * ARP entry has no outstanding refcnt, and we're either
		 * draining or it has expired; delete it from the routing
		 * table.  Safe to drop rt_lock and use rt_key, since holding
		 * rnh_lock here prevents another thread from calling
		 * rt_setgate() on this route.
		 */
		RT_UNLOCK(rt);
		rtrequest_locked(RTM_DELETE, rt_key(rt), NULL,
		    rt_mask(rt), 0, NULL);
		arpstat.timeouts++;
		ap->killed++;
	} else {
		/* ARP entry is static; let it linger */
		RT_UNLOCK(rt);
	}
}

void
in_arpdrain(void *arg)
{
#pragma unused(arg)
	struct llinfo_arp *la, *ola;
	struct arptf_arg farg;

	if (arp_verbose)
		log(LOG_DEBUG, "%s: draining ARP entries\n", __func__);

	lck_mtx_lock(rnh_lock);
	la = llinfo_arp.lh_first;
	bzero(&farg, sizeof (farg));
	farg.draining = 1;
	while ((ola = la) != NULL) {
		la = la->la_le.le_next;
		arptfree(ola, &farg);
	}
	if (arp_verbose) {
		log(LOG_DEBUG, "%s: found %u, aging %u, sticky %u, killed %u\n",
		    __func__, farg.found, farg.aging, farg.sticky, farg.killed);
	}
	lck_mtx_unlock(rnh_lock);
}

/*
 * Timeout routine.  Age arp_tab entries periodically.
 */
static void
arp_timeout(void *arg)
{
#pragma unused(arg)
	struct llinfo_arp *la, *ola;
	struct timeval atv;
	struct arptf_arg farg;

	lck_mtx_lock(rnh_lock);
	la = llinfo_arp.lh_first;
	bzero(&farg, sizeof (farg));
	while ((ola = la) != NULL) {
		la = la->la_le.le_next;
		arptfree(ola, &farg);
	}
	if (arp_verbose) {
		log(LOG_DEBUG, "%s: found %u, aging %u, sticky %u, killed %u\n",
		    __func__, farg.found, farg.aging, farg.sticky, farg.killed);
	}
	atv.tv_usec = 0;
	atv.tv_sec = arpt_prune;
	/* re-arm the timer if there's work to do */
	arp_timeout_run = 0;
	if (farg.aging > 0)
		arp_sched_timeout(&atv);
	else if (arp_verbose)
		log(LOG_DEBUG, "%s: not rescheduling timer\n", __func__);
	lck_mtx_unlock(rnh_lock);
}

static void
arp_sched_timeout(struct timeval *atv)
{
	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);

	if (!arp_timeout_run) {
		struct timeval tv;

		if (atv == NULL) {
			tv.tv_usec = 0;
			tv.tv_sec = MAX(arpt_prune / 5, 1);
			atv = &tv;
		}
		if (arp_verbose) {
			log(LOG_DEBUG, "%s: timer scheduled in "
			    "T+%llus.%lluu\n", __func__,
			    (uint64_t)atv->tv_sec, (uint64_t)atv->tv_usec);
		}
		arp_timeout_run = 1;
		timeout(arp_timeout, NULL, tvtohz(atv));
	}
}

/*
 * ifa_rtrequest() callback
 */
static void
arp_rtrequest(int req, struct rtentry *rt, struct sockaddr *sa)
{
#pragma unused(sa)
	struct sockaddr *gate = rt->rt_gateway;
	struct llinfo_arp *la = rt->rt_llinfo;
	static struct sockaddr_dl null_sdl =
	    { .sdl_len = sizeof (null_sdl), .sdl_family = AF_LINK };
	uint64_t timenow;
	char buf[MAX_IPv4_STR_LEN];

	VERIFY(arpinit_done);
	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);
	RT_LOCK_ASSERT_HELD(rt);

	if (rt->rt_flags & RTF_GATEWAY)
		return;

	timenow = net_uptime();
	switch (req) {
	case RTM_ADD:
		/*
		 * XXX: If this is a manually added route to interface
		 * such as older version of routed or gated might provide,
		 * restore cloning bit.
		 */
		if (!(rt->rt_flags & RTF_HOST) && rt_mask(rt) != NULL &&
		    SIN(rt_mask(rt))->sin_addr.s_addr != INADDR_BROADCAST)
			rt->rt_flags |= RTF_CLONING;

		if (rt->rt_flags & RTF_CLONING) {
			/*
			 * Case 1: This route should come from a route to iface.
			 */
			if (rt_setgate(rt, rt_key(rt), SA(&null_sdl)) == 0) {
				gate = rt->rt_gateway;
				SDL(gate)->sdl_type = rt->rt_ifp->if_type;
				SDL(gate)->sdl_index = rt->rt_ifp->if_index;
				/*
				 * In case we're called before 1.0 sec.
				 * has elapsed.
				 */
				rt_setexpire(rt, MAX(timenow, 1));
			}
			break;
		}
		/* Announce a new entry if requested. */
		if (rt->rt_flags & RTF_ANNOUNCE) {
			if (la != NULL)
				arp_llreach_use(la); /* Mark use timestamp */
			RT_UNLOCK(rt);
			dlil_send_arp(rt->rt_ifp, ARPOP_REQUEST,
			    SDL(gate), rt_key(rt), NULL, rt_key(rt), 0);
			RT_LOCK(rt);
			arpstat.txannounces++;
		}
		/* FALLTHRU */
	case RTM_RESOLVE:
		if (gate->sa_family != AF_LINK ||
		    gate->sa_len < sizeof (null_sdl)) {
			arpstat.invalidreqs++;
			log(LOG_ERR, "%s: route to %s has bad gateway address "
			    "(sa_family %u sa_len %u) on %s\n",
			    __func__, inet_ntop(AF_INET,
			    &SIN(rt_key(rt))->sin_addr.s_addr, buf,
			    sizeof (buf)), gate->sa_family, gate->sa_len,
			    if_name(rt->rt_ifp));
			break;
		}
		SDL(gate)->sdl_type = rt->rt_ifp->if_type;
		SDL(gate)->sdl_index = rt->rt_ifp->if_index;

		if (la != NULL)
			break; /* This happens on a route change */

		/*
		 * Case 2:  This route may come from cloning, or a manual route
		 * add with a LL address.
		 */
		rt->rt_llinfo = la = arp_llinfo_alloc(M_WAITOK);
		if (la == NULL) {
			arpstat.reqnobufs++;
			break;
		}
		rt->rt_llinfo_get_ri	= arp_llinfo_get_ri;
		rt->rt_llinfo_get_iflri	= arp_llinfo_get_iflri;
		rt->rt_llinfo_purge	= arp_llinfo_purge;
		rt->rt_llinfo_free	= arp_llinfo_free;
		rt->rt_flags |= RTF_LLINFO;
		la->la_rt = rt;
		LIST_INSERT_HEAD(&llinfo_arp, la, la_le);
		arpstat.inuse++;

		/* We have at least one entry; arm the timer if not already */
		arp_sched_timeout(NULL);

		/*
		 * This keeps the multicast addresses from showing up
		 * in `arp -a' listings as unresolved.  It's not actually
		 * functional.  Then the same for broadcast.  For IPv4
		 * link-local address, keep the entry around even after
		 * it has expired.
		 */
		if (IN_MULTICAST(ntohl(SIN(rt_key(rt))->sin_addr.s_addr))) {
			RT_UNLOCK(rt);
			dlil_resolve_multi(rt->rt_ifp, rt_key(rt), gate,
			    sizeof (struct sockaddr_dl));
			RT_LOCK(rt);
			rt_setexpire(rt, 0);
		} else if (in_broadcast(SIN(rt_key(rt))->sin_addr,
		    rt->rt_ifp)) {
			struct sockaddr_dl *gate_ll = SDL(gate);
			size_t broadcast_len;
			ifnet_llbroadcast_copy_bytes(rt->rt_ifp,
			    LLADDR(gate_ll), sizeof (gate_ll->sdl_data),
			    &broadcast_len);
			gate_ll->sdl_alen = broadcast_len;
			gate_ll->sdl_family = AF_LINK;
			gate_ll->sdl_len = sizeof (struct sockaddr_dl);
			/* In case we're called before 1.0 sec. has elapsed */
			rt_setexpire(rt, MAX(timenow, 1));
		} else if (IN_LINKLOCAL(ntohl(SIN(rt_key(rt))->
		    sin_addr.s_addr))) {
			rt->rt_flags |= RTF_STATIC;
		}

		/* Set default maximum number of retries */
		la->la_maxtries = arp_maxtries;

		/* Become a regular mutex, just in case */
		RT_CONVERT_LOCK(rt);
		IFA_LOCK_SPIN(rt->rt_ifa);
		if (SIN(rt_key(rt))->sin_addr.s_addr ==
		    (IA_SIN(rt->rt_ifa))->sin_addr.s_addr) {
			IFA_UNLOCK(rt->rt_ifa);
			/*
			 * This test used to be
			 *	if (loif.if_flags & IFF_UP)
			 * It allowed local traffic to be forced through the
			 * hardware by configuring the loopback down.  However,
			 * it causes problems during network configuration
			 * for boards that can't receive packets they send.
			 * It is now necessary to clear "useloopback" and
			 * remove the route to force traffic out to the
			 * hardware.
			 */
			rt_setexpire(rt, 0);
			ifnet_lladdr_copy_bytes(rt->rt_ifp, LLADDR(SDL(gate)),
			    SDL(gate)->sdl_alen = rt->rt_ifp->if_addrlen);
			if (useloopback) {
				if (rt->rt_ifp != lo_ifp) {
					/*
					 * Purge any link-layer info caching.
					 */
					if (rt->rt_llinfo_purge != NULL)
						rt->rt_llinfo_purge(rt);

					/*
					 * Adjust route ref count for the
					 * interfaces.
					 */
					if (rt->rt_if_ref_fn != NULL) {
						rt->rt_if_ref_fn(lo_ifp, 1);
						rt->rt_if_ref_fn(rt->rt_ifp, -1);
					}
				}
				rt->rt_ifp = lo_ifp;
				/*
				 * If rmx_mtu is not locked, update it
				 * to the MTU used by the new interface.
				 */
				if (!(rt->rt_rmx.rmx_locks & RTV_MTU))
					rt->rt_rmx.rmx_mtu = rt->rt_ifp->if_mtu;
			}
		} else {
			IFA_UNLOCK(rt->rt_ifa);
		}
		break;

	case RTM_DELETE:
		if (la == NULL)
			break;
		/*
		 * Unchain it but defer the actual freeing until the route
		 * itself is to be freed.  rt->rt_llinfo still points to
		 * llinfo_arp, and likewise, la->la_rt still points to this
		 * route entry, except that RTF_LLINFO is now cleared.
		 */
		LIST_REMOVE(la, la_le);
		la->la_le.le_next = NULL;
		la->la_le.le_prev = NULL;
		arpstat.inuse--;

		/*
		 * Purge any link-layer info caching.
		 */
		if (rt->rt_llinfo_purge != NULL)
			rt->rt_llinfo_purge(rt);

		rt->rt_flags &= ~RTF_LLINFO;
		if (la->la_hold != NULL) {
			m_freem(la->la_hold);
			la->la_hold = NULL;
			arpstat.purged++;
		}
	}
}

/*
 * convert hardware address to hex string for logging errors.
 */
static const char *
sdl_addr_to_hex(const struct sockaddr_dl *sdl, char *orig_buf, int buflen)
{
	char *buf = orig_buf;
	int i;
	const u_char *lladdr = (u_char *)(size_t)sdl->sdl_data;
	int maxbytes = buflen / 3;

	if (maxbytes > sdl->sdl_alen) {
		maxbytes = sdl->sdl_alen;
	}
	*buf = '\0';
	for (i = 0; i < maxbytes; i++) {
		snprintf(buf, 3, "%02x", lladdr[i]);
		buf += 2;
		*buf = (i == maxbytes - 1) ? '\0' : ':';
		buf++;
	}
	return (orig_buf);
}

/*
 * arp_lookup_route will lookup the route for a given address.
 *
 * The address must be for a host on a local network on this interface.
 * If the returned route is non-NULL, the route is locked and the caller
 * is responsible for unlocking it and releasing its reference.
 */
static errno_t
arp_lookup_route(const struct in_addr *addr, int create, int proxy,
    route_t *route, unsigned int ifscope)
{
	struct sockaddr_inarp sin =
	    { sizeof (sin), AF_INET, 0, { 0 }, { 0 }, 0, 0 };
	const char *why = NULL;
	errno_t	error = 0;
	route_t rt;

	*route = NULL;

	sin.sin_addr.s_addr = addr->s_addr;
	sin.sin_other = proxy ? SIN_PROXY : 0;

	/*
	 * If the destination is a link-local address, don't
	 * constrain the lookup (don't scope it).
	 */
	if (IN_LINKLOCAL(ntohl(addr->s_addr)))
		ifscope = IFSCOPE_NONE;

	rt = rtalloc1_scoped((struct sockaddr *)&sin, create, 0, ifscope);
	if (rt == NULL)
		return (ENETUNREACH);

	RT_LOCK(rt);

	if (rt->rt_flags & RTF_GATEWAY) {
		why = "host is not on local network";
		error = ENETUNREACH;
	} else if (!(rt->rt_flags & RTF_LLINFO)) {
		why = "could not allocate llinfo";
		error = ENOMEM;
	} else if (rt->rt_gateway->sa_family != AF_LINK) {
		why = "gateway route is not ours";
		error = EPROTONOSUPPORT;
	}

	if (error != 0) {
		if (create && (arp_verbose || log_arp_warnings)) {
			char tmp[MAX_IPv4_STR_LEN];
			log(LOG_DEBUG, "%s: link#%d %s failed: %s\n",
			    __func__, ifscope, inet_ntop(AF_INET, addr, tmp,
			    sizeof (tmp)), why);
		}

		/*
		 * If there are no references to this route, and it is
		 * a cloned route, and not static, and ARP had created
		 * the route, then purge it from the routing table as
		 * it is probably bogus.
		 */
		if (rt->rt_refcnt == 1 &&
		    (rt->rt_flags & (RTF_WASCLONED | RTF_STATIC)) ==
		    RTF_WASCLONED) {
			/*
			 * Prevent another thread from modiying rt_key,
			 * rt_gateway via rt_setgate() after rt_lock is
			 * dropped by marking the route as defunct.
			 */
			rt->rt_flags |= RTF_CONDEMNED;
			RT_UNLOCK(rt);
			rtrequest(RTM_DELETE, rt_key(rt), rt->rt_gateway,
			    rt_mask(rt), rt->rt_flags, NULL);
			rtfree(rt);
		} else {
			RT_REMREF_LOCKED(rt);
			RT_UNLOCK(rt);
		}
		return (error);
	}

	/*
	 * Caller releases reference and does RT_UNLOCK(rt).
	 */
	*route = rt;
	return (0);
}

/*
 * This is the ARP pre-output routine; care must be taken to ensure that
 * the "hint" route never gets freed via rtfree(), since the caller may
 * have stored it inside a struct route with a reference held for that
 * placeholder.
 */
errno_t
arp_lookup_ip(ifnet_t ifp, const struct sockaddr_in *net_dest,
    struct sockaddr_dl *ll_dest, size_t	ll_dest_len, route_t hint,
    mbuf_t packet)
{
	route_t	route = NULL;	/* output route */
	errno_t	result = 0;
	struct sockaddr_dl *gateway;
	struct llinfo_arp *llinfo = NULL;
	uint64_t timenow;
	int unreachable = 0;
	struct if_llreach *lr;
	struct ifaddr *rt_ifa;
	struct sockaddr *sa;
	uint32_t rtflags;
	struct sockaddr_dl sdl;

	if (net_dest->sin_family != AF_INET)
		return (EAFNOSUPPORT);

	if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING))
		return (ENETDOWN);

	/*
	 * If we were given a route, verify the route and grab the gateway
	 */
	if (hint != NULL) {
		/*
		 * Callee holds a reference on the route and returns
		 * with the route entry locked, upon success.
		 */
		result = route_to_gwroute((const struct sockaddr *)
		    net_dest, hint, &route);
		if (result != 0)
			return (result);
		if (route != NULL)
			RT_LOCK_ASSERT_HELD(route);
	}

	if (packet->m_flags & M_BCAST) {
		size_t broadcast_len;
		bzero(ll_dest, ll_dest_len);
		result = ifnet_llbroadcast_copy_bytes(ifp, LLADDR(ll_dest),
		    ll_dest_len - offsetof(struct sockaddr_dl, sdl_data),
		    &broadcast_len);
		if (result == 0) {
			ll_dest->sdl_alen = broadcast_len;
			ll_dest->sdl_family = AF_LINK;
			ll_dest->sdl_len = sizeof (struct sockaddr_dl);
		}
		goto release;
	}
	if (packet->m_flags & M_MCAST) {
		if (route != NULL)
			RT_UNLOCK(route);
		result = dlil_resolve_multi(ifp,
		    (const struct sockaddr *)net_dest,
		    (struct sockaddr *)ll_dest, ll_dest_len);
		if (route != NULL)
			RT_LOCK(route);
		goto release;
	}

	/*
	 * If we didn't find a route, or the route doesn't have
	 * link layer information, trigger the creation of the
	 * route and link layer information.
	 */
	if (route == NULL || route->rt_llinfo == NULL) {
		/* Clean up now while we can */
		if (route != NULL) {
			if (route == hint) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
			} else {
				RT_UNLOCK(route);
				rtfree(route);
			}
		}
		/*
		 * Callee holds a reference on the route and returns
		 * with the route entry locked, upon success.
		 */
		result = arp_lookup_route(&net_dest->sin_addr, 1, 0, &route,
		    ifp->if_index);
		if (result == 0)
			RT_LOCK_ASSERT_HELD(route);
	}

	if (result || route == NULL || (llinfo = route->rt_llinfo) == NULL) {
		/* In case result is 0 but no route, return an error */
		if (result == 0)
			result = EHOSTUNREACH;

		if (route != NULL && route->rt_llinfo == NULL) {
			char tmp[MAX_IPv4_STR_LEN];
			log(LOG_ERR, "%s: can't allocate llinfo for %s\n",
			    __func__, inet_ntop(AF_INET, &net_dest->sin_addr,
			    tmp, sizeof (tmp)));
		}
		goto release;
	}

	/*
	 * Now that we have the right route, is it filled in?
	 */
	gateway = SDL(route->rt_gateway);
	timenow = net_uptime();
	VERIFY(route->rt_expire == 0 || route->rt_rmx.rmx_expire != 0);
	VERIFY(route->rt_expire != 0 || route->rt_rmx.rmx_expire == 0);
	if ((route->rt_expire == 0 ||
	    route->rt_expire > timenow) && gateway != NULL &&
	    gateway->sdl_family == AF_LINK && gateway->sdl_alen != 0 &&
	    !(unreachable = !arp_llreach_reachable(llinfo))) {
		bcopy(gateway, ll_dest, MIN(gateway->sdl_len, ll_dest_len));
		result = 0;
		arp_llreach_use(llinfo);	/* Mark use timestamp */
		/*
		 * Start the unicast probe right before the entry expires.
		 */
		lr = llinfo->la_llreach;
		if (lr == NULL)
			goto release;
		rt_ifa = route->rt_ifa;
		/* Become a regular mutex, just in case */
		RT_CONVERT_LOCK(route);
		IFLR_LOCK_SPIN(lr);
		if (route->rt_expire <= timenow + arp_unicast_lim &&
		    ifp->if_addrlen == IF_LLREACH_MAXLEN &&
		    lr->lr_probes <= arp_unicast_lim) {
			lr->lr_probes++;
			bzero(&sdl, sizeof (sdl));
			sdl.sdl_alen = ifp->if_addrlen;
			bcopy(&lr->lr_key.addr, LLADDR(&sdl),
			    ifp->if_addrlen);
			IFLR_UNLOCK(lr);
			IFA_LOCK_SPIN(rt_ifa);
			IFA_ADDREF_LOCKED(rt_ifa);
			sa = rt_ifa->ifa_addr;
			IFA_UNLOCK(rt_ifa);
			rtflags = route->rt_flags;
			RT_UNLOCK(route);
			dlil_send_arp(ifp, ARPOP_REQUEST, NULL, sa,
			    (const struct sockaddr_dl *)&sdl,
			    (const struct sockaddr *)net_dest, rtflags);
			IFA_REMREF(rt_ifa);
			RT_LOCK(route);
		} else
			IFLR_UNLOCK(lr);
		goto release;
	} else if (unreachable) {
		/*
		 * Discard existing answer in case we need to probe.
		 */
		gateway->sdl_alen = 0;
	}

	if (ifp->if_flags & IFF_NOARP) {
		result = ENOTSUP;
		goto release;
	}

	/*
	 * Route wasn't complete/valid. We need to arp.
	 */
	if (packet != NULL) {
		if (llinfo->la_hold != NULL) {
			m_freem(llinfo->la_hold);
			arpstat.dropped++;
		}
		llinfo->la_hold = packet;
	}

	if (route->rt_expire) {
		route->rt_flags &= ~RTF_REJECT;
		if (llinfo->la_asked == 0 || route->rt_expire != timenow) {
			rt_setexpire(route, timenow);
			if (llinfo->la_asked++ < llinfo->la_maxtries) {
				struct kev_msg ev_msg;
				struct kev_in_arpfailure in_arpfailure;
				boolean_t sendkev = FALSE;

				rt_ifa = route->rt_ifa;
				lr = llinfo->la_llreach;
				/* Become a regular mutex, just in case */
				RT_CONVERT_LOCK(route);
				/* Update probe count, if applicable */
				if (lr != NULL) {
					IFLR_LOCK_SPIN(lr);
					lr->lr_probes++;
					IFLR_UNLOCK(lr);
				}
				if (ifp->if_addrlen == IF_LLREACH_MAXLEN &&
				    route->rt_flags & RTF_ROUTER &&
				    llinfo->la_asked > 1) {
					sendkev = TRUE;
					llinfo->la_flags |= LLINFO_RTRFAIL_EVTSENT;
				}
				IFA_LOCK_SPIN(rt_ifa);
				IFA_ADDREF_LOCKED(rt_ifa);
				sa = rt_ifa->ifa_addr;
				IFA_UNLOCK(rt_ifa);
				arp_llreach_use(llinfo); /* Mark use tstamp */
				rtflags = route->rt_flags;
				RT_UNLOCK(route);
				dlil_send_arp(ifp, ARPOP_REQUEST, NULL, sa,
				    NULL, (const struct sockaddr *)net_dest,
				    rtflags);
				IFA_REMREF(rt_ifa);
				if (sendkev) {
					bzero(&ev_msg, sizeof(ev_msg));
					bzero(&in_arpfailure, 
					    sizeof(in_arpfailure));
					in_arpfailure.link_data.if_family =
					    ifp->if_family;
					in_arpfailure.link_data.if_unit =
					    ifp->if_unit;
					strlcpy(in_arpfailure.link_data.if_name,
					    ifp->if_name, IFNAMSIZ);
					ev_msg.vendor_code = KEV_VENDOR_APPLE;
					ev_msg.kev_class = KEV_NETWORK_CLASS;
					ev_msg.kev_subclass = KEV_INET_SUBCLASS;
					ev_msg.event_code =
					    KEV_INET_ARPRTRFAILURE;
					ev_msg.dv[0].data_ptr = &in_arpfailure;
					ev_msg.dv[0].data_length =
					    sizeof(struct
						kev_in_arpfailure); 
					kev_post_msg(&ev_msg);
				}
				result = EJUSTRETURN;
				RT_LOCK(route);
				goto release;
			} else {
				route->rt_flags |= RTF_REJECT;
				rt_setexpire(route,
				    route->rt_expire + arpt_down);
				llinfo->la_asked = 0;
				/*
				 * Clear la_hold; don't free the packet since
				 * we're not returning EJUSTRETURN; the caller
				 * will handle the freeing.
				 */
				llinfo->la_hold = NULL;
				result = EHOSTUNREACH;
				goto release;
			}
		}
	}

	/* The packet is now held inside la_hold (can "packet" be NULL?) */
	result = EJUSTRETURN;

release:
	if (result == EHOSTUNREACH)
		arpstat.dropped++;

	if (route != NULL) {
		if (route == hint) {
			RT_REMREF_LOCKED(route);
			RT_UNLOCK(route);
		} else {
			RT_UNLOCK(route);
			rtfree(route);
		}
	}
	return (result);
}

errno_t
arp_ip_handle_input(ifnet_t ifp, u_short arpop,
    const struct sockaddr_dl *sender_hw, const struct sockaddr_in *sender_ip,
    const struct sockaddr_in *target_ip)
{
	char ipv4str[MAX_IPv4_STR_LEN];
	struct sockaddr_dl proxied;
	struct sockaddr_dl *gateway, *target_hw = NULL;
	struct ifaddr *ifa;
	struct in_ifaddr *ia;
	struct in_ifaddr *best_ia = NULL;
	struct sockaddr_in best_ia_sin;
	route_t	route = NULL;
	char buf[3 * MAX_HW_LEN]; /* enough for MAX_HW_LEN byte hw address */
	struct llinfo_arp *llinfo;
	errno_t	error;
	int created_announcement = 0;
	int bridged = 0, is_bridge = 0;

	arpstat.received++;

	/* Do not respond to requests for 0.0.0.0 */
	if (target_ip->sin_addr.s_addr == INADDR_ANY && arpop == ARPOP_REQUEST)
		goto done;

	if (ifp->if_bridge)
		bridged = 1;
	if (ifp->if_type == IFT_BRIDGE)
		is_bridge = 1;

	if (arpop == ARPOP_REPLY)
		arpstat.rxreplies++;

	/*
	 * Determine if this ARP is for us
	 * For a bridge, we want to check the address irrespective
	 * of the receive interface.
	 */
	lck_rw_lock_shared(in_ifaddr_rwlock);
	TAILQ_FOREACH(ia, INADDR_HASH(target_ip->sin_addr.s_addr), ia_hash) {
		IFA_LOCK_SPIN(&ia->ia_ifa);
		if (((bridged && ia->ia_ifp->if_bridge != NULL) ||
		    (ia->ia_ifp == ifp)) &&
		    ia->ia_addr.sin_addr.s_addr == target_ip->sin_addr.s_addr) {
			best_ia = ia;
			best_ia_sin = best_ia->ia_addr;
			IFA_ADDREF_LOCKED(&ia->ia_ifa);
			IFA_UNLOCK(&ia->ia_ifa);
			lck_rw_done(in_ifaddr_rwlock);
			goto match;
		}
		IFA_UNLOCK(&ia->ia_ifa);
	}

	TAILQ_FOREACH(ia, INADDR_HASH(sender_ip->sin_addr.s_addr), ia_hash) {
		IFA_LOCK_SPIN(&ia->ia_ifa);
		if (((bridged && ia->ia_ifp->if_bridge != NULL) ||
		    (ia->ia_ifp == ifp)) &&
		    ia->ia_addr.sin_addr.s_addr == sender_ip->sin_addr.s_addr) {
			best_ia = ia;
			best_ia_sin = best_ia->ia_addr;
			IFA_ADDREF_LOCKED(&ia->ia_ifa);
			IFA_UNLOCK(&ia->ia_ifa);
			lck_rw_done(in_ifaddr_rwlock);
			goto match;
		}
		IFA_UNLOCK(&ia->ia_ifa);
	}

#define	BDG_MEMBER_MATCHES_ARP(addr, ifp, ia)				     \
	(ia->ia_ifp->if_bridge == ifp->if_softc &&			     \
	bcmp(IF_LLADDR(ia->ia_ifp), IF_LLADDR(ifp), ifp->if_addrlen) == 0 && \
	addr == ia->ia_addr.sin_addr.s_addr)
	/*
	 * Check the case when bridge shares its MAC address with
	 * some of its children, so packets are claimed by bridge
	 * itself (bridge_input() does it first), but they are really
	 * meant to be destined to the bridge member.
	 */
	if (is_bridge) {
		TAILQ_FOREACH(ia, INADDR_HASH(target_ip->sin_addr.s_addr),
		    ia_hash) {
			IFA_LOCK_SPIN(&ia->ia_ifa);
			if (BDG_MEMBER_MATCHES_ARP(target_ip->sin_addr.s_addr,
			    ifp, ia)) {
				ifp = ia->ia_ifp;
				best_ia = ia;
				best_ia_sin = best_ia->ia_addr;
				IFA_ADDREF_LOCKED(&ia->ia_ifa);
				IFA_UNLOCK(&ia->ia_ifa);
				lck_rw_done(in_ifaddr_rwlock);
				goto match;
			}
			IFA_UNLOCK(&ia->ia_ifa);
		}
	}
#undef BDG_MEMBER_MATCHES_ARP
	lck_rw_done(in_ifaddr_rwlock);

	/*
	 * No match, use the first inet address on the receive interface
	 * as a dummy address for the rest of the function; we may be
	 * proxying for another address.
	 */
	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		IFA_LOCK_SPIN(ifa);
		if (ifa->ifa_addr->sa_family != AF_INET) {
			IFA_UNLOCK(ifa);
			continue;
		}
		best_ia = (struct in_ifaddr *)ifa;
		best_ia_sin = best_ia->ia_addr;
		IFA_ADDREF_LOCKED(ifa);
		IFA_UNLOCK(ifa);
		ifnet_lock_done(ifp);
		goto match;
	}
	ifnet_lock_done(ifp);

	/*
	 * If we're not a bridge member, or if we are but there's no
	 * IPv4 address to use for the interface, drop the packet.
	 */
	if (!bridged || best_ia == NULL)
		goto done;

match:
	/* If the packet is from this interface, ignore the packet */
	if (bcmp(CONST_LLADDR(sender_hw), IF_LLADDR(ifp),
	    sender_hw->sdl_alen) == 0)
		goto done;

	/* Check for a conflict */
	if (!bridged &&
	    sender_ip->sin_addr.s_addr == best_ia_sin.sin_addr.s_addr) {
		struct kev_msg ev_msg;
		struct kev_in_collision	*in_collision;
		u_char storage[sizeof (struct kev_in_collision) + MAX_HW_LEN];

		bzero(&ev_msg, sizeof (struct kev_msg));
		bzero(storage, (sizeof (struct kev_in_collision) + MAX_HW_LEN));
		in_collision = (struct kev_in_collision *)(void *)storage;
		log(LOG_ERR, "%s duplicate IP address %s sent from "
		    "address %s\n", if_name(ifp),
		    inet_ntop(AF_INET, &sender_ip->sin_addr, ipv4str,
		    sizeof (ipv4str)), sdl_addr_to_hex(sender_hw, buf,
		    sizeof (buf)));

		/* Send a kernel event so anyone can learn of the conflict */
		in_collision->link_data.if_family = ifp->if_family;
		in_collision->link_data.if_unit = ifp->if_unit;
		strlcpy(&in_collision->link_data.if_name[0],
		    ifp->if_name, IFNAMSIZ);
		in_collision->ia_ipaddr = sender_ip->sin_addr;
		in_collision->hw_len = (sender_hw->sdl_alen < MAX_HW_LEN) ?
		    sender_hw->sdl_alen : MAX_HW_LEN;
		bcopy(CONST_LLADDR(sender_hw), (caddr_t)in_collision->hw_addr,
		    in_collision->hw_len);
		ev_msg.vendor_code = KEV_VENDOR_APPLE;
		ev_msg.kev_class = KEV_NETWORK_CLASS;
		ev_msg.kev_subclass = KEV_INET_SUBCLASS;
		ev_msg.event_code = KEV_INET_ARPCOLLISION;
		ev_msg.dv[0].data_ptr = in_collision;
		ev_msg.dv[0].data_length =
		    sizeof (struct kev_in_collision) + in_collision->hw_len;
		ev_msg.dv[1].data_length = 0;
		kev_post_msg(&ev_msg);
		arpstat.dupips++;
		goto respond;
	}

	/*
	 * Look up the routing entry. If it doesn't exist and we are the
	 * target, and the sender isn't 0.0.0.0, go ahead and create one.
	 * Callee holds a reference on the route and returns with the route
	 * entry locked, upon success.
	 */
	error = arp_lookup_route(&sender_ip->sin_addr,
	    (target_ip->sin_addr.s_addr == best_ia_sin.sin_addr.s_addr &&
	    sender_ip->sin_addr.s_addr != 0), 0, &route, ifp->if_index);

	if (error == 0)
		RT_LOCK_ASSERT_HELD(route);

	if (error || route == NULL || route->rt_gateway == NULL) {
		if (arpop != ARPOP_REQUEST)
			goto respond;

		if (arp_sendllconflict && send_conflicting_probes != 0 &&
		    (ifp->if_eflags & IFEF_ARPLL) &&
		    IN_LINKLOCAL(ntohl(target_ip->sin_addr.s_addr)) &&
		    sender_ip->sin_addr.s_addr == INADDR_ANY) {
			/*
			 * Verify this ARP probe doesn't conflict with
			 * an IPv4LL we know of on another interface.
			 */
			if (route != NULL) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				route = NULL;
			}
			/*
			 * Callee holds a reference on the route and returns
			 * with the route entry locked, upon success.
			 */
			error = arp_lookup_route(&target_ip->sin_addr, 0, 0,
			    &route, ifp->if_index);

			if (error != 0 || route == NULL ||
			    route->rt_gateway == NULL)
				goto respond;

			RT_LOCK_ASSERT_HELD(route);

			gateway = SDL(route->rt_gateway);
			if (route->rt_ifp != ifp && gateway->sdl_alen != 0 &&
			    (gateway->sdl_alen != sender_hw->sdl_alen ||
			    bcmp(CONST_LLADDR(gateway), CONST_LLADDR(sender_hw),
			    gateway->sdl_alen) != 0)) {
				/*
				 * A node is probing for an IPv4LL we know
				 * exists on a different interface. We respond
				 * with a conflicting probe to force the new
				 * device to pick a different IPv4LL address.
				 */
				if (arp_verbose || log_arp_warnings) {
					log(LOG_INFO, "arp: %s on %s sent "
					    "probe for %s, already on %s\n",
					    sdl_addr_to_hex(sender_hw, buf,
					    sizeof (buf)), if_name(ifp),
					    inet_ntop(AF_INET,
					    &target_ip->sin_addr, ipv4str,
					    sizeof (ipv4str)),
					    if_name(route->rt_ifp));
					log(LOG_INFO, "arp: sending "
					    "conflicting probe to %s on %s\n",
					    sdl_addr_to_hex(sender_hw, buf,
					    sizeof (buf)), if_name(ifp));
				}
				/* Mark use timestamp */
				if (route->rt_llinfo != NULL)
					arp_llreach_use(route->rt_llinfo);
				/* We're done with the route */
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				route = NULL;
				/*
				 * Send a conservative unicast "ARP probe".
				 * This should force the other device to pick
				 * a new number.  This will not force the
				 * device to pick a new number if the device
				 * has already assigned that number.  This will
				 * not imply to the device that we own that
				 * address.  The link address is always
				 * present; it's never freed.
				 */
				ifnet_lock_shared(ifp);
				ifa = ifp->if_lladdr;
				IFA_ADDREF(ifa);
				ifnet_lock_done(ifp);
				dlil_send_arp_internal(ifp, ARPOP_REQUEST,
				    SDL(ifa->ifa_addr),
				    (const struct sockaddr *)sender_ip,
				    sender_hw,
				    (const struct sockaddr *)target_ip);
				IFA_REMREF(ifa);
				ifa = NULL;
				arpstat.txconflicts++;
			}
			goto respond;
		} else if (keep_announcements != 0 &&
		    target_ip->sin_addr.s_addr == sender_ip->sin_addr.s_addr) {
			/*
			 * Don't create entry if link-local address and
			 * link-local is disabled
			 */
			if (!IN_LINKLOCAL(ntohl(sender_ip->sin_addr.s_addr)) ||
			    (ifp->if_eflags & IFEF_ARPLL)) {
				if (route != NULL) {
					RT_REMREF_LOCKED(route);
					RT_UNLOCK(route);
					route = NULL;
				}
				/*
				 * Callee holds a reference on the route and
				 * returns with the route entry locked, upon
				 * success.
				 */
				error = arp_lookup_route(&sender_ip->sin_addr,
				    1, 0, &route, ifp->if_index);

				if (error == 0)
					RT_LOCK_ASSERT_HELD(route);

				if (error == 0 && route != NULL &&
				    route->rt_gateway != NULL)
					created_announcement = 1;
			}
			if (created_announcement == 0)
				goto respond;
		} else {
			goto respond;
		}
	}

	RT_LOCK_ASSERT_HELD(route);
	VERIFY(route->rt_expire == 0 || route->rt_rmx.rmx_expire != 0);
	VERIFY(route->rt_expire != 0 || route->rt_rmx.rmx_expire == 0);

	gateway = SDL(route->rt_gateway);
	if (!bridged && route->rt_ifp != ifp) {
		if (!IN_LINKLOCAL(ntohl(sender_ip->sin_addr.s_addr)) ||
		    !(ifp->if_eflags & IFEF_ARPLL)) {
			if (arp_verbose || log_arp_warnings)
				log(LOG_ERR, "arp: %s is on %s but got "
				    "reply from %s on %s\n",
				    inet_ntop(AF_INET, &sender_ip->sin_addr,
				    ipv4str, sizeof (ipv4str)),
				    if_name(route->rt_ifp),
				    sdl_addr_to_hex(sender_hw, buf,
				    sizeof (buf)), if_name(ifp));
			goto respond;
		} else {
			/* Don't change a permanent address */
			if (route->rt_expire == 0)
				goto respond;

			/*
			 * We're about to check and/or change the route's ifp
			 * and ifa, so do the lock dance: drop rt_lock, hold
			 * rnh_lock and re-hold rt_lock to avoid violating the
			 * lock ordering.  We have an extra reference on the
			 * route, so it won't go away while we do this.
			 */
			RT_UNLOCK(route);
			lck_mtx_lock(rnh_lock);
			RT_LOCK(route);
			/*
			 * Don't change the cloned route away from the
			 * parent's interface if the address did resolve
			 * or if the route is defunct.  rt_ifp on both
			 * the parent and the clone can now be freely
			 * accessed now that we have acquired rnh_lock.
			 */
			gateway = SDL(route->rt_gateway);
			if ((gateway->sdl_alen != 0 &&
			    route->rt_parent != NULL &&
			    route->rt_parent->rt_ifp == route->rt_ifp) ||
			    (route->rt_flags & RTF_CONDEMNED)) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				route = NULL;
				lck_mtx_unlock(rnh_lock);
				goto respond;
			}
			if (route->rt_ifp != ifp) {
				/*
				 * Purge any link-layer info caching.
				 */
				if (route->rt_llinfo_purge != NULL)
					route->rt_llinfo_purge(route);

				/* Adjust route ref count for the interfaces */
				if (route->rt_if_ref_fn != NULL) {
					route->rt_if_ref_fn(ifp, 1);
					route->rt_if_ref_fn(route->rt_ifp, -1);
				}
			}
			/* Change the interface when the existing route is on */
			route->rt_ifp = ifp;
			/*
			 * If rmx_mtu is not locked, update it
			 * to the MTU used by the new interface.
			 */
			if (!(route->rt_rmx.rmx_locks & RTV_MTU))
				route->rt_rmx.rmx_mtu = route->rt_ifp->if_mtu;

			rtsetifa(route, &best_ia->ia_ifa);
			gateway->sdl_index = ifp->if_index;
			RT_UNLOCK(route);
			lck_mtx_unlock(rnh_lock);
			RT_LOCK(route);
			/* Don't bother if the route is down */
			if (!(route->rt_flags & RTF_UP))
				goto respond;
			/* Refresh gateway pointer */
			gateway = SDL(route->rt_gateway);
		}
		RT_LOCK_ASSERT_HELD(route);
	}

	if (gateway->sdl_alen != 0 && bcmp(LLADDR(gateway),
	    CONST_LLADDR(sender_hw), gateway->sdl_alen) != 0) {
		if (route->rt_expire != 0 &&
		    (arp_verbose || log_arp_warnings)) {
			char buf2[3 * MAX_HW_LEN];
			log(LOG_INFO, "arp: %s moved from %s to %s on %s\n",
			    inet_ntop(AF_INET, &sender_ip->sin_addr, ipv4str,
			    sizeof (ipv4str)),
			    sdl_addr_to_hex(gateway, buf, sizeof (buf)),
			    sdl_addr_to_hex(sender_hw, buf2, sizeof (buf2)),
			    if_name(ifp));
		} else if (route->rt_expire == 0) {
			if (arp_verbose || log_arp_warnings) {
				log(LOG_ERR, "arp: %s attempts to modify "
				    "permanent entry for %s on %s\n",
				    sdl_addr_to_hex(sender_hw, buf,
				    sizeof (buf)),
				    inet_ntop(AF_INET, &sender_ip->sin_addr,
				    ipv4str, sizeof (ipv4str)),
				    if_name(ifp));
			}
			goto respond;
		}
	}

	/* Copy the sender hardware address in to the route's gateway address */
	gateway->sdl_alen = sender_hw->sdl_alen;
	bcopy(CONST_LLADDR(sender_hw), LLADDR(gateway), gateway->sdl_alen);

	/* Update the expire time for the route and clear the reject flag */
	if (route->rt_expire != 0)
		rt_setexpire(route, net_uptime() + arpt_keep);
	route->rt_flags &= ~RTF_REJECT;

	/* cache the gateway (sender HW) address */
	arp_llreach_alloc(route, ifp, LLADDR(gateway), gateway->sdl_alen,
	    (arpop == ARPOP_REPLY));

	llinfo = route->rt_llinfo;
	/* send a notification that the route is back up */
	if (ifp->if_addrlen == IF_LLREACH_MAXLEN &&
	    route->rt_flags & RTF_ROUTER && 
	    llinfo->la_flags & LLINFO_RTRFAIL_EVTSENT) {
		struct kev_msg ev_msg;
		struct kev_in_arpfailure in_arpalive;

		llinfo->la_flags &= ~LLINFO_RTRFAIL_EVTSENT;
		RT_UNLOCK(route);
		bzero(&ev_msg, sizeof(ev_msg));
		bzero(&in_arpalive, sizeof(in_arpalive));
		in_arpalive.link_data.if_family = ifp->if_family;
		in_arpalive.link_data.if_unit = ifp->if_unit;
		strlcpy(in_arpalive.link_data.if_name, ifp->if_name, IFNAMSIZ);
		ev_msg.vendor_code = KEV_VENDOR_APPLE;
		ev_msg.kev_class = KEV_NETWORK_CLASS;
		ev_msg.kev_subclass = KEV_INET_SUBCLASS;
		ev_msg.event_code = KEV_INET_ARPRTRALIVE;
		ev_msg.dv[0].data_ptr = &in_arpalive;
		ev_msg.dv[0].data_length = sizeof(struct kev_in_arpalive); 
		kev_post_msg(&ev_msg);
		RT_LOCK(route);
	}
	/* update the llinfo, send a queued packet if there is one */
	llinfo->la_asked = 0;
	if (llinfo->la_hold) {
		struct mbuf *m0 = llinfo->la_hold;
		llinfo->la_hold = NULL;
		RT_UNLOCK(route);
		dlil_output(ifp, PF_INET, m0, (caddr_t)route,
		    rt_key(route), 0, NULL);
		RT_REMREF(route);
		route = NULL;
	}


respond:
	if (route != NULL) {
		/* Mark use timestamp if we're going to send a reply */
		if (arpop == ARPOP_REQUEST && route->rt_llinfo != NULL)
			arp_llreach_use(route->rt_llinfo);
		RT_REMREF_LOCKED(route);
		RT_UNLOCK(route);
		route = NULL;
	}

	if (arpop != ARPOP_REQUEST)
		goto done;

	arpstat.rxrequests++;

	/* If we are not the target, check if we should proxy */
	if (target_ip->sin_addr.s_addr != best_ia_sin.sin_addr.s_addr) {
		/*
		 * Find a proxy route; callee holds a reference on the
		 * route and returns with the route entry locked, upon
		 * success.
		 */
		error = arp_lookup_route(&target_ip->sin_addr, 0, SIN_PROXY,
		    &route, ifp->if_index);

		if (error == 0) {
			RT_LOCK_ASSERT_HELD(route);
			/*
			 * Return proxied ARP replies only on the interface
			 * or bridge cluster where this network resides.
			 * Otherwise we may conflict with the host we are
			 * proxying for.
			 */
			if (route->rt_ifp != ifp &&
			    (route->rt_ifp->if_bridge != ifp->if_bridge ||
			    ifp->if_bridge == NULL)) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				goto done;
			}
			proxied = *SDL(route->rt_gateway);
			target_hw = &proxied;
		} else {
			/*
			 * We don't have a route entry indicating we should
			 * use proxy.  If we aren't supposed to proxy all,
			 * we are done.
			 */
			if (!arp_proxyall)
				goto done;

			/*
			 * See if we have a route to the target ip before
			 * we proxy it.
			 */
			route = rtalloc1_scoped((struct sockaddr *)
			    (size_t)target_ip, 0, 0, ifp->if_index);
			if (!route)
				goto done;

			/*
			 * Don't proxy for hosts already on the same interface.
			 */
			RT_LOCK(route);
			if (route->rt_ifp == ifp) {
				RT_UNLOCK(route);
				rtfree(route);
				goto done;
			}
		}
		/* Mark use timestamp */
		if (route->rt_llinfo != NULL)
			arp_llreach_use(route->rt_llinfo);
		RT_REMREF_LOCKED(route);
		RT_UNLOCK(route);
	}

	dlil_send_arp(ifp, ARPOP_REPLY,
	    target_hw, (const struct sockaddr *)target_ip,
	    sender_hw, (const struct sockaddr *)sender_ip, 0);

done:
	if (best_ia != NULL)
		IFA_REMREF(&best_ia->ia_ifa);
	return (0);
}

void
arp_ifinit(struct ifnet *ifp, struct ifaddr *ifa)
{
	struct sockaddr *sa;

	IFA_LOCK(ifa);
	ifa->ifa_rtrequest = arp_rtrequest;
	ifa->ifa_flags |= RTF_CLONING;
	sa = ifa->ifa_addr;
	IFA_UNLOCK(ifa);
	dlil_send_arp(ifp, ARPOP_REQUEST, NULL, sa, NULL, sa, 0);
}

static int
arp_getstat SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (req->oldptr == USER_ADDR_NULL)
		req->oldlen = (size_t)sizeof (struct arpstat);

	return (SYSCTL_OUT(req, &arpstat, MIN(sizeof (arpstat), req->oldlen)));
}
