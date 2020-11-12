/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <netinet/in.h>
#include <sys/capability.h>

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-get-properties.h"
#include "bus-message-util.h"
#include "bus-polkit.h"
#include "parse-util.h"
#include "resolve-util.h"
#include "resolved-bus.h"
#include "resolved-link-bus.h"
#include "resolved-resolv-conf.h"
#include "socket-netlink.h"
#include "stdio-util.h"
#include "strv.h"
#include "user-util.h"

static BUS_DEFINE_PROPERTY_GET(property_get_dnssec_supported, "b", Link, link_dnssec_supported);
static BUS_DEFINE_PROPERTY_GET2(property_get_dnssec_mode, "s", Link, link_get_dnssec_mode, dnssec_mode_to_string);

static int property_get_dns_over_tls_mode(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Link *l = userdata;

        assert(reply);
        assert(l);

        return sd_bus_message_append(reply, "s", dns_over_tls_mode_to_string(link_get_dns_over_tls_mode(l)));
}

static int property_get_dns_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error,
                bool extended) {

        Link *l = userdata;
        DnsServer *s;
        int r;

        assert(reply);
        assert(l);

        r = sd_bus_message_open_container(reply, 'a', extended ? "(iayqs)" : "(iay)");
        if (r < 0)
                return r;

        LIST_FOREACH(servers, s, l->dns_servers) {
                r = bus_dns_server_append(reply, s, false, extended);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_dns(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        return property_get_dns_internal(bus, path, interface, property, reply, userdata, error, false);
}

static int property_get_dns_ex(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        return property_get_dns_internal(bus, path, interface, property, reply, userdata, error, true);
}

static int property_get_current_dns_server_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error,
                bool extended) {

        DnsServer *s;

        assert(reply);
        assert(userdata);

        s = *(DnsServer **) userdata;

        return bus_dns_server_append(reply, s, false, extended);
}

static int property_get_current_dns_server(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        return property_get_current_dns_server_internal(bus, path, interface, property, reply, userdata, error, false);
}

static int property_get_current_dns_server_ex(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        return property_get_current_dns_server_internal(bus, path, interface, property, reply, userdata, error, true);
}

static int property_get_domains(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Link *l = userdata;
        DnsSearchDomain *d;
        int r;

        assert(reply);
        assert(l);

        r = sd_bus_message_open_container(reply, 'a', "(sb)");
        if (r < 0)
                return r;

        LIST_FOREACH(domains, d, l->search_domains) {
                r = sd_bus_message_append(reply, "(sb)", d->name, d->route_only);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_default_route(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Link *l = userdata;

        assert(reply);
        assert(l);

        /* Return what is configured, if there's something configured */
        if (l->default_route >= 0)
                return sd_bus_message_append(reply, "b", l->default_route);

        /* Otherwise report what is in effect */
        if (l->unicast_scope)
                return sd_bus_message_append(reply, "b", dns_scope_is_default_route(l->unicast_scope));

        return sd_bus_message_append(reply, "b", false);
}

static int property_get_scopes_mask(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Link *l = userdata;
        uint64_t mask;

        assert(reply);
        assert(l);

        mask =  (l->unicast_scope ? SD_RESOLVED_DNS : 0) |
                (l->llmnr_ipv4_scope ? SD_RESOLVED_LLMNR_IPV4 : 0) |
                (l->llmnr_ipv6_scope ? SD_RESOLVED_LLMNR_IPV6 : 0) |
                (l->mdns_ipv4_scope ? SD_RESOLVED_MDNS_IPV4 : 0) |
                (l->mdns_ipv6_scope ? SD_RESOLVED_MDNS_IPV6 : 0);

        return sd_bus_message_append(reply, "t", mask);
}

static int property_get_ntas(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Link *l = userdata;
        const char *name;
        int r;

        assert(reply);
        assert(l);

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        SET_FOREACH(name, l->dnssec_negative_trust_anchors) {
                r = sd_bus_message_append(reply, "s", name);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int verify_unmanaged_link(Link *l, sd_bus_error *error) {
        assert(l);

        if (l->flags & IFF_LOOPBACK)
                return sd_bus_error_setf(error, BUS_ERROR_LINK_BUSY, "Link %s is loopback device.", l->ifname);
        if (l->is_managed)
                return sd_bus_error_setf(error, BUS_ERROR_LINK_BUSY, "Link %s is managed.", l->ifname);

        return 0;
}

static int bus_link_method_set_dns_servers_internal(sd_bus_message *message, void *userdata, sd_bus_error *error, bool extended) {
        struct in_addr_full **dns;
        Link *l = userdata;
        size_t n;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = bus_message_read_dns_servers(message, error, extended, &dns, &n);
        if (r < 0)
                return r;

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-dns-servers",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                goto finalize;
        if (r == 0) {
                r = 1; /* Polkit will call us back */
                goto finalize;
        }

        dns_server_mark_all(l->dns_servers);

        for (size_t i = 0; i < n; i++) {
                DnsServer *s;

                s = dns_server_find(l->dns_servers, dns[i]->family, &dns[i]->address, dns[i]->port, 0, dns[i]->server_name);
                if (s)
                        dns_server_move_back_and_unmark(s);
                else {
                        r = dns_server_new(l->manager, NULL, DNS_SERVER_LINK, l, dns[i]->family, &dns[i]->address, dns[i]->port, 0, dns[i]->server_name);
                        if (r < 0) {
                                dns_server_unlink_all(l->dns_servers);
                                goto finalize;
                        }
                }

        }

        dns_server_unlink_marked(l->dns_servers);
        link_allocate_scopes(l);

        (void) link_save_user(l);
        (void) manager_write_resolv_conf(l->manager);
        (void) manager_send_changed(l->manager, "DNS");

        r = sd_bus_reply_method_return(message, NULL);

finalize:
        for (size_t i = 0; i < n; i++)
                in_addr_full_free(dns[i]);
        free(dns);

        return r;
}

int bus_link_method_set_dns_servers(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_link_method_set_dns_servers_internal(message, userdata, error, false);
}

int bus_link_method_set_dns_servers_ex(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_link_method_set_dns_servers_internal(message, userdata, error, true);
}

int bus_link_method_set_domains(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_enter_container(message, 'a', "(sb)");
        if (r < 0)
                return r;

        for (;;) {
                const char *name;
                int route_only;

                r = sd_bus_message_read(message, "(sb)", &name, &route_only);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = dns_name_is_valid(name);
                if (r < 0)
                        return r;
                if (r == 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid search domain %s", name);
                if (!route_only && dns_name_is_root(name))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Root domain is not suitable as search domain");
        }

        r = sd_bus_message_rewind(message, false);
        if (r < 0)
                return r;

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-domains",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        dns_search_domain_mark_all(l->search_domains);

        for (;;) {
                DnsSearchDomain *d;
                const char *name;
                int route_only;

                r = sd_bus_message_read(message, "(sb)", &name, &route_only);
                if (r < 0)
                        goto clear;
                if (r == 0)
                        break;

                r = dns_search_domain_find(l->search_domains, name, &d);
                if (r < 0)
                        goto clear;

                if (r > 0)
                        dns_search_domain_move_back_and_unmark(d);
                else {
                        r = dns_search_domain_new(l->manager, &d, DNS_SEARCH_DOMAIN_LINK, l, name);
                        if (r < 0)
                                goto clear;
                }

                d->route_only = route_only;
        }

        r = sd_bus_message_exit_container(message);
        if (r < 0)
                goto clear;

        dns_search_domain_unlink_marked(l->search_domains);

        (void) link_save_user(l);
        (void) manager_write_resolv_conf(l->manager);

        return sd_bus_reply_method_return(message, NULL);

clear:
        dns_search_domain_unlink_all(l->search_domains);
        return r;
}

int bus_link_method_set_default_route(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        int r, b;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "b", &b);
        if (r < 0)
                return r;

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-default-route",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        if (l->default_route != b) {
                l->default_route = b;

                (void) link_save_user(l);
                (void) manager_write_resolv_conf(l->manager);
        }

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_set_llmnr(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        ResolveSupport mode;
        const char *llmnr;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &llmnr);
        if (r < 0)
                return r;

        if (isempty(llmnr))
                mode = RESOLVE_SUPPORT_YES;
        else {
                mode = resolve_support_from_string(llmnr);
                if (mode < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid LLMNR setting: %s", llmnr);
        }

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-llmnr",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        l->llmnr_support = mode;
        link_allocate_scopes(l);
        link_add_rrs(l, false);

        (void) link_save_user(l);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_set_mdns(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        ResolveSupport mode;
        const char *mdns;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &mdns);
        if (r < 0)
                return r;

        if (isempty(mdns))
                mode = RESOLVE_SUPPORT_NO;
        else {
                mode = resolve_support_from_string(mdns);
                if (mode < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid MulticastDNS setting: %s", mdns);
        }

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-mdns",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        l->mdns_support = mode;
        link_allocate_scopes(l);
        link_add_rrs(l, false);

        (void) link_save_user(l);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_set_dns_over_tls(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        const char *dns_over_tls;
        DnsOverTlsMode mode;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &dns_over_tls);
        if (r < 0)
                return r;

        if (isempty(dns_over_tls))
                mode = _DNS_OVER_TLS_MODE_INVALID;
        else {
                mode = dns_over_tls_mode_from_string(dns_over_tls);
                if (mode < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid DNSOverTLS setting: %s", dns_over_tls);
        }

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-dns-over-tls",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        link_set_dns_over_tls_mode(l, mode);

        (void) link_save_user(l);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_set_dnssec(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        const char *dnssec;
        DnssecMode mode;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &dnssec);
        if (r < 0)
                return r;

        if (isempty(dnssec))
                mode = _DNSSEC_MODE_INVALID;
        else {
                mode = dnssec_mode_from_string(dnssec);
                if (mode < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid DNSSEC setting: %s", dnssec);
        }

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-dnssec",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        link_set_dnssec_mode(l, mode);

        (void) link_save_user(l);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_set_dnssec_negative_trust_anchors(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_set_free_free_ Set *ns = NULL;
        _cleanup_strv_free_ char **ntas = NULL;
        Link *l = userdata;
        int r;
        char **i;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        ns = set_new(&dns_name_hash_ops);
        if (!ns)
                return -ENOMEM;

        r = sd_bus_message_read_strv(message, &ntas);
        if (r < 0)
                return r;

        STRV_FOREACH(i, ntas) {
                r = dns_name_is_valid(*i);
                if (r < 0)
                        return r;
                if (r == 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                                                 "Invalid negative trust anchor domain: %s", *i);

                r = set_put_strdup(&ns, *i);
                if (r < 0)
                        return r;
        }

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.set-dnssec-negative-trust-anchors",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        set_free_free(l->dnssec_negative_trust_anchors);
        l->dnssec_negative_trust_anchors = TAKE_PTR(ns);

        (void) link_save_user(l);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_link_method_revert(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Link *l = userdata;
        int r;

        assert(message);
        assert(l);

        r = verify_unmanaged_link(l, error);
        if (r < 0)
                return r;

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.resolve1.revert",
                                    NULL, true, UID_INVALID,
                                    &l->manager->polkit_registry, error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Polkit will call us back */

        link_flush_settings(l);
        link_allocate_scopes(l);
        link_add_rrs(l, false);

        (void) link_save_user(l);
        (void) manager_write_resolv_conf(l->manager);
        (void) manager_send_changed(l->manager, "DNS");

        return sd_bus_reply_method_return(message, NULL);
}

static int link_object_find(sd_bus *bus, const char *path, const char *interface, void *userdata, void **found, sd_bus_error *error) {
        _cleanup_free_ char *e = NULL;
        Manager *m = userdata;
        Link *link;
        int ifindex, r;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        r = sd_bus_path_decode(path, "/org/freedesktop/resolve1/link", &e);
        if (r <= 0)
                return 0;

        ifindex = parse_ifindex(e);
        if (ifindex < 0)
                return 0;

        link = hashmap_get(m->links, INT_TO_PTR(ifindex));
        if (!link)
                return 0;

        *found = link;
        return 1;
}

char *link_bus_path(const Link *link) {
        char *p, ifindex[DECIMAL_STR_MAX(link->ifindex)];
        int r;

        assert(link);

        xsprintf(ifindex, "%i", link->ifindex);

        r = sd_bus_path_encode("/org/freedesktop/resolve1/link", ifindex, &p);
        if (r < 0)
                return NULL;

        return p;
}

static int link_node_enumerator(sd_bus *bus, const char *path, void *userdata, char ***nodes, sd_bus_error *error) {
        _cleanup_strv_free_ char **l = NULL;
        Manager *m = userdata;
        Link *link;
        unsigned c = 0;

        assert(bus);
        assert(path);
        assert(m);
        assert(nodes);

        l = new0(char*, hashmap_size(m->links) + 1);
        if (!l)
                return -ENOMEM;

        HASHMAP_FOREACH(link, m->links) {
                char *p;

                p = link_bus_path(link);
                if (!p)
                        return -ENOMEM;

                l[c++] = p;
        }

        l[c] = NULL;
        *nodes = TAKE_PTR(l);

        return 1;
}

static const sd_bus_vtable link_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("ScopesMask", "t", property_get_scopes_mask, 0, 0),
        SD_BUS_PROPERTY("DNS", "a(iay)", property_get_dns, 0, 0),
        SD_BUS_PROPERTY("DNSEx", "a(iayqs)", property_get_dns_ex, 0, 0),
        SD_BUS_PROPERTY("CurrentDNSServer", "(iay)", property_get_current_dns_server, offsetof(Link, current_dns_server), 0),
        SD_BUS_PROPERTY("CurrentDNSServerEx", "(iayqs)", property_get_current_dns_server_ex, offsetof(Link, current_dns_server), 0),
        SD_BUS_PROPERTY("Domains", "a(sb)", property_get_domains, 0, 0),
        SD_BUS_PROPERTY("DefaultRoute", "b", property_get_default_route, 0, 0),
        SD_BUS_PROPERTY("LLMNR", "s", bus_property_get_resolve_support, offsetof(Link, llmnr_support), 0),
        SD_BUS_PROPERTY("MulticastDNS", "s", bus_property_get_resolve_support, offsetof(Link, mdns_support), 0),
        SD_BUS_PROPERTY("DNSOverTLS", "s", property_get_dns_over_tls_mode, 0, 0),
        SD_BUS_PROPERTY("DNSSEC", "s", property_get_dnssec_mode, 0, 0),
        SD_BUS_PROPERTY("DNSSECNegativeTrustAnchors", "as", property_get_ntas, 0, 0),
        SD_BUS_PROPERTY("DNSSECSupported", "b", property_get_dnssec_supported, 0, 0),

        SD_BUS_METHOD_WITH_ARGS("SetDNS",
                                SD_BUS_ARGS("a(iay)", addresses),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_dns_servers,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDNSEx",
                                SD_BUS_ARGS("a(iayqs)", addresses),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_dns_servers_ex,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDomains",
                                SD_BUS_ARGS("a(sb)", domains),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_domains,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDefaultRoute",
                                SD_BUS_ARGS("b", enable),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_default_route,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetLLMNR",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_llmnr,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetMulticastDNS",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_mdns,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDNSOverTLS",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_dns_over_tls,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDNSSEC",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_dnssec,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("SetDNSSECNegativeTrustAnchors",
                                SD_BUS_ARGS("as", names),
                                SD_BUS_NO_RESULT,
                                bus_link_method_set_dnssec_negative_trust_anchors,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("Revert",
                                SD_BUS_NO_ARGS,
                                SD_BUS_NO_RESULT,
                                bus_link_method_revert,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_VTABLE_END
};

const BusObjectImplementation link_object = {
        "/org/freedesktop/resolve1/link",
        "org.freedesktop.resolve1.Link",
        .fallback_vtables = BUS_FALLBACK_VTABLES({link_vtable, link_object_find}),
        .node_enumerator = link_node_enumerator,
};
