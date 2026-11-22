// Memory map and entry points for the SL6801 (yp3_2.0.43) flash dump. Run as a headless -preScript
// on a raw import of the 2 MB dump at base 0xC00000 with processor ARM:LE:32:Cortex.
// Programs whose name contains "boot" get the bootloader layout, all others the app layout.
// Optional script argument: a boot ROM dump, mapped at 0 with the names in ROM_FUNCTIONS.
//@category SL6801
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.data.*;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;

public class SL6801Setup extends GhidraScript {
	static final long FLASH = 0xC00000L;
	static final int ROM_SIZE = 0x60000; // content ends at 0x5BEB4

	// Boot ROM functions identified so far (see README, "Boot ROM"). Most are called by the firmware.
	static final Object[][] ROM_FUNCTIONS = {
		// boot
		{ 0x40L, "rom_reset" }, { 0x4c2b8L, "rom_boot_stage" }, { 0x4c914L, "rom_boot" },
		{ 0x4c9c8L, "rom_main" }, { 0x4c8a0L, "rom_usb_download" }, { 0x4cd7cL, "rom_flash_boot" },
		{ 0x4cc74L, "rom_find_header" }, { 0x4cbe8L, "rom_read_header" }, { 0x4ccb8L, "rom_load_image" },
		// debug UART and printf
		{ 0x210L, "rom_uart_putc" }, { 0x222L, "rom_uart_getc" }, { 0x27cL, "rom_putchar" },
		{ 0x2a0L, "rom_printf_init" }, { 0x2b0L, "rom_printf" },
		// C library
		{ 0xa32L, "rom_strcpy" }, { 0xaa2L, "rom_strlen" }, { 0xab2L, "rom_strcmp" }, { 0xb2aL, "rom_memcpy" },
		{ 0xbaaL, "rom_memset" }, { 0xc1eL, "rom_memmove" }, { 0xc54L, "rom_memcmp" }, { 0xc70L, "rom_malloc" },
		{ 0xc74L, "rom_free" }, { 0xc78L, "rom_zalloc" }, { 0xc90L, "rom_qsort" }, { 0xd1eL, "rom_atoi" },
		// compiler helpers (double precision and 64-bit division)
		{ 0x1720L, "__aeabi_dsub" }, { 0x1724L, "__aeabi_dadd" }, { 0x199cL, "__aeabi_ui2d" },
		{ 0x19bcL, "__aeabi_i2d" }, { 0x19e0L, "__aeabi_f2d" }, { 0x1a24L, "__aeabi_ul2d" },
		{ 0x1a90L, "__aeabi_dmul" }, { 0x1ce4L, "__aeabi_ddiv" }, { 0x1eb4L, "__aeabi_d2uiz" },
		{ 0x1ef4L, "__aeabi_ldivmod" }, { 0x1f94L, "__aeabi_uldivmod" },
		// 16-bit PCM conversions
		{ 0x12d4L, "rom_pcm_stereo_to_mono" }, { 0x12f8L, "rom_pcm_s16_to_s32" },
		{ 0x13eaL, "rom_pcm_mono_to_stereo" }, { 0x1428L, "rom_pcm_mono_to_stereo_s32" },
		// clocks, pins, SPI flash, timer
		{ 0x2518L, "rom_clk_is_enabled" }, { 0x2564L, "rom_clk_enable" }, { 0x2570L, "rom_clk_disable" },
		{ 0x77aL, "rom_pin_decode" }, { 0x7acL, "rom_pin_config" }, { 0x7ecL, "rom_gpio_get" },
		{ 0x83b4L, "rom_flash_read" }, { 0x8500L, "rom_flash_crc_get" }, { 0x8510L, "rom_flash_crc_start" },
		{ 0x8528L, "rom_flash_crc_stop" }, { 0xb5e8L, "rom_delay_init" }, { 0xb6d8L, "rom_timer_read64" },
		{ 0xb730L, "rom_spin" }, { 0xb73eL, "rom_delay_us" }, { 0xb764L, "rom_delay_ms" },
		{ 0xb78cL, "rom_os_delay_ms" },
		// FreeRTOS kernel (Cortex-M4F port, 1 kHz tick)
		{ 0x8a30L, "xTaskCreate" }, { 0x8bf4L, "vTaskDelete" }, { 0x8d80L, "xEventGroupCreate" },
		{ 0x8d9aL, "xEventGroupWaitBits" }, { 0x8e50L, "xEventGroupClearBits" },
		{ 0x8ea8L, "xEventGroupSetBits" }, { 0x8f94L, "vEventGroupDelete" }, { 0x9068L, "xQueueGenericCreate" },
		{ 0x90c4L, "xQueueCreateMutex" }, { 0x92f4L, "xQueueGiveFromISR" }, { 0x936aL, "xQueueReceiveFromISR" },
		{ 0x9442L, "vQueueDelete" }, { 0x94f0L, "xPortIsInsideInterrupt" }, { 0x9d50L, "pvPortMalloc" },
		{ 0x9e3cL, "vPortFree" }, { 0x9e88L, "xPortGetFreeHeapSize" }, { 0x9d28L, "rom_heap_init" },
		{ 0x9f50L, "prvListTasksWithinSingleList" }, { 0xb70cL, "rom_run_time_counter" }, { 0xa2a4L, "vTaskSuspendAll" },
		{ 0xa2b4L, "xTaskGetTickCount" }, { 0xa2c0L, "xTaskGetTickCountFromISR" },
		{ 0xa2ccL, "uxTaskGetNumberOfTasks" }, { 0xa2d8L, "pcTaskGetName" }, { 0xa428L, "xTaskResumeAll" },
		{ 0xa500L, "vTaskDelay" }, { 0xa564L, "uxTaskGetSystemState" }, { 0xace0L, "xTaskNotifyWait" },
		{ 0xb1c0L, "xQueueGenericSend" }, { 0xb372L, "xQueueSemaphoreTake" }, { 0xb514L, "vPortEnterCritical" },
		{ 0xb534L, "vPortExitCritical" }, { 0xb548L, "xPortPendSVHandler" },
		// memory pool used by the audio code. Each first calls its hook if set: rom_pool_* those at
		// 0x800988 + 0x20/0x24/0x30, rom_mem_* those at 0x8009CC + 0/4 (else rom_pool_calloc/free).
		{ 0x1100cL, "rom_pool_alloc" }, { 0x11044L, "rom_pool_calloc" }, { 0x110c0L, "rom_pool_free" },
		{ 0x4b29cL, "rom_mem_calloc" }, { 0x4b2acL, "rom_mem_free" },
		// table-based float approximations (hard-float ABI); the 0x12xxx copies are identical
		{ 0x48228L, "rom_log2f" }, { 0x48268L, "rom_exp2f" }, { 0x4840cL, "rom_cosf" }, { 0x48488L, "rom_sinf" },
		{ 0x128d8L, "rom_log2f_2" }, { 0x12918L, "rom_exp2f_2" }, { 0x129ccL, "rom_cosf_2" },
		{ 0x12a48L, "rom_sinf_2" },
	};

