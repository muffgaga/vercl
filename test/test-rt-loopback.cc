#include "vercl/RealtimeComm.h"

int main() {
	HMF::RealtimeComm r("10.0.0.2", 2345, 2345);

	do {
		auto sp = r.receive_and_spin<Spike::dummy>();
		r.send_single_spike<Spike::dummy>({sp->timestamp0, sp->timestamp, sp->label, sp->packet_type}); // FIXME
		r.free_receive();
		std::cout << "looping back spike -> label: " << sp->label << std::endl;
	} while(true);
}
