#ifndef FRONTEND_WINDOWED_H

#define EMIT_SPECIAL_KEYS \
	X( ESCAPE       ) \
	X( BACKSPACE    ) \
	X( TAB          ) \
	X( ENTER        ) \
	X( HOME         ) \
	X( END          ) \
	X( INSERT       ) \
	X( DELETE       ) \
	X( PAGE_UP      ) \
	X( PAGE_DOWN    ) \
	X( ARROW_UP     ) \
	X( ARROW_DOWN   ) \
	X( ARROW_LEFT   ) \
	X( ARROW_RIGHT  ) \
	X( PRINT_SCREEN ) \
	X( F1           ) \
	X( F2           ) \
	X( F3           ) \
	X( F4           ) \
	X( F5           ) \
	X( F6           ) \
	X( F7           ) \
	X( F8           ) \
	X( F9           ) \
	X( F10          ) \
	X( F11          ) \
	X( F12          ) \
	X( F13          ) \
	X( F14          ) \
	X( F15          ) \
	X( F16          ) \
	X( F17          ) \
	X( F18          ) \
	X( F19          ) \
	X( F20          ) \
	X( F21          ) \
	X( F22          ) \
	X( F23          ) \
	X( F24          ) \
	X( CONTROL      ) \
	X( ALT          ) \
	X( SHIFT        ) \
	X( META         )

enum special_key {
	// keys representing printable characters are encoded with their unicode
	// codepoint. codepoints require at most 21 bits.
	SPECIAL_KEY_BEGIN = 1<<21,
	#define X(NAME) KEY_##NAME,
	EMIT_SPECIAL_KEYS
	#undef X
};

enum modifier_key_flag {
	MOD_SHIFT   = (1<<22),
	MOD_CONTROL = (1<<23),
	MOD_ALT     = (1<<24),
	MOD_META    = (1<<25),
};

#define KEY_MASK ((1<<22)-1)

#define FRONTEND_WINDOWED_H
#endif
