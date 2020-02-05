/*	$NetBSD: res_send.c,v 1.9 2006/01/24 17:41:25 christos Exp $	*/

/*
 * Copyright (c) 1985, 1989, 1993
 *    The Regents of the University of California.  All rights reserved.
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
 * 	This product includes software developed by the University of
 * 	California, Berkeley and its contributors.
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
 */

/*
 * Portions Copyright (c) 1993 by Digital Equipment Corporation.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies, and that
 * the name of Digital Equipment Corporation not be used in advertising or
 * publicity pertaining to distribution of the document or software without
 * specific, written prior permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND DIGITAL EQUIPMENT CORP. DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL DIGITAL EQUIPMENT
 * CORPORATION BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Portions Copyright (c) 1996-1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Send query to name server and wait for reply.
 */

#define LOG_TAG "resolv"

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android/multinetwork.h>  // ResNsendFlags

#include <netdutils/Slice.h>
#include <netdutils/Stopwatch.h>
#include "DnsTlsDispatcher.h"
#include "DnsTlsTransport.h"
#include "PrivateDnsConfiguration.h"
#include "netd_resolv/resolv.h"
#include "private/android_filesystem_config.h"
#include "res_debug.h"
#include "res_init.h"
#include "resolv_cache.h"
#include "stats.h"
#include "stats.pb.h"
#include "util.h"

// TODO: use the namespace something like android::netd_resolv for libnetd_resolv
using android::net::CacheStatus;
using android::net::DnsQueryEvent;
using android::net::DnsTlsDispatcher;
using android::net::DnsTlsTransport;
using android::net::gPrivateDnsConfiguration;
using android::net::IpVersion;
using android::net::IV_IPV4;
using android::net::IV_IPV6;
using android::net::IV_UNKNOWN;
using android::net::NetworkDnsEventReported;
using android::net::NS_T_INVALID;
using android::net::NsRcode;
using android::net::NsType;
using android::net::PrivateDnsMode;
using android::net::PrivateDnsModes;
using android::net::PrivateDnsStatus;
using android::net::PROTO_TCP;
using android::net::PROTO_UDP;
using android::netdutils::IPSockAddr;
using android::netdutils::Slice;
using android::netdutils::Stopwatch;

static DnsTlsDispatcher sDnsTlsDispatcher;

static struct sockaddr* get_nsaddr(res_state, size_t);
static int send_vc(res_state, res_params* params, const uint8_t*, int, uint8_t*, int, int*, int,
                   time_t*, int*, int*);
static int send_dg(res_state, res_params* params, const uint8_t*, int, uint8_t*, int, int*, int,
                   int*, int*, time_t*, int*, int*);
static void dump_error(const char*, const struct sockaddr*, int);

static int sock_eq(struct sockaddr*, struct sockaddr*);
static int connect_with_timeout(int sock, const struct sockaddr* nsap, socklen_t salen,
                                const struct timespec timeout);
static int retrying_poll(const int sock, short events, const struct timespec* finish);
static int res_tls_send(res_state, const Slice query, const Slice answer, int* rcode,
                        bool* fallback);

NsType getQueryType(const uint8_t* msg, size_t msgLen) {
    ns_msg handle;
    ns_rr rr;
    if (ns_initparse((const uint8_t*)msg, msgLen, &handle) < 0 ||
        ns_parserr(&handle, ns_s_qd, 0, &rr) < 0) {
        return NS_T_INVALID;
    }
    return static_cast<NsType>(ns_rr_type(rr));
}

IpVersion ipFamilyToIPVersion(const int ipFamily) {
    switch (ipFamily) {
        case AF_INET:
            return IV_IPV4;
        case AF_INET6:
            return IV_IPV6;
        default:
            return IV_UNKNOWN;
    }
}

// BEGIN: Code copied from ISC eventlib
// TODO: move away from this code
#define BILLION 1000000000

static struct timespec evConsTime(time_t sec, long nsec) {
    struct timespec x;

    x.tv_sec = sec;
    x.tv_nsec = nsec;
    return (x);
}

static struct timespec evAddTime(struct timespec addend1, struct timespec addend2) {
    struct timespec x;

    x.tv_sec = addend1.tv_sec + addend2.tv_sec;
    x.tv_nsec = addend1.tv_nsec + addend2.tv_nsec;
    if (x.tv_nsec >= BILLION) {
        x.tv_sec++;
        x.tv_nsec -= BILLION;
    }
    return (x);
}

static struct timespec evSubTime(struct timespec minuend, struct timespec subtrahend) {
    struct timespec x;

    x.tv_sec = minuend.tv_sec - subtrahend.tv_sec;
    if (minuend.tv_nsec >= subtrahend.tv_nsec)
        x.tv_nsec = minuend.tv_nsec - subtrahend.tv_nsec;
    else {
        x.tv_nsec = BILLION - subtrahend.tv_nsec + minuend.tv_nsec;
        x.tv_sec--;
    }
    return (x);
}

static int evCmpTime(struct timespec a, struct timespec b) {
#define SGN(x) ((x) < 0 ? (-1) : (x) > 0 ? (1) : (0));
    time_t s = a.tv_sec - b.tv_sec;
    long n;

    if (s != 0) return SGN(s);

    n = a.tv_nsec - b.tv_nsec;
    return SGN(n);
}

