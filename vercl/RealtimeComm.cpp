#include "RealtimeComm.h"

HMF::RealtimeComm::RealtimeComm(
		std::string const remote_ip,
		uint16_t const sport,
		uint16_t const dport) :
	sport(sport),
	dport(dport),
	master(true)
{
	// get local ip of device ETH_NAME
	int fd;
	struct ifreq ifr;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, ETH_NAME, IFNAMSIZ-1);
	ioctl(fd, SIOCGIFADDR, &ifr);
	close(fd);

	std::string ip_local = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
	std::cout << "using local ip " << ip_local << std::endl;

	// get remote mac -- FIXME: de-ugly-fy!
	std::string cmd;
	cmd += "LANG=C arping ";
	cmd += remote_ip;
	cmd += " -c 1 | grep from | cut -d' ' -f4";
	FILE* pipe = popen(cmd.c_str(), "r");
	assert(pipe);
	char buffer[128];
	std::string result = "";
	while(!feof(pipe)) {
		if(fgets(buffer, 128, pipe) != NULL)
			result += buffer;
	}
	pclose(pipe);
	uint8_t mac_remote[6];
	for (int i=0; i<6; i++) {
		int t;
		std::stringstream s;
		std::string subs = result.substr(i*3, 2);
		s << std::hex << subs;
		s >> t;
		mac_remote[i] = t;
	}
	std::cout << "remote ip " << remote_ip << " mac " << result;

	// common init
	initial_config(ip_local, remote_ip, mac_remote);
}

HMF::RealtimeComm::RealtimeComm(
		std::string const local_ip,
		std::string const remote_ip,
		uint16_t const sport,
		uint16_t const dport,
		uint8_t remote_mac[ETH_ALEN],
		bool master) :
	sport(sport),
	dport(dport),
	master(master)
{
	initial_config(local_ip, remote_ip, remote_mac);
}

HMF::RealtimeComm::~RealtimeComm() {
	close(rxringfd);
	close(txringfd);
}

