#pragma once


#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
}

// spike formats
#include "spike.h"


// TODO: make confiurable
#define ETH_NAME "eth1"

// number of pages to buffer
#define RING_COUNT 100000

// force gcc to inline
#define RC_INLINE __attribute__((always_inline))


namespace HMF {
class RealtimeComm {
public:
	// TODO: add support for multiple targets
	RealtimeComm(std::string const, std::string const, uint16_t const sport, uint16_t const dport, uint8_t remote_mac[ETH_ALEN], bool master);
	RealtimeComm(std::string const, uint16_t const sport, uint16_t const dport);
	~RealtimeComm();

	// common config (TODO: maybe C++11 delegated ctors)
	void initial_config(std::string const local_ip, std::string const remote_ip, uint8_t remote_mac[ETH_ALEN]);

	// non-blocking receive
	template<typename SpikeType>
	inline std::vector<SpikeType*> & receive() RC_INLINE; // doesn't help... can't hide cache misses?

	// blocking receive -- used by time sync
	template<typename SpikeType>
	inline SpikeType const *receive_and_spin() RC_INLINE;

	// clear receiving buffer
	inline void free_receive() RC_INLINE;

	// enqueue spike
	template<typename SpikeType>
	inline void queue_spike(SpikeType&& sp) RC_INLINE;

	// enqueue spikes
	template<typename SpikeType>
	inline void queue_spike(std::vector<SpikeType> inits) RC_INLINE;

	// enqueue + send spike
	template<typename SpikeType>
	inline void send_single_spike(SpikeType&& sp_init) RC_INLINE;

	// trigger send
	inline void send() RC_INLINE;

	// send-trigger thread
	void start_sending_thread();

	// time stuff
	typedef uint64_t timepoint_t;
	inline timepoint_t curtime() const RC_INLINE { return _curtime + _offset; }

	// time sync stuff
	struct SyncStatus {
		// everything in [ns]
		double delay;
		double delay_stdev;
		double offset;
		double offset_stdev;
		friend std::ostream & operator<<(std::ostream&, SyncStatus const&);
	};
	inline SyncStatus sync();

	inline timepoint_t gettime() RC_INLINE;

	timepoint_t _curtime;
	int64_t _offset;
	int64_t _delay;
	bool master;

	std::thread * sender_thread;


private:
	// optimize program behavior directly after startup (trigger page faults, ...)
	void protect_stack_and_other_stuff();
	void set_process_prio_and_stuff();

	// some inline helpers
	inline uint16_t crc_calc(uint16_t * buf, int nwords) const RC_INLINE;
	inline uint32_t sum_words(uint16_t * buf, int nwords, uint32_t sum = 0) const RC_INLINE;
	inline uint16_t wrapsum(uint32_t sum) const RC_INLINE;
	inline void create_udp_frame(char * buf, int const len) const RC_INLINE;
	inline void check_tp_status(tpacket_hdr const * header) const RC_INLINE;
	inline bool check_packet(char const * packet) const RC_INLINE;
	inline bool test_header(tpacket_hdr const * header) const RC_INLINE;

	// kernel ringer buffer
	inline tpacket_hdr * get_tx_header(int const idx) const RC_INLINE;
	inline tpacket_hdr * get_rx_header(int const idx) const RC_INLINE;

	// configuration & setup data
	uint16_t const sport, dport;
	sockaddr_in local_addr, remote_addr;