static struct timespec evNowTime(void) {
    struct timespec tsnow;
    clock_gettime(CLOCK_REALTIME, &tsnow);
    return tsnow;
}

// END: Code copied from ISC eventlib

/* BIONIC-BEGIN: implement source port randomization */
static int random_bind(int s, int family) {
    sockaddr_union u;
    int j;
    socklen_t slen;

    /* clear all, this also sets the IP4/6 address to 'any' */
    memset(&u, 0, sizeof u);

    switch (family) {
        case AF_INET:
            u.sin.sin_family = family;
            slen = sizeof u.sin;
            break;
        case AF_INET6:
            u.sin6.sin6_family = family;
            slen = sizeof u.sin6;
            break;
        default:
            errno = EPROTO;
            return -1;
    }

    /* first try to bind to a random source port a few times */
    for (j = 0; j < 10; j++) {
        /* find a random port between 1025 .. 65534 */
        int port = 1025 + (arc4random_uniform(65535 - 1025));
        if (family == AF_INET)
            u.sin.sin_port = htons(port);
        else
            u.sin6.sin6_port = htons(port);

        if (!bind(s, &u.sa, slen)) return 0;
    }

    // nothing after 10 attempts, our network table is probably busy
    // let the system decide which port is best
    if (family == AF_INET)
        u.sin.sin_port = 0;
    else
        u.sin6.sin6_port = 0;

    return bind(s, &u.sa, slen);
}
/* BIONIC-END */

// Disables all nameservers other than selectedServer
static void res_set_usable_server(int selectedServer, int nscount, bool usable_servers[]) {
    int usableIndex = 0;
    for (int ns = 0; ns < nscount; ns++) {
        if (usable_servers[ns]) ++usableIndex;
        if (usableIndex != selectedServer) usable_servers[ns] = false;
    }
}

// Looks up the nameserver address in res.nsaddrs[], returns true if found, otherwise false.
static bool res_ourserver_p(res_state statp, const sockaddr* sa) {
    const sockaddr_in *inp, *srv;
    const sockaddr_in6 *in6p, *srv6;
    int ns;

    switch (sa->sa_family) {
        case AF_INET:
            inp = (const struct sockaddr_in*) (const void*) sa;
            for (ns = 0; ns < statp->nscount; ns++) {
                srv = (struct sockaddr_in*) (void*) get_nsaddr(statp, (size_t) ns);
                if (srv->sin_family == inp->sin_family && srv->sin_port == inp->sin_port &&
                    (srv->sin_addr.s_addr == INADDR_ANY ||
                     srv->sin_addr.s_addr == inp->sin_addr.s_addr))
                    return true;
            }
            break;
        case AF_INET6:
            in6p = (const struct sockaddr_in6*) (const void*) sa;
            for (ns = 0; ns < statp->nscount; ns++) {
                srv6 = (struct sockaddr_in6*) (void*) get_nsaddr(statp, (size_t) ns);
                if (srv6->sin6_family == in6p->sin6_family && srv6->sin6_port == in6p->sin6_port &&
#ifdef HAVE_SIN6_SCOPE_ID
                    (srv6->sin6_scope_id == 0 || srv6->sin6_scope_id == in6p->sin6_scope_id) &&
#endif
                    (IN6_IS_ADDR_UNSPECIFIED(&srv6->sin6_addr) ||
                     IN6_ARE_ADDR_EQUAL(&srv6->sin6_addr, &in6p->sin6_addr)))
                    return true;
            }
            break;
        default:
            break;
    }
    return false;
}

/* int
 * res_nameinquery(name, type, cl, buf, eom)
 *	look for (name, type, cl) in the query section of packet (buf, eom)
 * requires:
 *	buf + HFIXEDSZ <= eom
 * returns:
 *	-1 : format error
 *	0  : not found
 *	>0 : found
 * author:
 *	paul vixie, 29may94
 */
int res_nameinquery(const char* name, int type, int cl, const uint8_t* buf, const uint8_t* eom) {
    const uint8_t* cp = buf + HFIXEDSZ;
    int qdcount = ntohs(((const HEADER*) (const void*) buf)->qdcount);

    while (qdcount-- > 0) {
        char tname[MAXDNAME + 1];
        int n = dn_expand(buf, eom, cp, tname, sizeof tname);
        if (n < 0) return (-1);
        cp += n;
        if (cp + 2 * INT16SZ > eom) return (-1);
        int ttype = ntohs(*reinterpret_cast<const uint16_t*>(cp));
        cp += INT16SZ;
        int tclass = ntohs(*reinterpret_cast<const uint16_t*>(cp));
        cp += INT16SZ;
        if (ttype == type && tclass == cl && ns_samename(tname, name) == 1) return (1);
    }
    return (0);
}

/* int
 * res_queriesmatch(buf1, eom1, buf2, eom2)
 *	is there a 1:1 mapping of (name,type,class)
 *	in (buf1,eom1) and (buf2,eom2)?
 * returns:
 *	-1 : format error
 *	0  : not a 1:1 mapping
 *	>0 : is a 1:1 mapping
 * author:
 *	paul vixie, 29may94
 */
