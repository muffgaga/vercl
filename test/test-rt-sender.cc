#include "vercl/RealtimeComm.h"

int main() {
	HMF::RealtimeComm r("10.0.0.3", 2345, 2345);
	r.send_single_spike<Spike::dummy>({0, 0, 1, 2});
}