	int rxringfd, txringfd;
	tpacket_req req;
	ifreq s_ifr;
	sockaddr_ll my_addr;
	void *rx_ring, *tx_ring;
	unsigned int rx_ring_idx, old_rx_ring_idx, tx_ring_idx, old_tx_ring_idx;
	sockaddr_ll * ps_sockaddr;
};








void RealtimeComm::free_receive() {
	while ((old_rx_ring_idx % RING_COUNT) != rx_ring_idx) {
		tpacket_hdr * header = get_rx_header(old_rx_ring_idx);
		// release received frame and update index
		header->tp_status = TP_STATUS_KERNEL;
		//__sync_synchronize(); // senseless
		old_rx_ring_idx = (old_rx_ring_idx + 1) % RING_COUNT;
	}
}

template<typename SpikeType>
inline void RealtimeComm::queue_spike(std::vector<SpikeType> inits) {
	std::vector<char*> locations;
	locations.reserve(inits.size());

	int const data_offset = TPACKET_HDRLEN - sizeof(sockaddr_ll);
	for (size_t i = 0; i < inits.size(); i++) {
		auto tx_header = get_tx_header(tx_ring_idx);
		locations.emplace_back(reinterpret_cast<char*>(tx_header) + data_offset);
		tx_ring_idx = (tx_ring_idx + 1) % RING_COUNT;
	}

	for (size_t i = 0; i < inits.size(); i++) {
		char * data = locations[i];
		new (data + sizeof(iphdr) + sizeof(udphdr)) SpikeType(inits[i]);
	}
}

// FIXME: Multi-spike support!
template<typename SpikeType>
inline void RealtimeComm::queue_spike(SpikeType&& sp_init) {
	// get next free entry (or trigger send?)
	tpacket_hdr * tx_header = get_tx_header(tx_ring_idx);

	// only valid for VERSION 1! (TPACKET2_HDRLEN, ...)
	int const data_offset = TPACKET_HDRLEN - sizeof(sockaddr_ll);
	char * data = reinterpret_cast<char*>(tx_header) + data_offset;

	for (bool tx_loop = true; tx_loop;) {
		switch(tx_header->tp_status) {
			case TP_STATUS_AVAILABLE:
				{
					new (data + sizeof(iphdr) + sizeof(udphdr)) SpikeType(std::forward<SpikeType>(sp_init));
					tx_ring_idx = (tx_ring_idx + 1) % RING_COUNT;
					int data_offset = TPACKET_HDRLEN - sizeof(sockaddr_ll);
					char * data2 = reinterpret_cast<char*>(tx_header) + data_offset;
					create_udp_frame(data2, 1 * sizeof(SpikeType));
					tx_header->tp_len = sizeof(iphdr) + sizeof(udphdr) + 1 * sizeof(SpikeType);
					tx_header->tp_status = TP_STATUS_SEND_REQUEST;
					//old_tx_ring_idx = (old_tx_ring_idx + 1) % RING_COUNT;
					tx_loop = false;
				}
				break;

#ifndef NDEBUG
			case TP_STATUS_WRONG_FORMAT:
				throw std::runtime_error("TP_STATUS_WRONG_FORMAT");
				break;
#endif

			default:
				// just spin or trigger send first?
				//send();
				break;
		}
	}
}

static inline void send_thread(int txringfd, sockaddr_ll * ps_sockaddr) {
	while(true)
		sendto(txringfd, NULL, 0, 0, reinterpret_cast<sockaddr*>(ps_sockaddr), sizeof(sockaddr_ll));
}

inline void RealtimeComm::start_sending_thread() {
	    sender_thread = new std::thread(send_thread, txringfd, ps_sockaddr);
}


void RealtimeComm::send() {
	if (sender_thread != nullptr) return; // skip if threaded send
	//std::async(std::launch::async, [&](){
	sendto(txringfd, NULL, 0, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(ps_sockaddr), sizeof(sockaddr_ll));
	//});
	//while (old_tx_ring_idx != tx_ring_idx) {
	//	tpacket_hdr * tx_header = get_tx_header(old_tx_ring_idx);

	//	// FIXME multi-spike support here...
	//	size_t spikedata_size = 1 * sizeof(spike);

	//	int data_offset = TPACKET_HDRLEN - sizeof(sockaddr_ll);
	//	char * data = reinterpret_cast<char*>(tx_header) + data_offset;

	//	create_udp_frame(data, spikedata_size);

	//	tx_header->tp_len = sizeof(iphdr) + sizeof(udphdr) + spikedata_size;
	//	tx_header->tp_status = TP_STATUS_SEND_REQUEST;
	//
	//	//__sync_synchronize(); // lazy update saves res

	//	old_tx_ring_idx = (old_tx_ring_idx + 1) % RING_COUNT;
	//	// flushing: this sets remote linklayer address for all previous packets???
	//	sendto(txringfd, NULL, 0, 0, reinterpret_cast<sockaddr*>(ps_sockaddr), sizeof(sockaddr_ll));
#ifndef NDEBUG
	//	if (sendto(txringfd, NULL, 0, 0, reinterpret_cast<sockaddr*>(ps_sockaddr), sizeof(sockaddr_ll)) == -1) {
	//		throw std::runtime_error(std::string("Could not send to remote: ") + strerror(errno));
	//	}
#endif
	//}
}


template<typename SpikeType>
void RealtimeComm::send_single_spike(SpikeType&& sp_init) {
	queue_spike(std::forward<SpikeType>(sp_init));
	send();
}

template<typename InputIt>
double stdev(InputIt first, InputIt last) {
	double dsum = std::accumulate(first, last, 0.0);
	double dmean = dsum / std::distance(first, last);
	double dsq_sum = std::inner_product(first, last, first, 0.0);
	return std::sqrt(dsq_sum / std::distance(first, last) - dmean * dmean);
}

RealtimeComm::timepoint_t RealtimeComm::gettime() {
	timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
#ifndef NDEBUG
		throw std::runtime_error(std::string("gettime() failed: ") + strerror(errno));
#else
		;
#endif
	}
	_curtime = t.tv_sec * 1E9 + t.tv_nsec;
	return _offset + _curtime;
}

