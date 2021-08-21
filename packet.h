/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __PACKET_H__
#define __PACKET_H__

#include "config.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include <algorithm>
#include <string>
#include <vector>
#include <map>

#include "eventbus.h"
#include "globalregistry.h"
#include "macaddr.h"
#include "packet_ieee80211.h"
#include "trackedelement.h"
#include "trackedcomponent.h"

#include "string_view.hpp"

// This is the main switch for how big the vector is.  If something ever starts
// bumping up against this we'll need to increase it, but that'll slow down 
// generating a packet (slightly) so I'm leaving it relatively low.
#define MAX_PACKET_COMPONENTS	64

// Maximum length of a frame
#define MAX_PACKET_LEN			8192

// Same as defined in libpcap/system, but we need to know the basic dot11 DLT
// even when we don't have pcap
#define KDLT_IEEE802_11			105

class packet_component {
public:
    packet_component() { };
    virtual ~packet_component() { }
};

// Overall packet container that holds packet information
class kis_packet {
public:
    // Time of packet creation
    struct timeval ts;

    // Do we know this is in error from the capture source itself?
    int error;

    // Do we know this packet is OK and don't have to run a CRC check on 
    // it inside a data layer?
    int crc_ok;

    // Have we been filtered for some reason?
    int filtered;

    // Are we a duplicate?
    int duplicate;

    // Raw packet data; other packet components refer to this via stringviews
    // whenever possible to minimize the copy duplication.  It is pre-reserved as a
    // max packet size block
    std::string raw_data;
    nonstd::string_view data;

    // Did this packet trigger creation of a new device?  Since a 
    // single packet can create multiple devices in some phys, maintain
    // a vector of device events to publish when all devices are done
    // processing
    std::vector<std::shared_ptr<eventbus_event>> process_complete_events;

    // pre-allocated vector of broken down packet components
    std::shared_ptr<packet_component> content_vec[MAX_PACKET_COMPONENTS];

    kis_packet();
    ~kis_packet();

    kis_packet(kis_packet&& p) {
        error = p.error;
        crc_ok = p.crc_ok;
        filtered = p.filtered;
        process_complete_events = std::move(p.process_complete_events);
        raw_data = std::move(p.raw_data);
        data = std::move(p.data);

        for (int c = 0; c < MAX_PACKET_COMPONENTS; c++)
            content_vec[c] = p.content_vec[c];
    }

    void reset() {
        error = 0;
        crc_ok = 0;
        filtered = 0;
        duplicate = 0;

        // Reset and re-reserve in case we were resized somehow
        raw_data = "";
        raw_data.reserve(MAX_PACKET_LEN);

        process_complete_events.clear();

        for (size_t x = 0; x < MAX_PACKET_COMPONENTS; x++)
            content_vec[x].reset();
    }

    void set_data(const std::string& sdata) {
        raw_data = sdata;
    }

    template<typename T>
    void set_data(const T* tdata, size_t len) {
        raw_data = std::string(tdata, len);
    }

    // Preferred smart pointers
    void insert(const unsigned int index, std::shared_ptr<packet_component> data);

    std::shared_ptr<packet_component> fetch(const unsigned int index) const;

    template<class T> 
    std::shared_ptr<T> fetch() {
        return nullptr;
    }

    template<class T, typename... Pn> 
    std::shared_ptr<T> fetch(const unsigned int index, const Pn& ... args) {
        auto k = std::static_pointer_cast<T>(this->fetch(index));

        if (k != nullptr)
            return k;

        return this->fetch<T>(args...);
    }

    template<class T, typename... Pn>
    std::shared_ptr<T> fetch_or_add(const unsigned int index, const Pn& ... args) {
        auto k = std::static_pointer_cast<T>(this->fetch(index));

        if (k != nullptr)
            return k;

        k = std::make_shared<T>(args...);
        this->insert(index, k);
        return k;
    }

    void erase(const unsigned int index);

    // Tags applied to the packet
    std::vector<std::string> tag_vec;
};


// A generic tracked packet, which allows us to save some frames in a way we
// can recall and expose via the REST interface, for instance
class kis_tracked_packet : public tracker_component {
public:
    kis_tracked_packet() :
        tracker_component() {
            register_fields();
            reserve_fields(NULL);
        }

    kis_tracked_packet(int in_id) :
        tracker_component(in_id) {
            register_fields();
            reserve_fields(NULL);
        }

    kis_tracked_packet(int in_id, std::shared_ptr<tracker_element_map> e) :
        tracker_component(in_id) {
            register_fields();
            reserve_fields(e);
        }