int res_queriesmatch(const uint8_t* buf1, const uint8_t* eom1, const uint8_t* buf2,
                     const uint8_t* eom2) {
    const uint8_t* cp = buf1 + HFIXEDSZ;
    int qdcount = ntohs(((const HEADER*) (const void*) buf1)->qdcount);

    if (buf1 + HFIXEDSZ > eom1 || buf2 + HFIXEDSZ > eom2) return (-1);

    /*
     * Only header section present in replies to
     * dynamic update packets.
     */
    if ((((const HEADER*) (const void*) buf1)->opcode == ns_o_update) &&
        (((const HEADER*) (const void*) buf2)->opcode == ns_o_update))
        return (1);

    if (qdcount != ntohs(((const HEADER*) (const void*) buf2)->qdcount)) return (0);
    while (qdcount-- > 0) {
        char tname[MAXDNAME + 1];
        int n = dn_expand(buf1, eom1, cp, tname, sizeof tname);
        if (n < 0) return (-1);
        cp += n;
        if (cp + 2 * INT16SZ > eom1) return (-1);
        int ttype = ntohs(*reinterpret_cast<const uint16_t*>(cp));
        cp += INT16SZ;
        int tclass = ntohs(*reinterpret_cast<const uint16_t*>(cp));
        cp += INT16SZ;
        if (!res_nameinquery(tname, ttype, tclass, buf2, eom2)) return (0);
    }
    return (1);
}

static DnsQueryEvent* addDnsQueryEvent(NetworkDnsEventReported* event) {
    return event->mutable_dns_query_events()->add_dns_query_event();
}

