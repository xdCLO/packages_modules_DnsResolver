/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "dns_responder_client"

#include "dns_responder_client.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "NetdClient.h"
#include "binder/IServiceManager.h"

// TODO: make this dynamic and stop depending on implementation details.
#define TEST_OEM_NETWORK "oem29"
#define TEST_NETID 30

// TODO: move this somewhere shared.
static const char* ANDROID_DNS_MODE = "ANDROID_DNS_MODE";

using android::base::StringPrintf;
using android::net::INetd;
using android::net::ResolverParamsParcel;

static const char kCaCert[] = R"(
-----BEGIN CERTIFICATE-----
MIIC4TCCAcmgAwIBAgIUQUHZnWhL6M4qcS+I0lLkMyqf3VMwDQYJKoZIhvcNAQEL
BQAwADAeFw0xOTA2MTAwODM3MzlaFw0yOTA2MDcwODM3MzlaMAAwggEiMA0GCSqG
SIb3DQEBAQUAA4IBDwAwggEKAoIBAQCapRbBg6dRT4id4DxmlyktomE8gpm4W+VA
ZOivhKat4CvGfVjVIAUYxV7LOGREkkT8Qhn5/gU0lShsnURzEDWY+IjMDDw+kRAm
iFAlMRnCobTp/tenseNRB2tDuUhkRbzaT6qaidPbKy099p909gxf4YqsgY2NfsY2
JkideqIkVq2hvLehsu3BgiK06TGUgxOHfj74vx7iGyujq1v38J1hlox5vj/svJF6
jVdDw8p2UkJbO2N9u3al0nNSMG+MCgd3cvKUySTnjedYXsYB0WyH/JZn//KDq6o+
as6eQVHuH1fu+8XPzBNATlkHzy+YAs7T+UWbkoa1F8wIElVQg66lAgMBAAGjUzBR
MB0GA1UdDgQWBBShu/e54D3VdqdLOWo9Ou5hbjaIojAfBgNVHSMEGDAWgBShu/e5
4D3VdqdLOWo9Ou5hbjaIojAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUA
A4IBAQBFkEGqqzzdQlhP5m1kzh+SiUCwekzSump0CSk0JAXAdeLNbWs3H+pE1/hM
Fx7oFonoX5O6pi68JfcIP0u7wNuZkKqubUl4gG6aHDkAad2oeTov0Be7EKt8Ekwf
tmFWVQQrF9otlG3Stn4vmE5zVNGQXDgRsNKPekSo0XJz757q5WgblauB71Rekvio
TCUXXt3jf3SuovcUFjgBkaohikBRbLiPWZrW4y0XUsgBKI6sLtiSZOPiNevY2xAR
y7mCSmi4wP7vtUQ5G8znkAMKoo0FzyfjSogGQeREUM8Oe9Mmh/D39sq/D4TsiAxt
Pwl59DlzlHHJhmOL+SCGciBX4X7p
-----END CERTIFICATE-----
)";

void DnsResponderClient::SetupMappings(unsigned num_hosts, const std::vector<std::string>& domains,
                                       std::vector<Mapping>* mappings) {
    mappings->resize(num_hosts * domains.size());
    auto mappings_it = mappings->begin();
    for (unsigned i = 0; i < num_hosts; ++i) {
        for (const auto& domain : domains) {
            mappings_it->host = StringPrintf("host%u", i);
            mappings_it->entry = StringPrintf("%s.%s.", mappings_it->host.c_str(), domain.c_str());
            mappings_it->ip4 = StringPrintf("192.0.2.%u", i % 253 + 1);
            mappings_it->ip6 = StringPrintf("2001:db8::%x", i % 65534 + 1);
            ++mappings_it;
        }
    }
}

// TODO: Use SetResolverConfiguration() with ResolverParamsParcel struct directly.
// DEPRECATED: Use SetResolverConfiguration() in new code
static ResolverParamsParcel makeResolverParamsParcel(int netId, const std::vector<int>& params,
                                                     const std::vector<std::string>& servers,
                                                     const std::vector<std::string>& domains,
                                                     const std::string& tlsHostname,
                                                     const std::vector<std::string>& tlsServers,
                                                     const std::string& caCert) {
    using android::net::IDnsResolver;
    ResolverParamsParcel paramsParcel;

    paramsParcel.netId = netId;
    paramsParcel.sampleValiditySeconds = params[IDnsResolver::RESOLVER_PARAMS_SAMPLE_VALIDITY];
    paramsParcel.successThreshold = params[IDnsResolver::RESOLVER_PARAMS_SUCCESS_THRESHOLD];
    paramsParcel.minSamples = params[IDnsResolver::RESOLVER_PARAMS_MIN_SAMPLES];
    paramsParcel.maxSamples = params[IDnsResolver::RESOLVER_PARAMS_MAX_SAMPLES];
    if (params.size() > IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC) {
        paramsParcel.baseTimeoutMsec = params[IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC];
    } else {
        paramsParcel.baseTimeoutMsec = 0;
    }
    if (params.size() > IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT) {
        paramsParcel.retryCount = params[IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT];
    } else {
        paramsParcel.retryCount = 0;
    }
    paramsParcel.servers = servers;
    paramsParcel.domains = domains;
    paramsParcel.tlsName = tlsHostname;
    paramsParcel.tlsServers = tlsServers;
    paramsParcel.tlsFingerprints = {};
    paramsParcel.caCertificate = caCert;
    paramsParcel.tlsConnectTimeoutMs = 1000;

    return paramsParcel;
}