RealtimeComm::SyncStatus RealtimeComm::sync() {
	size_t const iters = 10000;

	if (!master) {
		bool looping = true;
		while(looping) {
			auto sp = receive_and_spin<Spike::dummy>();
			if (sp->packet_type != Spike::dummy::SYNC)
				looping = false;
			queue_spike<Spike::dummy>({sp->timestamp0, gettime(), 0, Spike::dummy::SYNC});
			send();
			free_receive();
		}
		return SyncStatus();
	} else {
		std::vector<uint64_t> rtt(iters);
		std::vector<int64_t> clkdiff(iters);
		for(size_t i = 0; i < rtt.size(); i++) {
			queue_spike<Spike::dummy>({gettime(), curtime(), 0, Spike::dummy::SYNC});
			send();
			auto sp = receive_and_spin<Spike::dummy>();
			rtt[i] = gettime() - sp->timestamp0;

			// update delay/offset adjusts
			_delay  = 0.9*_delay  + 0.1*rtt[i]/2;
			_offset = 0.99*_offset + 0.01*((sp->timestamp + _delay/*rtt[i]/2*/) - _curtime);

			clkdiff[i] = curtime() - (sp->timestamp + _delay);
			free_receive();
		}

		// end sync mode
		queue_spike<Spike::dummy>({0, 0, 0, Spike::dummy::SPIKES});
		send();

		// skip first part of the delay and offset measurements
		float skip_part = 0.8;
		auto clkdiff_start = clkdiff.begin();
		std::advance(clkdiff_start, skip_part*iters);
		auto rtt_start = rtt.begin();
		std::advance(rtt_start, skip_part*iters);

		return SyncStatus{
			static_cast<double>(_delay),
			stdev(rtt_start, rtt.end()),
			static_cast<double>(_offset),
			stdev(clkdiff_start, clkdiff.end())
		};
	}
}