	byte[] flash;

	MemoryBlock init(String name, long addr, long flashOff, int len, boolean exec) throws Exception {
		byte[] b = new byte[len];
		System.arraycopy(flash, (int) flashOff, b, 0, len);
		MemoryBlock m = currentProgram.getMemory().createInitializedBlock(name, toAddr(addr),
			new ByteArrayInputStream(b), len, monitor, false);
		m.setPermissions(true, true, exec);
		m.setComment(String.format("copied from flash offset 0x%x", flashOff));
		return m;
	}

	MemoryBlock uninit(String name, long addr, long len, boolean volatil) throws Exception {
		MemoryBlock m = currentProgram.getMemory().createUninitializedBlock(name, toAddr(addr), len, false);
		m.setPermissions(true, true, false);
		m.setVolatile(volatil);
		return m;
	}

	int u32(long off) {
		return (flash[(int) off] & 0xff) | (flash[(int) off + 1] & 0xff) << 8 | (flash[(int) off + 2] & 0xff) << 16
			| (flash[(int) off + 3] & 0xff) << 24;
	}

	void vectors(long ramAddr, long flashOff, int count) throws Exception {
		Address a = toAddr(ramAddr);
		for (int i = 0; i < count; i++) {
			createData(a.add(4 * i), PointerDataType.dataType);
		}
		createLabel(a, "vector_table", true, SourceType.IMPORTED);
		for (int i = 1; i < count; i++) {
			long v = u32(flashOff + 4L * i) & 0xffffffffL;
			if ((v & 1) == 0 || v == 0) continue;
			Address t = toAddr(v & ~1L);
			if (currentProgram.getMemory().contains(t)) {
				String n = i == 1 ? "Reset_Handler" : String.format("vector_%d", i);
				addEntryPoint(t);
				createFunction(t, i == 1 ? n : null);
			}
		}
	}

	@Override
	public void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		MemoryBlock fb = mem.getBlock(toAddr(FLASH));
		fb.setName("flash");
		fb.setPermissions(true, false, true);
		flash = new byte[(int) fb.getSize()];
		mem.getBytes(toAddr(FLASH), flash);
		boolean boot = currentProgram.getName().toLowerCase().contains("boot");

