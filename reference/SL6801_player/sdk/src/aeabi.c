/* Compiler helpers for 64-bit division and double-precision math, forwarded to the boot ROM's
 * copies of libgcc's. Each is a tail jump, so arguments and results pass through untouched.
 *
 * libgcc keeps related helpers in one object (add/sub/convert, mul/div). Defining only part of
 * such a group would make the linker pull the object in for the rest and fail on duplicate
 * symbols, so each group here is complete. Double comparisons, __aeabi_d2iz and __aeabi_d2f are
 * not in the ROM and still come from libgcc.
 *
 * The ROM's add/sub predates a libgcc fix for results at the edge of the denormal range; the
 * other helpers are byte for byte the same as GCC 15's. */

#define ROM_HELPER(name, addr) \
	__attribute__((naked)) void name(void) \
	{ \
		__asm__ volatile("movw ip, #" #addr " + 1\n\tbx ip"); \
	}

ROM_HELPER(__aeabi_drsub, 0x1718)
ROM_HELPER(__aeabi_dsub, 0x1720)
ROM_HELPER(__aeabi_dadd, 0x1724)
ROM_HELPER(__aeabi_ui2d, 0x199C)
ROM_HELPER(__aeabi_i2d, 0x19BC)
ROM_HELPER(__aeabi_f2d, 0x19E0)
ROM_HELPER(__aeabi_ul2d, 0x1A24)
ROM_HELPER(__aeabi_l2d, 0x1A34)
ROM_HELPER(__aeabi_dmul, 0x1A90)
ROM_HELPER(__aeabi_ddiv, 0x1CE4)
ROM_HELPER(__aeabi_d2uiz, 0x1EB4)
ROM_HELPER(__aeabi_ldivmod, 0x1EF4)
ROM_HELPER(__aeabi_uldivmod, 0x1F94)