template<typename SpikeType>
SpikeType const * RealtimeComm::receive_and_spin() {
	while(true) {
		tpacket_hdr * header = get_rx_header(rx_ring_idx);

		// alignment
		assert((((unsigned long) header) & (getpagesize() - 1)) == 0);

		// loop until packet there
		while (test_header(header)) {
			// nop
		}

		check_tp_status(header);

		// packet begins here (w/o ethernet header)
		char * packet  = reinterpret_cast<char*>(header) + header->tp_net;

		rx_ring_idx = (rx_ring_idx + 1) % RING_COUNT;

		// drop all other frames (match ip, udp, port 2013... etc)
		if (!check_packet(packet)) {
			//std::cout << "dropping" << std::endl;
			header->tp_status = TP_STATUS_KERNEL;
			continue;
		}

		// payload here
		udphdr * uh = reinterpret_cast<udphdr*>(packet + sizeof(iphdr) /*+ sizeof(udphdr)*/);

		size_t number_of_spikes_in_packet = (ntohs(uh->len) - sizeof(udphdr)) / sizeof(SpikeType);

#ifndef NDEBUG
		if (number_of_spikes_in_packet > (header->tp_len / sizeof(SpikeType)))
			throw std::runtime_error("packet length wrong!");
#else
		static_cast<void>(number_of_spikes_in_packet);
#endif

		SpikeType * sp = reinterpret_cast<SpikeType*>(reinterpret_cast<char*>(uh) + sizeof(udphdr));
		return sp;
	}
}


// block to receive and return copy (!) of user pdu; user has to free when done
template<typename SpikeType>
std::vector<SpikeType*> & RealtimeComm::receive() {
	static std::vector<SpikeType*> received_items(100);

	// update time
	gettime();

	received_items.clear();
	while(true) {
		tpacket_hdr * header = get_rx_header(rx_ring_idx);

		// alignment
		assert((((unsigned long) header) & (getpagesize() - 1)) == 0);

		// return empty spike set if nothing in buffer
		if (test_header(header)) {
			return received_items;
		}

		check_tp_status(header);

		// packet begins here (w/o ethernet header)
		char * packet  = reinterpret_cast<char*>(header) + header->tp_net;

		rx_ring_idx = (rx_ring_idx + 1) % RING_COUNT;

		// drop all other frames (match ip, udp, port 2013... etc)
		if (!check_packet(packet)) {
			//std::cout << "dropping" << std::endl;
			header->tp_status = TP_STATUS_KERNEL;
			continue;
		}

		// payload here
		udphdr * uh = reinterpret_cast<udphdr*>(packet + sizeof(iphdr) /*+ sizeof(udphdr)*/);

		size_t number_of_spikes_in_packet = (ntohs(uh->len) - sizeof(udphdr)) / sizeof(SpikeType);

#ifndef NDEBUG
		if (number_of_spikes_in_packet > (header->tp_len / sizeof(SpikeType)))
			throw std::runtime_error("packet length wrong!");
#endif

		SpikeType * base = reinterpret_cast<SpikeType*>(reinterpret_cast<char*>(uh) + sizeof(udphdr));
		SpikeType * sp = base;
		for (; sp < (base + number_of_spikes_in_packet); sp++) {
			received_items.push_back(sp);
		}
	}
#ifndef NDEBUG
	throw std::runtime_error("WTF?!?!");
#endif
}



uint32_t RealtimeComm::sum_words(uint16_t * buf, int nwords, uint32_t sum) const {
	for(; nwords > 0; nwords--)
		sum += *buf++;
	return sum;
}

uint16_t RealtimeComm::wrapsum(uint32_t sum) const {
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16); // instead of while(sum >> 16) above
	return static_cast<uint16_t>(~sum);
}

// CRC: 16 bit one's complement of the one's complement of all 16 bit words
// (checksum field assumed to be zero)
uint16_t RealtimeComm::crc_calc(uint16_t * buf, int nwords) const {
	return wrapsum(sum_words(buf, nwords));
}