int res_nsend(res_state statp, const uint8_t* buf, int buflen, uint8_t* ans, int anssiz, int* rcode,
              uint32_t flags) {
    LOG(DEBUG) << __func__;

    // Should not happen
    if (anssiz < HFIXEDSZ) {
        // TODO: Remove errno once callers stop using it
        errno = EINVAL;
        return -EINVAL;
    }
    res_pquery(buf, buflen);

    int anslen = 0;
    Stopwatch cacheStopwatch;
    ResolvCacheStatus cache_status =
            resolv_cache_lookup(statp->netid, buf, buflen, ans, anssiz, &anslen, flags);
    const int32_t cacheLatencyUs = saturate_cast<int32_t>(cacheStopwatch.timeTakenUs());
    if (cache_status == RESOLV_CACHE_FOUND) {
        HEADER* hp = (HEADER*)(void*)ans;
        *rcode = hp->rcode;
        DnsQueryEvent* dnsQueryEvent = addDnsQueryEvent(statp->event);
        dnsQueryEvent->set_latency_micros(cacheLatencyUs);
        dnsQueryEvent->set_cache_hit(static_cast<CacheStatus>(cache_status));
        dnsQueryEvent->set_type(getQueryType(buf, buflen));
        return anslen;
    } else if (cache_status != RESOLV_CACHE_UNSUPPORTED) {
        // had a cache miss for a known network, so populate the thread private
        // data so the normal resolve path can do its thing
        resolv_populate_res_for_net(statp);
    }
    if (statp->nscount == 0) {
        // We have no nameservers configured, so there's no point trying.
        // Tell the cache the query failed, or any retries and anyone else asking the same
        // question will block for PENDING_REQUEST_TIMEOUT seconds instead of failing fast.
        _resolv_cache_query_failed(statp->netid, buf, buflen, flags);

        // TODO: Remove errno once callers stop using it
        errno = ESRCH;
        return -ESRCH;
    }

    // DoT
    if (!(statp->netcontext_flags & NET_CONTEXT_FLAG_USE_LOCAL_NAMESERVERS)) {
        bool fallback = false;
        int resplen = res_tls_send(statp, Slice(const_cast<uint8_t*>(buf), buflen),
                                   Slice(ans, anssiz), rcode, &fallback);
        if (resplen > 0) {
            LOG(DEBUG) << __func__ << ": got answer from DoT";
            res_pquery(ans, resplen);
            if (cache_status == RESOLV_CACHE_NOTFOUND) {
                resolv_cache_add(statp->netid, buf, buflen, ans, resplen);
            }
            return resplen;
        }
        if (!fallback) {
            _resolv_cache_query_failed(statp->netid, buf, buflen, flags);
            return -ETIMEDOUT;
        }
    }

    res_stats stats[MAXNS];
    res_params params;
    int revision_id = resolv_cache_get_resolver_stats(statp->netid, &params, stats);
    if (revision_id < 0) {
        // TODO: Remove errno once callers stop using it
        errno = ESRCH;
        return -ESRCH;
    }
    bool usable_servers[MAXNS];
    int usableServersCount = android_net_res_stats_get_usable_servers(
            &params, stats, statp->nscount, usable_servers);

    if ((flags & ANDROID_RESOLV_NO_RETRY) && usableServersCount > 1) {
        auto hp = reinterpret_cast<const HEADER*>(buf);

        // Select a random server based on the query id
        int selectedServer = (hp->id % usableServersCount) + 1;
        res_set_usable_server(selectedServer, statp->nscount, usable_servers);
    }

    // Send request, RETRY times, or until successful.
    int retryTimes = (flags & ANDROID_RESOLV_NO_RETRY) ? 1 : params.retry_count;
    int useTcp = buflen > PACKETSZ;
    int gotsomewhere = 0;
    int terrno = ETIMEDOUT;

    for (int attempt = 0; attempt < retryTimes; ++attempt) {
        for (int ns = 0; ns < statp->nscount; ++ns) {
            if (!usable_servers[ns]) continue;

            *rcode = RCODE_INTERNAL_ERROR;

            // Get server addr
            const sockaddr* nsap = get_nsaddr(statp, ns);
            const int nsaplen = sockaddrSize(nsap);

            static const int niflags = NI_NUMERICHOST | NI_NUMERICSERV;
            char abuf[NI_MAXHOST];
            if (getnameinfo(nsap, (socklen_t)nsaplen, abuf, sizeof(abuf), NULL, 0, niflags) == 0)
                LOG(DEBUG) << __func__ << ": Querying server (# " << ns + 1
                           << ") address = " << abuf;

            ::android::net::Protocol query_proto = useTcp ? PROTO_TCP : PROTO_UDP;
            time_t now = 0;
            int delay = 0;
            bool fallbackTCP = false;
            const bool shouldRecordStats = (attempt == 0);
            int resplen;
            Stopwatch queryStopwatch;
            if (useTcp) {
                // TCP; at most one attempt per server.
                attempt = retryTimes;
                resplen = send_vc(statp, &params, buf, buflen, ans, anssiz, &terrno, ns, &now,
                                  rcode, &delay);

                if (buflen <= PACKETSZ && resplen <= 0 &&
                    statp->tc_mode == aidl::android::net::IDnsResolver::TC_MODE_UDP_TCP) {
                    // reset to UDP for next query on next DNS server if resolver is currently doing
                    // TCP fallback retry and current server does not support TCP connectin
                    useTcp = false;
                }
                LOG(INFO) << __func__ << ": used send_vc " << resplen;
            } else {
                // UDP
                resplen = send_dg(statp, &params, buf, buflen, ans, anssiz, &terrno, ns, &useTcp,
                                  &gotsomewhere, &now, rcode, &delay);
                fallbackTCP = useTcp ? true : false;
                LOG(INFO) << __func__ << ": used send_dg " << resplen;
            }

            DnsQueryEvent* dnsQueryEvent = addDnsQueryEvent(statp->event);
            dnsQueryEvent->set_cache_hit(static_cast<CacheStatus>(cache_status));
            dnsQueryEvent->set_latency_micros(saturate_cast<int32_t>(queryStopwatch.timeTakenUs()));
            dnsQueryEvent->set_dns_server_index(ns);
            dnsQueryEvent->set_ip_version(ipFamilyToIPVersion(nsap->sa_family));
            dnsQueryEvent->set_retry_times(attempt);
            dnsQueryEvent->set_rcode(static_cast<NsRcode>(*rcode));
            dnsQueryEvent->set_protocol(query_proto);
            dnsQueryEvent->set_type(getQueryType(buf, buflen));

            // Only record stats the first time we try a query. This ensures that
            // queries that deterministically fail (e.g., a name that always returns
            // SERVFAIL or times out) do not unduly affect the stats.
            if (shouldRecordStats) {
                res_sample sample;
                _res_stats_set_sample(&sample, now, *rcode, delay);
                resolv_cache_add_resolver_stats_sample(statp->netid, revision_id, nsap, sample,
                                                       params.max_samples);
                resolv_stats_add(statp->netid, IPSockAddr::toIPSockAddr(*nsap), dnsQueryEvent);
            }

            if (resplen == 0) continue;
            if (fallbackTCP) {
                ns--;
                continue;
            }
            if (resplen < 0) {
                _resolv_cache_query_failed(statp->netid, buf, buflen, flags);
                res_nclose(statp);
                return -terrno;
            };

            LOG(DEBUG) << __func__ << ": got answer:";
            res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);

            if (cache_status == RESOLV_CACHE_NOTFOUND) {
                resolv_cache_add(statp->netid, buf, buflen, ans, resplen);
            }
            res_nclose(statp);
            return (resplen);
        }  // for each ns
    }  // for each retry
    res_nclose(statp);
    terrno = useTcp ? terrno : gotsomewhere ? ETIMEDOUT : ECONNREFUSED;
    // TODO: Remove errno once callers stop using it
    errno = useTcp ? terrno
                   : gotsomewhere ? ETIMEDOUT /* no answer obtained */
                                  : ECONNREFUSED /* no nameservers found */;

    _resolv_cache_query_failed(statp->netid, buf, buflen, flags);
    return -terrno;
}

/* Private */

static struct sockaddr* get_nsaddr(res_state statp, size_t n) {
    return (struct sockaddr*)(void*)&statp->nsaddrs[n];
}

static struct timespec get_timeout(res_state statp, const res_params* params, const int ns) {
    int msec;
    // Legacy algorithm which scales the timeout by nameserver number.
    // For instance, with 4 nameservers: 5s, 2.5s, 5s, 10s
    // This has no effect with 1 or 2 nameservers
    msec = params->base_timeout_msec << ns;
    if (ns > 0) {
        msec /= statp->nscount;
    }
    // For safety, don't allow OEMs and experiments to configure a timeout shorter than 1s.
    if (msec < 1000) {
        msec = 1000;  // Use at least 1000ms
    }
    LOG(INFO) << __func__ << ": using timeout of " << msec << " msec";

    struct timespec result;
    result.tv_sec = msec / 1000;
    result.tv_nsec = (msec % 1000) * 1000000;
    return result;
}