    kis_tracked_packet(const kis_tracked_packet *p) :
        tracker_component{p} {
            __ImportField(ts_sec, p);
            __ImportField(ts_usec, p);
            __ImportField(dlt, p);
            __ImportField(source, p);
            __ImportField(data, p);
            reserve_fields(nullptr);
        }

    virtual uint32_t get_signature() const override {
        return adler32_checksum("kis_tracked_packet");
    }

    virtual std::unique_ptr<tracker_element> clone_type() override {
        using this_t = std::remove_pointer<decltype(this)>::type;
        auto dup = std::unique_ptr<this_t>(new this_t(this));
        return std::move(dup);
    }

    __Proxy(ts_sec, uint64_t, time_t, time_t, ts_sec);
    __Proxy(ts_usec, uint64_t, uint64_t, uint64_t, ts_usec);
    __Proxy(dlt, uint64_t, uint64_t, uint64_t, dlt);
    __Proxy(source, uint64_t, uint64_t, uint64_t, source);

    __ProxyTrackable(data, tracker_element_byte_array, data);

    virtual void copy_packet(std::shared_ptr<kis_tracked_packet> in) {
        set_ts_sec(in->get_ts_sec());
        set_ts_usec(in->get_ts_usec());
        set_dlt(in->get_dlt());
        set_source(in->get_source());
        set_data(in->get_data());
    }

protected:
    virtual void register_fields() override {
        tracker_component::register_fields();

        register_field("kismet.packet.ts_sec", "packet timestamp (second)", &ts_sec);
        register_field("kismet.packet.ts_usec", "packet timestamp (usec)", &ts_usec);
        register_field("kismet.packet.dlt", "packet DLT linktype", &dlt);
        register_field("kismet.packet.source", "packetsource id", &source);
        register_field("kismet.packet.data", "packet data", &data);
    }

    std::shared_ptr<tracker_element_uint64> ts_sec;
    std::shared_ptr<tracker_element_uint64> ts_usec;
    std::shared_ptr<tracker_element_uint64> dlt;
    std::shared_ptr<tracker_element_uint64> source;
    std::shared_ptr<tracker_element_byte_array> data;
};

// Arbitrary data chunk, decapsulated from the link headers
class kis_datachunk : public packet_component {
public:
    // Data window
    nonstd::string_view data;

    // Underlying raw data, typically unused
    std::string raw_data;

    int dlt;
    uint16_t source_id;

    kis_datachunk() {
        source_id = 0;
    }

    virtual ~kis_datachunk() { }

    virtual void set_data(const nonstd::string_view& view) {
        data = view;
    }
    virtual void set_data(std::string& data) {
        data = nonstd::string_view(data);
    }

    virtual void set_raw_data(const std::string& sdata) {
        raw_data = sdata;
        data = std::string_view(raw_data);
    }

    template<typename T>
    void set_raw_data(const T* rd, size_t sz) {
        raw_data = std::string(rd, sz);
        data = std::string_view(raw_data);
    }
};

// Arbitrary data blob which gets logged into the DATA table in the kismet log
class packet_metablob : public packet_component {
public:
    std::string meta_type;
    std::string meta_data;

    packet_metablob(std::string in_type, std::string in_data) {
        meta_type = in_type;
        meta_data = in_data;
    }
};

class kis_packet_checksum : public kis_datachunk {
public:
    int checksum_valid;

    kis_packet_checksum() : kis_datachunk() {
        checksum_valid = 0;
    }
};

enum kis_packet_basictype {
    packet_basic_unknown = 0,
    packet_basic_mgmt = 1,
    packet_basic_data = 2,
    packet_basic_phy = 3
};

// Common info
// Extracted by phy-specific dissectors, used by the common classifier
// to build phy-neutral devices and tracking records.
class kis_tracked_device_base;

enum kis_packet_direction {
    packet_direction_unknown = 0,

    // From device
    packet_direction_from = 1,

    // To device
    packet_direction_to = 2,

    // Intra-carrier (WDS for instance)
    packet_direction_carrier = 3
};

// Common info item which is aggregated into a packet under 
// the packet_info_map type
class kis_common_info : public packet_component {
public:
    kis_common_info() {
        type = packet_basic_unknown;
        direction = packet_direction_unknown;

        phyid = -1;
        error = 0;
        datasize = 0;
        channel = "0";
        freq_khz = 0;
        basic_crypt_set = 0;

        source = mac_addr(0);
        dest = mac_addr(0);
        network = mac_addr(0);
        transmitter = mac_addr(0);
    }

