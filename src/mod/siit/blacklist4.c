#include "mod/siit/blacklist4.h"

#include <linux/rculist.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>

#include "mod/common/address.h"
#include "mod/common/xlator.h"
#include "mod/common/rcu.h"

/* TODO (fine) fuse this module and pool.c */

struct addr4_pool *blacklist4_alloc(void)
{
	return pool_alloc();
}

void blacklist4_get(struct addr4_pool *pool)
{
	pool_get(pool);
}

void blacklist4_put(struct addr4_pool *pool)
{
	pool_put(pool);
}

int blacklist4_add(struct addr4_pool *pool, struct ipv4_prefix *prefix)
{
	return pool_add(pool, prefix, false);
}

int blacklist4_rm(struct addr4_pool *pool, struct ipv4_prefix *prefix)
{
	return pool_rm(pool, prefix);
}

int blacklist4_flush(struct addr4_pool *pool)
{
	return pool_flush(pool);
}

/**
 * Is @addr *NOT* translatable, according to the interfaces?
 *
 * The name comes from the fact that interface addresses are usually
 * non-translatable (ie. the traffic is meant for the translator box).
 *
 * Recognizable directed broadcast is also not translatable.
 */
bool interface_contains(struct net *ns, struct in_addr *addr)
{
	struct net_device *dev;
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	struct in_addr ifaddr;

	rcu_read_lock();
	for_each_netdev_rcu(ns, dev) {
		in_dev = rcu_dereference(dev->ip_ptr);
		ifa = in_dev->ifa_list;
		while (ifa) {
			ifaddr.s_addr = ifa->ifa_local;
			if (ipv4_addr_cmp(&ifaddr, addr) == 0) {
				/* https://github.com/NICMx/Jool/issues/223 */
				if (ifa->ifa_prefixlen == 32)
					goto do_translate;
				else
					goto dont_translate;
			}

			/* RFC3021: /31 (and /32) networks lack broadcast. */
			if (ifa->ifa_prefixlen < 31) {
				ifaddr.s_addr = ifa->ifa_local | ~ifa->ifa_mask;
				if (ipv4_addr_cmp(&ifaddr, addr) == 0)
					goto dont_translate;
			}

			ifa = ifa->ifa_next;
		}
	}
	/* Fall through */

do_translate:
	rcu_read_unlock();
	return false;

dont_translate:
	rcu_read_unlock();
	return true;
}

bool blacklist4_contains(struct addr4_pool *pool, struct in_addr *addr)
{
	return pool_contains(pool, addr);
}

int blacklist4_foreach(struct addr4_pool *pool,
		int (*func)(struct ipv4_prefix *, void *), void *arg,
		struct ipv4_prefix *offset)
{
	return pool_foreach(pool, func, arg, offset);
}

bool blacklist4_is_empty(struct addr4_pool *pool)
{
	return pool_is_empty(pool);
}