void RealtimeComm::create_udp_frame(char * buf, int const len) const {

	//memset(buf, 0, sizeof(iphdr) + sizeof(udphdr));
	iphdr *ip = reinterpret_cast<iphdr*>(buf);
	udphdr *udp = reinterpret_cast<udphdr*>(buf + sizeof(iphdr));
	uint16_t const udp_len = sizeof(udphdr) + len; // in bytes...

	ip->ihl = 5; /* header length in 32bit words; minimum length == 5 */
	ip->version = 4;
	ip->tos = 0x0;
	ip->tot_len = htons(sizeof(udphdr) + sizeof(iphdr) + len);
	ip->id = 0;
	ip->frag_off = htons(0x4000); /* Don't Fragment */
	ip->ttl = 64; /* default value */
	ip->protocol = 17; /* UDP */
	// ip->check checksum at the end
	ip->check = 0;
	ip->saddr = local_addr.sin_addr.s_addr;
	ip->daddr = remote_addr.sin_addr.s_addr;
	ip->check = crc_calc(reinterpret_cast<uint16_t*>(ip), 2 * ip->ihl);

	udp->source = htons(sport);
	udp->dest = htons(dport);
	udp->len = htons(udp_len);
	udp->check = 0;

	uint32_t sum = 0;
	// pseudo header
	sum = sum_words(reinterpret_cast<uint16_t*>(&ip->saddr), 2, sum);
	sum = sum_words(reinterpret_cast<uint16_t*>(&ip->daddr), 2, sum);
	sum = sum + htons(ip->protocol); // single byte above, but here with padded zeros... => fix byte order!
	sum = sum_words(reinterpret_cast<uint16_t*>(&udp->len), 1, sum);

	// udp header + data checksum
	sum = sum_words(reinterpret_cast<uint16_t*>(udp), (udp_len % 2) ? (udp_len/2 + 1) : (udp_len/2), sum);
	udp->check = wrapsum(sum);
}


bool RealtimeComm::check_packet(char const * packet) const {
	iphdr  const * ip  = reinterpret_cast<iphdr  const *>(packet);
	udphdr const * udp = reinterpret_cast<udphdr const *>(packet + sizeof(iphdr));

	bool ret = true
	           && (ip->saddr   == remote_addr.sin_addr.s_addr)
	           && (ip->daddr   == local_addr.sin_addr.s_addr)
	           && (udp->source == htons(dport)) // inverse ports :)
	           && (udp->dest   == htons(sport));

	return ret;
}

tpacket_hdr * RealtimeComm::get_rx_header(int const idx) const {
	return reinterpret_cast<tpacket_hdr*>(reinterpret_cast<char*>(rx_ring) + idx * req.tp_frame_size);
}

tpacket_hdr * RealtimeComm::get_tx_header(int const idx) const {
	return reinterpret_cast<tpacket_hdr*>(reinterpret_cast<char*>(tx_ring) + idx * req.tp_frame_size);
}

bool RealtimeComm::test_header(tpacket_hdr const * header) const {
	//__sync_synchronize();
	return (*const_cast<volatile unsigned long*>(&header->tp_status) == TP_STATUS_KERNEL);
}

void RealtimeComm::check_tp_status(tpacket_hdr const * header) const {
#ifndef NDEBUG
	// check status of received packet
	if (header->tp_status & TP_STATUS_COPY)
		throw std::runtime_error("incomplete packet in rx ring");
	//if (header->tp_status & TP_STATUS_LOSING)
	//	throw std::runtime_error("dropped packets in rx ring");
	//if (header->tp_status & TP_STATUS_CSUMNOTREADY)
	//	// outgoing packet! drop for rx? let's see...
	//	throw std::runtime_error("checksum problem?");
	if (header->tp_len != header->tp_snaplen)
		throw std::runtime_error("capture missed some bytes?");
	// NOTE: if RealtimeComm class isn't called and lots of other traffic is seen on
	// device => drop (limited buffer sizes!) => TODO: implement BPF?
#else
	static_cast<void>(header);
#endif
}

inline std::ostream & operator<<(std::ostream & os, RealtimeComm::SyncStatus const & status) {
	auto old_flags = os.flags();
	auto old_prec  = os.precision();
	os.precision(9);
	os.setf(std::ios::fixed, std::ios::floatfield);

	os << "End2End Latency [us]: "
	          << std::setw(15) << 1.0E-3*status.delay << " +/- " << std::left
	          << std::setw(15) << 1.0E-3*status.delay_stdev << std::right << "\n";
	os << "Clock Offset    [ s]: "
	          << std::setw(15) << 1.0E-9*status.offset << " +/- " << std::left
	          << std::setw(15) << 1.0E-9*status.offset_stdev << std::right << "\n";

	os.setf(old_flags);
	os.precision(old_prec);

	return os;
}
}