    // Source - origin of packet
    // Destination - dest of packet
    // Network - Associated network device (such as ap bssid)
    // Transmitter - Independent transmitter, if not source or network
    // (wifi wds for instance)
    mac_addr source, dest, network, transmitter;

    kis_packet_basictype type;
    kis_packet_direction direction;

    int phyid;
    // Some sort of phy-level error 
    int error;
    // Data size if applicable
    int datasize;
    // Encryption if applicable
    uint32_t basic_crypt_set;
    // Phy-specific numeric channel, freq is held in l1info.  Channel is
    // represented as a string to carry whatever special attributes, ie
    // 6HT20 or 6HT40+ for wifi
    std::string channel;
    // Frequency in khz
    double freq_khz;
};

// String reference
class kis_string_info : public packet_component {
public:
    kis_string_info() { }
    std::vector<std::string> extracted_strings;
};

typedef struct {
    std::string text;
    mac_addr bssid;
    mac_addr source;
    mac_addr dest;
} string_proto_info;

// some protocols we do try to track
enum kis_protocol_info_type {
    proto_unknown,
    proto_udp, 
    proto_tcp, 
    proto_arp, 
    proto_dhcp_offer,
    proto_dhcp_discover,
    proto_cdp,
    proto_turbocell,
    proto_netstumbler_probe,
    proto_lucent_probe,
    proto_iapp,
    proto_isakmp,
    proto_pptp,
    proto_eap
};

class kis_data_packinfo : public packet_component {
public:
    kis_data_packinfo() {
        proto = proto_unknown;
        ip_source_port = 0;
        ip_dest_port = 0;
        ip_source_addr.s_addr = 0;
        ip_dest_addr.s_addr = 0;
        ip_netmask_addr.s_addr = 0;
        ip_gateway_addr.s_addr = 0;
        field1 = 0;
        ivset[0] = ivset[1] = ivset[2] = 0;
    }

    kis_protocol_info_type proto;

    // IP info, we re-use a subset of the kis_protocol_info_type enum to fill
    // in where we got our IP data from.  A little klugey, but really no reason
    // not to do it
    int ip_source_port;
    int ip_dest_port;
    in_addr ip_source_addr;
    in_addr ip_dest_addr;
    in_addr ip_netmask_addr;
    in_addr ip_gateway_addr;
    kis_protocol_info_type ip_type;

    // The two CDP fields we really care about for anything
    std::string cdp_dev_id;
    std::string cdp_port_id;

    // DHCP Discover data
    std::string discover_host, discover_vendor;

    // IV
    uint8_t ivset[3];

    // An extra field that can be filled in
    int field1;

    // A string field that can be filled in
    std::string auxstring;

};

// Layer 1 radio info record for kismet
enum kis_layer1_packinfo_signal_type {
    kis_l1_signal_type_none,
    kis_l1_signal_type_dbm,
    kis_l1_signal_type_rssi
};

class kis_layer1_packinfo : public packet_component {
public:
    kis_layer1_packinfo() {
        signal_type = kis_l1_signal_type_none;
        signal_dbm = noise_dbm = 0;
        signal_rssi = noise_rssi = 0;
        carrier = carrier_unknown;
        encoding = encoding_unknown;
        datarate = 0;
        freq_khz = 0;
        accuracy = 0;
        channel = "0";
    }

    // How "accurate" are we?  Higher == better.  Nothing uses this yet
    // but we might as well track it here.
    int accuracy;

    // Frequency seen on
    double freq_khz;

    // Logical channel
    std::string channel;

    // Connection info
    kis_layer1_packinfo_signal_type signal_type;
    int signal_dbm, signal_rssi;
    int noise_dbm, noise_rssi;

    // Per-antenna info, mapped to the antenna number
    std::map<uint8_t, int> antenna_signal_map;

    // What carrier brought us this packet?
    phy_carrier_type carrier;

    // What encoding?
    phy_encoding_type encoding;

    // What data rate?
    double datarate;

    // Checksum, if checksumming is enabled; Only of the non-header 
    // data
    uint32_t content_checkum;
};

// JSON as a raw string; parsing happens in the DS code; currently supports one JSON report
// per packet, which is fine for the current design
class kis_json_packinfo : public packet_component {
public:
    kis_json_packinfo() { }

    std::string type;
    std::string json_string;
};

// Protobuf record as a raw string-like record; parsing happens in the DS code; currently
// supports one protobuf report per packet, which is fine for the current design.
class kis_protobuf_packinfo : public packet_component {
public:
    kis_protobuf_packinfo() { }

    std::string type;
    std::string buffer_string;
};

#endif

