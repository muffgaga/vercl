#pragma once

namespace Spike {

struct __attribute__ ((__packed__)) dummy {
	enum PACKET_TYPE {
		DUMMY,
		SYNC,
		SPIKES = 0xff
	};

	// for now: full-width timestamps
	uint64_t timestamp0;
	uint64_t timestamp;
	uint16_t label;
	uint16_t packet_type;
};


// packet format used for UHEI-SpiNNAker udp-based spike comm
// cf. "A location-independent direct link neuromorphic interface"
struct __attribute__ ((__packed__)) SpiNNaker {
	uint32_t label; // 46?

	void ntoh() {
		label = ntohl(label);
	}
	void hton() {
		label = htonl(label);
	}
};

}
