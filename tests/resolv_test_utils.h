/*
 * Copyright (C) 2019 The Android Open Source Project
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
 *
 */

#pragma once

#include <arpa/nameser.h>
#include <netdb.h>

#include <string>
#include <vector>

#include <netdutils/InternetAddresses.h>

#include "dns_responder/dns_responder.h"
#include "params.h"

// TODO: make this dynamic and stop depending on implementation details.
constexpr int TEST_NETID = 30;

// Specifying 0 in ai_socktype or ai_protocol of struct addrinfo indicates that any type or
// protocol can be returned by getaddrinfo().
constexpr unsigned int ANY = 0;

static constexpr char kLocalHost[] = "localhost";
static constexpr char kLocalHostAddr[] = "127.0.0.1";
static constexpr char kIp6LocalHost[] = "ip6-localhost";
static constexpr char kIp6LocalHostAddr[] = "::1";
static constexpr char kHelloExampleCom[] = "hello.example.com.";
static constexpr char kHelloExampleComAddrV4[] = "1.2.3.4";
static constexpr char kHelloExampleComAddrV6[] = "::1.2.3.4";

static const std::vector<uint8_t> kHelloExampleComQueryV4 = {
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x01, 0x00, /* Flags: rd */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x00, /* Answer RRs: 0 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01              /* Class: IN */
};

static const std::vector<uint8_t> kHelloExampleComResponseV4 = {
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x81, 0x80, /* Flags: qr rd ra */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x01, /* Answer RRs: 1 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        /* Answers */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x04,             /* Data length: 4 */
        0x01, 0x02, 0x03, 0x04  /* Address: 1.2.3.4 */
};

// Illegal hostnames
static constexpr char kBadCharAfterPeriodHost[] = "hello.example.^com.";
static constexpr char kBadCharBeforePeriodHost[] = "hello.example^.com.";
static constexpr char kBadCharAtTheEndHost[] = "hello.example.com^.";
static constexpr char kBadCharInTheMiddleOfLabelHost[] = "hello.ex^ample.com.";

static const test::DNSHeader kDefaultDnsHeader = {
        // Don't need to initialize the flag "id" and "rd" because DNS responder assigns them from
        // query to response. See RFC 1035 section 4.1.1.
        .id = 0,                // unused. should be assigned from query to response
        .ra = false,            // recursive query support is not available
        .rcode = ns_r_noerror,  // no error
        .qr = true,             // message is a response
        .opcode = QUERY,        // a standard query
        .aa = false,            // answer/authority portion was not authenticated by the server
        .tr = false,            // message is not truncated
        .rd = false,            // unused. should be assigned from query to response
        .ad = false,            // non-authenticated data is unacceptable
};

// TODO: Integrate GetNumQueries relevent functions
size_t GetNumQueries(const test::DNSResponder& dns, const char* name);
size_t GetNumQueriesForProtocol(const test::DNSResponder& dns, const int protocol,
                                const char* name);
size_t GetNumQueriesForType(const test::DNSResponder& dns, ns_type type, const char* name);
std::string ToString(const hostent* he);
std::string ToString(const addrinfo* ai);
std::string ToString(const android::netdutils::ScopedAddrinfo& ai);
std::string ToString(const sockaddr_storage* addr);
std::vector<std::string> ToStrings(const hostent* he);
std::vector<std::string> ToStrings(const addrinfo* ai);
std::vector<std::string> ToStrings(const android::netdutils::ScopedAddrinfo& ai);
