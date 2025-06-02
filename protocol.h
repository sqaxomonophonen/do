#ifndef PROTOCOL_H

enum {
	// WS0 is peer=>host
	WS0_SET_JOURNAL_CURSOR = 1,
	WS0_MIM,

	// WS1 is host=>peer
	WS1_JOURNAL_UPDATE,
};

#define PROTOCOL_H
#endif