		String[] args = getScriptArgs();
		File romFile = args.length > 0 ? new File(args[0]) : null;
		if (romFile != null && romFile.isFile()) {
			byte[] dump = Files.readAllBytes(romFile.toPath());
			byte[] rom = Arrays.copyOf(dump, ROM_SIZE);
			MemoryBlock romBlock = mem.createInitializedBlock("rom", toAddr(0), new ByteArrayInputStream(rom),
				rom.length, monitor, false);
			romBlock.setPermissions(true, false, true);
			romBlock.setComment("boot ROM, from " + romFile.getName());
			for (Object[] f : ROM_FUNCTIONS) {
				Address a = toAddr((Long) f[0]);
				if (getFunctionAt(a) == null) {
					disassemble(a);
					createFunction(a, (String) f[1]);
				} else {
					getFunctionAt(a).setName((String) f[1], SourceType.IMPORTED);
				}
			}
			println("ROM functions named: " + ROM_FUNCTIONS.length);
		} else {
			// Without a dump, fill the ROM with "bx lr" so calls into it are treated as returning
			// functions instead of cutting off the caller's decompilation.
			byte[] rom = new byte[0x20000];
			for (int i = 0; i < rom.length; i += 2) {
				rom[i] = 0x70;
				rom[i + 1] = 0x47;
			}
			MemoryBlock romBlock = mem.createInitializedBlock("rom_stub", toAddr(0), new ByteArrayInputStream(rom),
				rom.length, monitor, false);
			romBlock.setPermissions(true, false, true);
			romBlock.setComment("placeholder: every halfword is bx lr (no ROM dump given)");
			for (long a : new long[] { 0x2b0, 0x2564, 0xb2a }) {
				disassemble(toAddr(a));
			}
			createFunction(toAddr(0x2b0), "rom_printf");
			createFunction(toAddr(0x2564), "rom_clk_enable");
			createFunction(toAddr(0xb2a), "rom_memcpy");
		}
		uninit("periph", 0x40000000L, 0x100000, true);
		uninit("scs", 0xE000E000L, 0x1000, true);

		if (boot) {
			fb.setExecute(false); // keep the bootloader analysis out of the app code
			// HLKJ header: load 0x81FBC0 from flash 0x60, 0xC8EC bytes
			uninit("ram_lo", 0x800000, 0x81FBC0 - 0x800000, false);
			init("boot", 0x81FBC0, 0x60, 0xC8EC, true);
			uninit("ram_hi", 0x81FBC0 + 0xC8EC, 0x870000 - (0x81FBC0 + 0xC8EC), false);
			vectors(0x81FBC0, 0x60, 0x5B);
		} else {
			// FIRM: 0x1000-byte block at flash 0xE030 runs at 0x804800; .data image at flash 0x39470
			// is copied to 0x80CC40..0x81AAEC; .bss is 0x81AAEC..0x86B710 (initial SP).
			uninit("ram_lo", 0x800000, 0x4800, false);
			init("ram_vec", 0x804800, 0xE030, 0x1000, true);
			uninit("ram_gap", 0x805800, 0x80CC40 - 0x805800, false).setExecute(true);
			init(".data", 0x80CC40, 0x39470, 0x81AAEC - 0x80CC40, true);
			uninit(".bss", 0x81AAEC, 0x86B710 - 0x81AAEC, false);
			uninit("ram_hi", 0x86B710, 0x870000 - 0x86B710, false);
			vectors(0x804800, 0xE030, 0x5B);
			createLabel(toAddr(FLASH + 0xE000), "FIRM_header", true, SourceType.IMPORTED);
			createLabel(toAddr(FLASH + 0x53154), "ui_strings", true, SourceType.IMPORTED);
			createLabel(toAddr(FLASH + 0x48770), "ui_language_tables", true, SourceType.IMPORTED);

			// Seed functions from Thumb pointers into the two code ranges of FIRM.
			long[][] code = { { FLASH + 0xE030, FLASH + 0x18A8E }, { FLASH + 0xD6000, FLASH + 0x182270 } };
			int seeded = 0;
			for (long o = 0xE000; o < 0x182270; o += 4) {
				long v = u32(o) & 0xffffffffL;
				if ((v & 1) == 0) continue;
				for (long[] r : code) {
					if (v >= r[0] && v < r[1]) {
						Address t = toAddr(v & ~1L);
						if (getFunctionAt(t) == null && disassemble(t)) {
							createFunction(t, null);
							seeded++;
						}
					}
				}
			}
			println("seeded functions from pointers: " + seeded);
		}
		createLabel(toAddr(FLASH), "HLKJ_header", true, SourceType.IMPORTED);
	}
}