void HMF::RealtimeComm::initial_config(
		std::string local_ip,
		std::string remote_ip,
		uint8_t remote_mac[ETH_ALEN]
) {
	_curtime = 0;
	_offset = 0;
	_delay = 0;
	sender_thread = nullptr;

	local_addr.sin_family = AF_INET;
	remote_addr.sin_family = AF_INET;
	local_addr.sin_port = htons(sport);
	remote_addr.sin_port = htons(dport);
	inet_pton(AF_INET, local_ip.c_str(),  reinterpret_cast<in_addr*>(&local_addr.sin_addr.s_addr));
	inet_pton(AF_INET, remote_ip.c_str(), reinterpret_cast<in_addr*>(&remote_addr.sin_addr.s_addr));

	// mlocking stuff
	protect_stack_and_other_stuff();
	// process stuff
	set_process_prio_and_stuff();

	// get rx/tx (cooked, promisc) sockets
	rxringfd = socket(PF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (rxringfd == -1)
		throw std::runtime_error(std::string("socket call failed: ") + strerror(errno));
	txringfd = socket(PF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (txringfd == -1)
		throw std::runtime_error(std::string("socket call failed: ") + strerror(errno));

	// set RX RING stuff
	req.tp_block_size = RING_COUNT * getpagesize();
	req.tp_block_nr = 1; // simple math ;)
	req.tp_frame_size = getpagesize();
	req.tp_frame_nr = RING_COUNT;

	if (setsockopt(rxringfd, SOL_PACKET, PACKET_RX_RING, reinterpret_cast<void*>(&req), sizeof(req)))
		throw std::runtime_error(std::string("setsockopt failed with: ") + strerror(errno) );
	if (setsockopt(txringfd, SOL_PACKET, PACKET_TX_RING, reinterpret_cast<void*>(&req), sizeof(req)))
		throw std::runtime_error(std::string("setsockopt failed with: ") + strerror(errno) );

	// TPACKET_V2 adds vlan support; who cares...
	// we could getsockopt/setsockopt to change version here

	// check buffer sizes of socket
	uint32_t bufsz = 0;
	socklen_t optsz = sizeof(bufsz);
	assert(0 == getsockopt(rxringfd, SOL_SOCKET, SO_RCVBUF, (void *)&bufsz, &optsz));
	if (bufsz < 1024*1024)
		throw std::runtime_error("SO_RCVBUF too small");
	assert(0 == getsockopt(txringfd, SOL_SOCKET, SO_SNDBUF, (void *)&bufsz, &optsz));
	if (bufsz < 1024*1024)
		throw std::runtime_error("SO_SNDBUF too small");

	// attach both rings to some ethernet device
	memset(&s_ifr, 0, sizeof(ifreq));
	strncpy (s_ifr.ifr_name, ETH_NAME, sizeof(s_ifr.ifr_name));
	if (ioctl(rxringfd, SIOCGIFINDEX, &s_ifr) == -1)
		throw std::runtime_error(std::string("ioctl failed: ") + strerror(errno));
	if (ioctl(txringfd, SIOCGIFINDEX, &s_ifr) == -1)
		throw std::runtime_error(std::string("ioctl failed: ") + strerror(errno));

	// bind both rings to PACKET socket
	memset(&my_addr, 0, sizeof(sockaddr_ll));
	my_addr.sll_family = AF_PACKET;
	my_addr.sll_protocol = htons(ETH_P_ALL);
	my_addr.sll_ifindex =  s_ifr.ifr_ifindex;
	if (bind(rxringfd, reinterpret_cast<sockaddr*>(&my_addr), sizeof(sockaddr_ll)) == -1)
		throw std::runtime_error(std::string("rxring bind failed with: ") + strerror(errno));
	if (bind(txringfd, reinterpret_cast<sockaddr*>(&my_addr), sizeof(sockaddr_ll)) == -1)
		throw std::runtime_error(std::string("txring bind failed with: ") + strerror(errno));

	// map both rings to our process space
	rx_ring = mmap(0, req.tp_block_size*req.tp_block_nr, PROT_READ|PROT_WRITE, MAP_SHARED, rxringfd, 0);
	if (rx_ring == reinterpret_cast<void*>(-1))
		throw std::runtime_error(std::string("mmap of rx_ring failed with: ") + strerror(errno));
	tx_ring = mmap(0, req.tp_block_size*req.tp_block_nr, PROT_READ|PROT_WRITE, MAP_SHARED, txringfd, 0);
	if (tx_ring == reinterpret_cast<void*>(-1))
		throw std::runtime_error(std::string("mmap of tx_ring failed with: ") + strerror(errno));
	assert(rx_ring != tx_ring);

	// ring indices
	rx_ring_idx = 0;
	old_rx_ring_idx = rx_ring_idx;
	tx_ring_idx = 0;
	old_tx_ring_idx = tx_ring_idx;

	// remote MAC (FIXME: it's fixed... we could set by ip option?)
	ps_sockaddr = new sockaddr_ll;
	ps_sockaddr->sll_family = AF_PACKET;
	ps_sockaddr->sll_protocol = htons(ETH_P_IP);
	ps_sockaddr->sll_ifindex = s_ifr.ifr_ifindex;
	ps_sockaddr->sll_halen = ETH_ALEN;
	memcpy(&(ps_sockaddr->sll_addr), remote_mac, ETH_ALEN);
}

void HMF::RealtimeComm::protect_stack_and_other_stuff() {
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
		throw std::runtime_error(std::string("mlockall failed: ") + strerror(errno));
	}
	// 8k pre-faulted of stack :)
#define MAX_SAFE_STACK (8*1024)
	unsigned char dummy[MAX_SAFE_STACK];
	memset(dummy, 0, MAX_SAFE_STACK);
}

void HMF::RealtimeComm::set_process_prio_and_stuff() {
	//sched_param p = { 50 }; // prio
	//sched_setscheduler(0 /*own process*/, SCHED_FIFO, &p);
	//nice(-15);
	cpu_set_t afmask;
	CPU_ZERO(&afmask);
	CPU_SET(0x1, &afmask); // core one only
	CPU_SET(0x2, &afmask); // core one only
	sched_setaffinity(0, sizeof(cpu_set_t), &afmask);
}