static int send_vc(res_state statp, res_params* params, const uint8_t* buf, int buflen,
                   uint8_t* ans, int anssiz, int* terrno, int ns, time_t* at, int* rcode,
                   int* delay) {
    *at = time(NULL);
    *delay = 0;
    const HEADER* hp = (const HEADER*) (const void*) buf;
    HEADER* anhp = (HEADER*) (void*) ans;
    struct sockaddr* nsap;
    int nsaplen;
    int truncating, connreset, n;
    uint8_t* cp;

    LOG(INFO) << __func__ << ": using send_vc";

    nsap = get_nsaddr(statp, (size_t) ns);
    nsaplen = sockaddrSize(nsap);

    connreset = 0;
same_ns:
    truncating = 0;

    struct timespec now = evNowTime();

    /* Are we still talking to whom we want to talk to? */
    if (statp->_vcsock >= 0 && (statp->_flags & RES_F_VC) != 0) {
        struct sockaddr_storage peer;
        socklen_t size = sizeof peer;
        unsigned old_mark;
        socklen_t mark_size = sizeof(old_mark);
        if (getpeername(statp->_vcsock, (struct sockaddr*) (void*) &peer, &size) < 0 ||
            !sock_eq((struct sockaddr*) (void*) &peer, nsap) ||
            getsockopt(statp->_vcsock, SOL_SOCKET, SO_MARK, &old_mark, &mark_size) < 0 ||
            old_mark != statp->_mark) {
            res_nclose(statp);
            statp->_flags &= ~RES_F_VC;
        }
    }

    if (statp->_vcsock < 0 || (statp->_flags & RES_F_VC) == 0) {
        if (statp->_vcsock >= 0) res_nclose(statp);

        statp->_vcsock = socket(nsap->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (statp->_vcsock < 0) {
            switch (errno) {
                case EPROTONOSUPPORT:
                case EPFNOSUPPORT:
                case EAFNOSUPPORT:
                    PLOG(DEBUG) << __func__ << ": socket(vc): ";
                    return 0;
                default:
                    *terrno = errno;
                    PLOG(DEBUG) << __func__ << ": socket(vc): ";
                    return -1;
            }
        }
        resolv_tag_socket(statp->_vcsock, statp->uid, statp->pid);
        if (statp->_mark != MARK_UNSET) {
            if (setsockopt(statp->_vcsock, SOL_SOCKET, SO_MARK, &statp->_mark,
                           sizeof(statp->_mark)) < 0) {
                *terrno = errno;
                PLOG(DEBUG) << __func__ << ": setsockopt: ";
                return -1;
            }
        }
        errno = 0;
        if (random_bind(statp->_vcsock, nsap->sa_family) < 0) {
            *terrno = errno;
            dump_error("bind/vc", nsap, nsaplen);
            res_nclose(statp);
            return (0);
        }
        if (connect_with_timeout(statp->_vcsock, nsap, (socklen_t) nsaplen,
                                 get_timeout(statp, params, ns)) < 0) {
            *terrno = errno;
            dump_error("connect/vc", nsap, nsaplen);
            res_nclose(statp);
            /*
             * The way connect_with_timeout() is implemented prevents us from reliably
             * determining whether this was really a timeout or e.g. ECONNREFUSED. Since
             * currently both cases are handled in the same way, there is no need to
             * change this (yet). If we ever need to reliably distinguish between these
             * cases, both connect_with_timeout() and retrying_poll() need to be
             * modified, though.
             */
            *rcode = RCODE_TIMEOUT;
            return (0);
        }
        statp->_flags |= RES_F_VC;
    }

    /*
     * Send length & message
     */
    uint16_t len = htons(static_cast<uint16_t>(buflen));
    const iovec iov[] = {
            {.iov_base = &len, .iov_len = INT16SZ},
            {.iov_base = const_cast<uint8_t*>(buf), .iov_len = static_cast<size_t>(buflen)},
    };
    if (writev(statp->_vcsock, iov, 2) != (INT16SZ + buflen)) {
        *terrno = errno;
        PLOG(DEBUG) << __func__ << ": write failed: ";
        res_nclose(statp);
        return (0);
    }
    /*
     * Receive length & response
     */
read_len:
    cp = ans;
    len = INT16SZ;
    while ((n = read(statp->_vcsock, (char*) cp, (size_t) len)) > 0) {
        cp += n;
        if ((len -= n) == 0) break;
    }
    if (n <= 0) {
        *terrno = errno;
        PLOG(DEBUG) << __func__ << ": read failed: ";
        res_nclose(statp);
        /*
         * A long running process might get its TCP
         * connection reset if the remote server was
         * restarted.  Requery the server instead of
         * trying a new one.  When there is only one
         * server, this means that a query might work
         * instead of failing.  We only allow one reset
         * per query to prevent looping.
         */
        if (*terrno == ECONNRESET && !connreset) {
            connreset = 1;
            res_nclose(statp);
            goto same_ns;
        }
        res_nclose(statp);
        return (0);
    }
    uint16_t resplen = ntohs(*reinterpret_cast<const uint16_t*>(ans));
    if (resplen > anssiz) {
        LOG(DEBUG) << __func__ << ": response truncated";
        truncating = 1;
        len = anssiz;
    } else
        len = resplen;
    if (len < HFIXEDSZ) {
        /*
         * Undersized message.
         */
        LOG(DEBUG) << __func__ << ": undersized: " << len;
        *terrno = EMSGSIZE;
        res_nclose(statp);
        return (0);
    }
    cp = ans;
    while (len != 0 && (n = read(statp->_vcsock, (char*) cp, (size_t) len)) > 0) {
        cp += n;
        len -= n;
    }
    if (n <= 0) {
        *terrno = errno;
        PLOG(DEBUG) << __func__ << ": read(vc): ";
        res_nclose(statp);
        return (0);
    }

    if (truncating) {
        /*
         * Flush rest of answer so connection stays in synch.
         */
        anhp->tc = 1;
        len = resplen - anssiz;
        while (len != 0) {
            char junk[PACKETSZ];

            n = read(statp->_vcsock, junk, (len > sizeof junk) ? sizeof junk : len);
            if (n > 0)
                len -= n;
            else
                break;
        }
    }
    /*
     * If the calling application has bailed out of
     * a previous call and failed to arrange to have
     * the circuit closed or the server has got
     * itself confused, then drop the packet and
     * wait for the correct one.
     */
    if (hp->id != anhp->id) {
        LOG(DEBUG) << __func__ << ": ld answer (unexpected):";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        goto read_len;
    }

    /*
     * All is well, or the error is fatal.  Signal that the
     * next nameserver ought not be tried.
     */
    if (resplen > 0) {
        struct timespec done = evNowTime();
        *delay = _res_stats_calculate_rtt(&done, &now);
        *rcode = anhp->rcode;
    }
    return (resplen);
}

/* return -1 on error (errno set), 0 on success */
static int connect_with_timeout(int sock, const sockaddr* nsap, socklen_t salen,
                                const timespec timeout) {
    int res, origflags;

    origflags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, origflags | O_NONBLOCK);

    res = connect(sock, nsap, salen);
    if (res < 0 && errno != EINPROGRESS) {
        res = -1;
        goto done;
    }
    if (res != 0) {
        timespec now = evNowTime();
        timespec finish = evAddTime(now, timeout);
        LOG(INFO) << __func__ << ": " << sock << " send_vc";
        res = retrying_poll(sock, POLLIN | POLLOUT, &finish);
        if (res <= 0) {
            res = -1;
        }
    }
done:
    fcntl(sock, F_SETFL, origflags);
    LOG(INFO) << __func__ << ": " << sock << " connect_with_const timeout returning " << res;
    return res;
}

