#ifndef PROTOCOL_H

enum {
	// ===========================
	// === WS0_* is peer=>host ===
	// ===========================

	WS0_HELLO = 1,
	// the first "hello" the peer sends to the host

	WS0_MIM,
	// peer mim (editor protocol) commands sent to host


	// ===========================
	// === WS1_* is host=>peer ===
	// ===========================

	WS1_HELLO,
	// response to WS0_HELLO

	WS1_JOURNAL_UPDATE,
	// host sends new journal data to peer
};

#define PROTOCOL_H
#endif