bool DnsResponderClient::SetResolversForNetwork(const std::vector<std::string>& servers,
                                                const std::vector<std::string>& domains,
                                                const std::vector<int>& params) {
    const auto& resolverParams =
            makeResolverParamsParcel(TEST_NETID, params, servers, domains, "", {}, "");
    const auto rv = mDnsResolvSrv->setResolverConfiguration(resolverParams);
    return rv.isOk();
}

bool DnsResponderClient::SetResolversWithTls(const std::vector<std::string>& servers,
                                             const std::vector<std::string>& domains,
                                             const std::vector<int>& params,
                                             const std::vector<std::string>& tlsServers,
                                             const std::string& name) {
    const auto& resolverParams = makeResolverParamsParcel(TEST_NETID, params, servers, domains,
                                                          name, tlsServers, kCaCert);
    const auto rv = mDnsResolvSrv->setResolverConfiguration(resolverParams);
    if (!rv.isOk()) LOG(ERROR) << "SetResolversWithTls() -> " << rv.toString8();
    return rv.isOk();
}

void DnsResponderClient::SetupDNSServers(unsigned num_servers, const std::vector<Mapping>& mappings,
                                         std::vector<std::unique_ptr<test::DNSResponder>>* dns,
                                         std::vector<std::string>* servers) {
    const char* listen_srv = "53";
    dns->resize(num_servers);
    servers->resize(num_servers);
    for (unsigned i = 0; i < num_servers; ++i) {
        auto& server = (*servers)[i];
        auto& d = (*dns)[i];
        server = StringPrintf("127.0.0.%u", i + 100);
        d = std::make_unique<test::DNSResponder>(server, listen_srv, ns_rcode::ns_r_servfail);
        for (const auto& mapping : mappings) {
            d->addMapping(mapping.entry.c_str(), ns_type::ns_t_a, mapping.ip4.c_str());
            d->addMapping(mapping.entry.c_str(), ns_type::ns_t_aaaa, mapping.ip6.c_str());
        }
        d->startServer();
    }
}

int DnsResponderClient::SetupOemNetwork() {
    mNetdSrv->networkDestroy(TEST_NETID);
    mDnsResolvSrv->destroyNetworkCache(TEST_NETID);
    auto ret = mNetdSrv->networkCreatePhysical(TEST_NETID, INetd::PERMISSION_NONE);
    if (!ret.isOk()) {
        fprintf(stderr, "Creating physical network %d failed, %s\n", TEST_NETID,
                ret.toString8().string());
        return -1;
    }
    ret = mDnsResolvSrv->createNetworkCache(TEST_NETID);
    if (!ret.isOk()) {
        fprintf(stderr, "Creating network cache %d failed, %s\n", TEST_NETID,
                ret.toString8().string());
        return -1;
    }
    setNetworkForProcess(TEST_NETID);
    if ((unsigned)TEST_NETID != getNetworkForProcess()) {
        return -1;
    }
    return TEST_NETID;
}

void DnsResponderClient::TearDownOemNetwork(int oemNetId) {
    if (oemNetId != -1) {
        mNetdSrv->networkDestroy(oemNetId);
        mDnsResolvSrv->destroyNetworkCache(oemNetId);
    }
}

void DnsResponderClient::SetUp() {
    // binder setup
    auto binder = android::defaultServiceManager()->getService(android::String16("netd"));
    mNetdSrv = android::interface_cast<android::net::INetd>(binder);
    if (mNetdSrv == nullptr) {
        LOG(FATAL) << "Can't connect to service 'netd'. Missing root privileges? uid=" << getuid();
    }

    auto resolvBinder =
            android::defaultServiceManager()->getService(android::String16("dnsresolver"));
    mDnsResolvSrv = android::interface_cast<android::net::IDnsResolver>(resolvBinder);

    // Ensure resolutions go via proxy.
    setenv(ANDROID_DNS_MODE, "", 1);
    mOemNetId = SetupOemNetwork();
}

void DnsResponderClient::TearDown() {
    TearDownOemNetwork(mOemNetId);
}