static int retrying_poll(const int sock, const short events, const struct timespec* finish) {
    struct timespec now, timeout;

retry:
    LOG(INFO) << __func__ << ": " << sock << " retrying_poll";

    now = evNowTime();
    if (evCmpTime(*finish, now) > 0)
        timeout = evSubTime(*finish, now);
    else
        timeout = evConsTime(0L, 0L);
    struct pollfd fds = {.fd = sock, .events = events};
    int n = ppoll(&fds, 1, &timeout, /*sigmask=*/NULL);
    if (n == 0) {
        LOG(INFO) << __func__ << ": " << sock << "retrying_poll timeout";
        errno = ETIMEDOUT;
        return 0;
    }
    if (n < 0) {
        if (errno == EINTR) goto retry;
        PLOG(INFO) << __func__ << ": " << sock << " retrying_poll failed";
        return n;
    }
    if (fds.revents & (POLLIN | POLLOUT | POLLERR)) {
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            errno = error;
            PLOG(INFO) << __func__ << ": " << sock << " retrying_poll getsockopt failed";
            return -1;
        }
    }
    LOG(INFO) << __func__ << ": " << sock << " retrying_poll returning " << n;
    return n;
}

static int send_dg(res_state statp, res_params* params, const uint8_t* buf, int buflen,
                   uint8_t* ans, int anssiz, int* terrno, int ns, int* v_circuit, int* gotsomewhere,
                   time_t* at, int* rcode, int* delay) {
    *at = time(NULL);
    *delay = 0;
    const HEADER* hp = (const HEADER*) (const void*) buf;
    HEADER* anhp = (HEADER*) (void*) ans;
    const struct sockaddr* nsap;
    int nsaplen;
    struct timespec now, timeout, finish, done;
    struct sockaddr_storage from;
    socklen_t fromlen;
    int resplen, n, s;

    nsap = get_nsaddr(statp, (size_t) ns);
    nsaplen = sockaddrSize(nsap);
    if (statp->nssocks[ns] == -1) {
        statp->nssocks[ns] = socket(nsap->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (statp->nssocks[ns] < 0) {
            switch (errno) {
                case EPROTONOSUPPORT:
                case EPFNOSUPPORT:
                case EAFNOSUPPORT:
                    PLOG(DEBUG) << __func__ << ": socket(dg): ";
                    return (0);
                default:
                    *terrno = errno;
                    PLOG(DEBUG) << __func__ << ": socket(dg): ";
                    return (-1);
            }
        }

        resolv_tag_socket(statp->nssocks[ns], statp->uid, statp->pid);
        if (statp->_mark != MARK_UNSET) {
            if (setsockopt(statp->nssocks[ns], SOL_SOCKET, SO_MARK, &(statp->_mark),
                           sizeof(statp->_mark)) < 0) {
                res_nclose(statp);
                return -1;
            }
        }
        // Use a "connected" datagram socket to receive an ECONNREFUSED error
        // on the next socket operation when the server responds with an
        // ICMP port-unreachable error. This way we can detect the absence of
        // a nameserver without timing out.
        if (random_bind(statp->nssocks[ns], nsap->sa_family) < 0) {
            dump_error("bind(dg)", nsap, nsaplen);
            res_nclose(statp);
            return (0);
        }
        if (connect(statp->nssocks[ns], nsap, (socklen_t)nsaplen) < 0) {
            dump_error("connect(dg)", nsap, nsaplen);
            res_nclose(statp);
            return (0);
        }
        LOG(DEBUG) << __func__ << ": new DG socket";
    }
    s = statp->nssocks[ns];
    if (send(s, (const char*) buf, (size_t) buflen, 0) != buflen) {
        PLOG(DEBUG) << __func__ << ": send: ";
        res_nclose(statp);
        return 0;
    }

    // Wait for reply.
    timeout = get_timeout(statp, params, ns);
    now = evNowTime();
    finish = evAddTime(now, timeout);
retry:
    n = retrying_poll(s, POLLIN, &finish);

    if (n == 0) {
        *rcode = RCODE_TIMEOUT;
        LOG(DEBUG) << __func__ << ": timeout";
        *gotsomewhere = 1;
        return 0;
    }
    if (n < 0) {
        PLOG(DEBUG) << __func__ << ": poll: ";
        res_nclose(statp);
        return 0;
    }
    errno = 0;
    fromlen = sizeof(from);
    resplen = recvfrom(s, (char*) ans, (size_t) anssiz, 0, (struct sockaddr*) (void*) &from,
                       &fromlen);
    if (resplen <= 0) {
        PLOG(DEBUG) << __func__ << ": recvfrom: ";
        res_nclose(statp);
        return 0;
    }
    *gotsomewhere = 1;
    if (resplen < HFIXEDSZ) {
        /*
         * Undersized message.
         */
        LOG(DEBUG) << __func__ << ": undersized: " << resplen;
        *terrno = EMSGSIZE;
        res_nclose(statp);
        return 0;
    }
    if (hp->id != anhp->id) {
        /*
         * response from old query, ignore it.
         * XXX - potential security hazard could
         *	 be detected here.
         */
        LOG(DEBUG) << __func__ << ": old answer:";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        goto retry;
    }
    if (!res_ourserver_p(statp, (struct sockaddr*)(void*)&from)) {
        /*
         * response from wrong server? ignore it.
         * XXX - potential security hazard could
         *	 be detected here.
         */
        LOG(DEBUG) << __func__ << ": not our server:";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        goto retry;
    }
    if (anhp->rcode == FORMERR && (statp->netcontext_flags & NET_CONTEXT_FLAG_USE_EDNS)) {
        /*
         * Do not retry if the server do not understand EDNS0.
         * The case has to be captured here, as FORMERR packet do not
         * carry query section, hence res_queriesmatch() returns 0.
         */
        LOG(DEBUG) << __func__ << ": server rejected query with EDNS0:";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        /* record the error */
        statp->_flags |= RES_F_EDNS0ERR;
        res_nclose(statp);
        return 0;
    }
    if (!res_queriesmatch(buf, buf + buflen, ans, ans + anssiz)) {
        /*
         * response contains wrong query? ignore it.
         * XXX - potential security hazard could
         *	 be detected here.
         */
        LOG(DEBUG) << __func__ << ": wrong query name:";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        goto retry;
    }
    done = evNowTime();
    *delay = _res_stats_calculate_rtt(&done, &now);
    if (anhp->rcode == SERVFAIL || anhp->rcode == NOTIMP || anhp->rcode == REFUSED) {
        LOG(DEBUG) << __func__ << ": server rejected query:";
        res_pquery(ans, (resplen > anssiz) ? anssiz : resplen);
        res_nclose(statp);
        *rcode = anhp->rcode;
        return 0;
    }
    if (anhp->tc) {
        /*
         * To get the rest of answer,
         * use TCP with same server.
         */
        LOG(DEBUG) << __func__ << ": truncated answer";
        *v_circuit = 1;
        res_nclose(statp);
        return 1;
    }
    /*
     * All is well, or the error is fatal.  Signal that the
     * next nameserver ought not be tried.
     */
    if (resplen > 0) {
        *rcode = anhp->rcode;
    }
    return resplen;
}

static void dump_error(const char* str, const struct sockaddr* address, int alen) {
    char hbuf[NI_MAXHOST];
    char sbuf[NI_MAXSERV];
    constexpr int niflags = NI_NUMERICHOST | NI_NUMERICSERV;
    const int err = errno;

    if (!WOULD_LOG(DEBUG)) return;

    if (getnameinfo(address, (socklen_t)alen, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), niflags)) {
        strncpy(hbuf, "?", sizeof(hbuf) - 1);
        hbuf[sizeof(hbuf) - 1] = '\0';
        strncpy(sbuf, "?", sizeof(sbuf) - 1);
        sbuf[sizeof(sbuf) - 1] = '\0';
    }
    errno = err;
    PLOG(DEBUG) << __func__ << ": " << str << " ([" << hbuf << "]." << sbuf << "): ";
}

static int sock_eq(struct sockaddr* a, struct sockaddr* b) {
    struct sockaddr_in *a4, *b4;
    struct sockaddr_in6 *a6, *b6;

    if (a->sa_family != b->sa_family) return 0;
    switch (a->sa_family) {
        case AF_INET:
            a4 = (struct sockaddr_in*) (void*) a;
            b4 = (struct sockaddr_in*) (void*) b;
            return a4->sin_port == b4->sin_port && a4->sin_addr.s_addr == b4->sin_addr.s_addr;
        case AF_INET6:
            a6 = (struct sockaddr_in6*) (void*) a;
            b6 = (struct sockaddr_in6*) (void*) b;
            return a6->sin6_port == b6->sin6_port &&
#ifdef HAVE_SIN6_SCOPE_ID
                   a6->sin6_scope_id == b6->sin6_scope_id &&
#endif
                   IN6_ARE_ADDR_EQUAL(&a6->sin6_addr, &b6->sin6_addr);
        default:
            return 0;
    }
}

PrivateDnsModes convertEnumType(PrivateDnsMode privateDnsmode) {
    switch (privateDnsmode) {
        case PrivateDnsMode::OFF:
            return PrivateDnsModes::PDM_OFF;
        case PrivateDnsMode::OPPORTUNISTIC:
            return PrivateDnsModes::PDM_OPPORTUNISTIC;
        case PrivateDnsMode::STRICT:
            return PrivateDnsModes::PDM_STRICT;
        default:
            return PrivateDnsModes::PDM_UNKNOWN;
    }
}

static int res_tls_send(res_state statp, const Slice query, const Slice answer, int* rcode,
                        bool* fallback) {
    int resplen = 0;
    const unsigned netId = statp->netid;

    PrivateDnsStatus privateDnsStatus = gPrivateDnsConfiguration.getStatus(netId);
    statp->event->set_private_dns_modes(convertEnumType(privateDnsStatus.mode));

    if (privateDnsStatus.mode == PrivateDnsMode::OFF) {
        *fallback = true;
        return -1;
    }

    if (privateDnsStatus.validatedServers().empty()) {
        if (privateDnsStatus.mode == PrivateDnsMode::OPPORTUNISTIC) {
            *fallback = true;
            return -1;
        } else {
            // Sleep and iterate some small number of times checking for the
            // arrival of resolved and validated server IP addresses, instead
            // of returning an immediate error.
            // This is needed because as soon as a network becomes the default network, apps will
            // send DNS queries on that network. If no servers have yet validated, and we do not
            // block those queries, they would immediately fail, causing application-visible errors.
            // Note that this can happen even before the network validates, since an unvalidated
            // network can become the default network if no validated networks are available.
            //
            // TODO: see if there is a better way to address this problem, such as buffering the
            // queries in a queue or only blocking queries for the first few seconds after a default
            // network change.
            for (int i = 0; i < 42; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // Calling getStatus() to merely check if there's any validated server seems
                // wasteful. Consider adding a new method in PrivateDnsConfiguration for speed ups.
                if (!gPrivateDnsConfiguration.getStatus(netId).validatedServers().empty()) {
                    privateDnsStatus = gPrivateDnsConfiguration.getStatus(netId);
                    break;
                }
            }
            if (privateDnsStatus.validatedServers().empty()) {
                return -1;
            }
        }
    }

    LOG(INFO) << __func__ << ": performing query over TLS";

    const auto response = sDnsTlsDispatcher.query(privateDnsStatus.validatedServers(), statp, query,
                                                  answer, &resplen);

    LOG(INFO) << __func__ << ": TLS query result: " << static_cast<int>(response);

    if (privateDnsStatus.mode == PrivateDnsMode::OPPORTUNISTIC) {
        // In opportunistic mode, handle falling back to cleartext in some
        // cases (DNS shouldn't fail if a validated opportunistic mode server
        // becomes unreachable for some reason).
        switch (response) {
            case DnsTlsTransport::Response::success:
                *rcode = reinterpret_cast<HEADER*>(answer.base())->rcode;
                return resplen;
            case DnsTlsTransport::Response::network_error:
                // No need to set the error timeout here since it will fallback to UDP.
            case DnsTlsTransport::Response::internal_error:
                // Note: this will cause cleartext queries to be emitted, with
                // all of the EDNS0 goodness enabled. Fingers crossed.  :-/
                *fallback = true;
                [[fallthrough]];
            default:
                return -1;
        }
    } else {
        // Strict mode
        switch (response) {
            case DnsTlsTransport::Response::success:
                *rcode = reinterpret_cast<HEADER*>(answer.base())->rcode;
                return resplen;
            case DnsTlsTransport::Response::network_error:
                // This case happens when the query stored in DnsTlsTransport is expired since
                // either 1) the query has been tried for 3 times but no response or 2) fail to
                // establish the connection with the server.
                *rcode = RCODE_TIMEOUT;
                [[fallthrough]];
            default:
                return -1;
        }
    }
}

int resolv_res_nsend(const android_net_context* netContext, const uint8_t* msg, int msgLen,
                     uint8_t* ans, int ansLen, int* rcode, uint32_t flags,
                     NetworkDnsEventReported* event) {
    assert(event != nullptr);
    ResState res;
    res_init(&res, netContext, event);
    resolv_populate_res_for_net(&res);
    *rcode = NOERROR;
    return res_nsend(&res, msg, msgLen, ans, ansLen, rcode, flags);
}
