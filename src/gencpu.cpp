/*
* UAE - The Un*x Amiga Emulator
*
* MC68000 emulation generator
*
* This is a fairly stupid program that generates a lot of case labels that
* can be #included in a switch statement.
* As an alternative, it can generate functions that handle specific
* MC68000 instructions, plus a prototype header file and a function pointer
* array to look up the function for an opcode.
* Error checking is bad, an illegal table68k file will cause the program to
* call abort().
* The generated code is sometimes sub-optimal, an optimizing compiler should
* take care of this.
*
* The source for the insn timings is Markt & Technik's Amiga Magazin 8/1992.
*
* Copyright 1995, 1996, 1997, 1998, 1999, 2000 Bernd Schmidt
*/

#include "sysconfig.h"
#include "sysdeps.h"
#include <ctype.h>

#include "readcpu.h"

#define BOOL_TYPE "int"
/* Define the minimal 680x0 where NV flags are not affected by xBCD instructions.  */
#define xBCD_KEEPS_N_FLAG 4
#define xBCD_KEEPS_V_FLAG 2

static FILE *headerfile;
static FILE *stblfile;

static int using_prefetch, using_indirect;
static int using_exception_3;
static int using_simple_cycles;
static int cpu_level, cpu_generic;
static int count_read, count_write, count_cycles, count_ncycles;
static int count_read_ea, count_write_ea, count_cycles_ea;
static int did_prefetch;

static int optimized_flags;

#define GF_APDI 1
#define GF_AD8R 2
#define GF_PC8R 4
#define GF_AA 7
#define GF_NOREFILL 8
#define GF_PREFETCH 16
#define GF_FC 32
#define GF_MOVE 64
#define GF_IR2IRC 128
#define GF_LRMW 256
#define GF_NOFAULTPC 512
#define GF_RMW 1024
#define GF_OPCE020 2048
#define GF_REVERSE 4096
#define GF_REVERSE2 8192

/* For the current opcode, the next lower level that will have different code.
* Initialized to -1 for each opcode. If it remains unchanged, indicates we
* are done with that opcode.  */
static int next_cpu_level;

static int *opcode_map;
static int *opcode_next_clev;
static int *opcode_last_postfix;
static unsigned long *counts;
static int generate_stbl;
static int disp020cnt;

#define GENA_GETV_NO_FETCH	0
#define GENA_GETV_FETCH		1
#define GENA_GETV_FETCH_ALIGN	2
#define GENA_MOVEM_DO_INC	0
#define GENA_MOVEM_NO_INC	1
#define GENA_MOVEM_MOVE16	2

static const char *srcl, *dstl;
static const char *srcw, *dstw;
static const char *srcb, *dstb;
static const char *srcblrmw, *srcwlrmw, *srcllrmw;
static const char *dstblrmw, *dstwlrmw, *dstllrmw;
static const char *prefetch_long, *prefetch_word, *prefetch_opcode;
static const char *srcli, *srcwi, *srcbi, *nextl, *nextw;
static const char *srcld, *dstld;
static const char *srcwd, *dstwd;
static const char *do_cycles, *disp000, *disp020, *getpc;

#define fetchmode_fea 1
#define fetchmode_cea 2
#define fetchmode_fiea 3
#define fetchmode_ciea 4
#define fetchmode_jea 5

NORETURN static void term (void)
{
	printf("Abort!\n");
	abort ();
}
NORETURN static void term (const char *err)
{
	printf ("%s\n", err);
	term ();
}

static void read_counts (void)
{
	FILE *file;
	unsigned int opcode, count, total;
	char name[20];
	int nr = 0;
	memset (counts, 0, 65536 * sizeof *counts);

	count = 0;
	file = fopen ("frequent.68k", "r");
	if (file) {
		if (fscanf (file, "Total: %u\n", &total) == 0) {
			abort ();
		}
		while (fscanf (file, "%x: %u %s\n", &opcode, &count, name) == 3) {
			opcode_next_clev[nr] = 5;
			opcode_last_postfix[nr] = -1;
			opcode_map[nr++] = opcode;
			counts[opcode] = count;
		}
		fclose (file);
	}
	if (nr == nr_cpuop_funcs)
		return;
	for (opcode = 0; opcode < 0x10000; opcode++) {
		if (table68k[opcode].handler == -1 && table68k[opcode].mnemo != i_ILLG
			&& counts[opcode] == 0)
		{
			opcode_next_clev[nr] = 5;
			opcode_last_postfix[nr] = -1;
			opcode_map[nr++] = opcode;
			counts[opcode] = count;
		}
	}
	if (nr != nr_cpuop_funcs)
		term ();
}

static char endlabelstr[80];
static int endlabelno = 0;
static int need_endlabel;
static int genamode_cnt, genamode8r_offset[2];

static int n_braces, limit_braces;
static int m68k_pc_offset, m68k_pc_offset_old;
static int m68k_pc_total;
static int branch_inst;
static int insn_n_cycles;
static int ir2irc;

static void fpulimit (void)
{
	if (limit_braces)
		return;
	printf ("\n#ifdef FPUEMU\n");
	limit_braces = n_braces;
	n_braces = 0;
}

static int s_count_read, s_count_write, s_count_cycles, s_count_ncycles;

static void push_ins_cnt(void)
{
	s_count_read = count_read;
	s_count_write = count_write;
	s_count_cycles = count_cycles;
	s_count_ncycles = count_ncycles;
}
static void pop_ins_cnt(void)
{
	count_read = s_count_read;
	count_write = s_count_write;
	count_cycles = s_count_cycles;
	count_ncycles = s_count_ncycles;
}

static void returncycles (const char *s, int cycles)
{
  if (using_simple_cycles)
		printf ("%sreturn %d * CYCLE_UNIT / 2 + count_cycles;\n", s, cycles);
	else
		printf ("%sreturn %d * CYCLE_UNIT / 2;\n", s, cycles);
}

static void returncycles_exception (char *s, int cycles)
{
	printf ("\t\t%sreturn %d * CYCLE_UNIT / 2;\n", s, cycles);
}

static void addcycles000_nonces(const char *s, const char *sc)
{
	if (using_simple_cycles) {
		printf("%scount_cycles += (%s) * CYCLE_UNIT / 2;\n", s, sc);
		count_ncycles++;
	}
}
static void addcycles000_nonce(const char *s, int c)
{
	if (using_simple_cycles) {
		printf("%scount_cycles += %d * CYCLE_UNIT / 2;\n", s, c);
		count_ncycles++;
	}
}

static void addcycles000 (int cycles)
{
	count_cycles += cycles;
}
static void addcycles000_2 (const char *s, int cycles)
{
	count_cycles += cycles;
}

static void addcycles000_3 (const char *s)
{
	count_ncycles++;
}

static int isreg (amodes mode)
{
	if (mode == Dreg || mode == Areg)
		return 1;
	return 0;
}

static void start_brace (void)
{
	n_braces++;
	printf ("{");
}

static void close_brace (void)
{
	assert (n_braces > 0);
	n_braces--;
	printf ("}");
}

static void finish_braces (void)
{
	while (n_braces > 0)
		close_brace ();
}

static void pop_braces (int to)
{
	while (n_braces > to)
		close_brace ();
}

static int bit_size (int size)
{
	switch (size) {
	case sz_byte: return 8;
	case sz_word: return 16;
	case sz_long: return 32;
	default: term ();
	}
	return 0;
}

static const char *bit_mask (int size)
{
	switch (size) {
	case sz_byte: return "0xff";
	case sz_word: return "0xffff";
	case sz_long: return "0xffffffff";
	default: term ();
	}
	return 0;
}

static void gen_nextilong2 (const char *type, const char *name, int flags, int movem)
{
	int r = m68k_pc_offset;
	m68k_pc_offset += 4;

	printf ("\t%s %s;\n", type, name);
	if (using_prefetch) {
		if (flags & GF_NOREFILL) {
			printf ("\t%s = %s (%d) << 16;\n", name, prefetch_word, r + 2);
			count_read++;
			printf ("\t%s |= regs.irc;\n", name);
			insn_n_cycles += 4;
		} else {
			printf ("\t%s = %s (%d) << 16;\n", name, prefetch_word, r + 2);
			count_read += 2;
			printf ("\t%s |= %s (%d);\n", name, prefetch_word, r + 4);
			insn_n_cycles += 8;
		}
	} else {
		count_read += 2;
		insn_n_cycles += 8;
		printf ("\t%s = %s (%d);\n", name, prefetch_long, r);
	}
}
static void gen_nextilong (const char *type, const char *name, int flags)
{
	gen_nextilong2 (type, name, flags, 0);
}

static const char *gen_nextiword (int flags)
{
	static char buffer[80];
	int r = m68k_pc_offset;
	m68k_pc_offset += 2;

	if (using_prefetch) {
		if (flags & GF_NOREFILL) {
			strcpy (buffer, "regs.irc");
		} else {
			sprintf (buffer, "%s (%d)", prefetch_word, r + 2);
			count_read++;
			insn_n_cycles += 4;
		}
	} else {
		sprintf (buffer, "%s (%d)", prefetch_word, r);
		count_read++;
		insn_n_cycles += 4;
	}
	return buffer;
}

static const char *gen_nextibyte (int flags)
{
	static char buffer[80];
	int r = m68k_pc_offset;
	m68k_pc_offset += 2;

	insn_n_cycles += 4;
	if (using_prefetch) {
		if (flags & GF_NOREFILL) {
			strcpy (buffer, "(uae_u8)regs.irc");
		} else {
			sprintf (buffer, "(uae_u8)%s (%d)", prefetch_word, r + 2);
			insn_n_cycles += 4;
			count_read++;
		}
	} else {
		sprintf (buffer, "%s (%d)", srcbi, r);
		insn_n_cycles += 4;
		count_read++;
	}
	return buffer;
}

static void makefromsr (void)
{
	printf ("\tMakeFromSR();\n");
}

static void makefromsr_t0(void)
{
	printf ("\tMakeFromSR_T0();\n");
}

static void irc2ir (bool dozero)
{
	if (!using_prefetch)
		return;
	if (ir2irc)
		return;
	ir2irc = 1;
	printf ("\tregs.ir = regs.irc;\n");
	if (dozero)
		printf ("\tregs.irc = 0;\n");
}
static void irc2ir (void)
{
	irc2ir (false);
}

static void fill_prefetch_2 (void)
{
	if (!using_prefetch)
		return;
	printf ("\t%s (%d);\n", prefetch_word, m68k_pc_offset + 2);
	did_prefetch = 1;
	ir2irc = 0;
	count_read++;
	insn_n_cycles += 4;
}

static void fill_prefetch_1 (int o)
{
	if (using_prefetch) {
		printf ("\t%s (%d);\n", prefetch_word, o);
		did_prefetch = 1;
		ir2irc = 0;
		count_read++;
		insn_n_cycles += 4;
	}
}

static void fill_prefetch_full_2 (void)
{
	if (using_prefetch) {
		fill_prefetch_1 (0);
		irc2ir ();
		fill_prefetch_1 (2);
	}
}

// don't check trace bits
static void fill_prefetch_full_ntx (void)
{
	if (using_prefetch) {
		fill_prefetch_1 (0);
		irc2ir ();
		fill_prefetch_1 (2);
	} else {
    // Count cycles for 2nd prefetch
		count_read++;
		insn_n_cycles += 4;
	}
}
// check trace bits
static void fill_prefetch_full (void)
{
	if (using_prefetch) {
		fill_prefetch_1 (0);
		irc2ir ();
		fill_prefetch_1 (2);
	} else {
    if (cpu_level >= 2) {
  		printf("\tif(regs.t0) check_t0_trace();\n");
    }
    // Count cycles for 2nd prefetch
		count_read++;
		insn_n_cycles += 4;
	}
}

// 68000 and 68010 only
static void fill_prefetch_full_000 (void)
{
	if (!using_prefetch)
		return;
	fill_prefetch_full ();
}

static void fill_prefetch_0 (void)
{
	if (!using_prefetch)
		return;
	printf ("\t%s (0);\n", prefetch_word);
	did_prefetch = 1;
	ir2irc = 0;
	count_read++;
	insn_n_cycles += 4;
}

static void fill_prefetch_next_1 (void)
{
	irc2ir ();
	fill_prefetch_1 (m68k_pc_offset + 2);
}

static void fill_prefetch_next (void)
{
	if (using_prefetch) {
		fill_prefetch_next_1 ();
	}
}

static void fill_prefetch_finish (void)
{
	if (did_prefetch)
		return;
	if (using_prefetch) {
		fill_prefetch_1 (m68k_pc_offset);
	}
}

static void setpc (const char *format, ...)
{
	va_list parms;
	char buffer[1000];

	va_start (parms, format);
	_vsnprintf (buffer, 1000 - 1, format, parms);
	va_end (parms);

	if (using_prefetch)
		printf ("\tm68k_setpci_j(%s);\n", buffer);
	else
		printf ("\tm68k_setpc_j(%s);\n", buffer);
}

static void incpc (const char *format, ...)
{
	va_list parms;
	char buffer[1000];

	va_start (parms, format);
	_vsnprintf (buffer, 1000 - 1, format, parms);
	va_end (parms);

	if (using_prefetch)
		printf ("\tm68k_incpci (%s);\n", buffer);
	else
		printf ("\tm68k_incpc (%s);\n", buffer);
}

static void sync_m68k_pc (void)
{
	m68k_pc_offset_old = m68k_pc_offset;
	if (m68k_pc_offset == 0)
		return;
	incpc ("%d", m68k_pc_offset);
	m68k_pc_total += m68k_pc_offset;
	m68k_pc_offset = 0;
}

static void clear_m68k_offset(void)
{
	m68k_pc_total += m68k_pc_offset;
	m68k_pc_offset = 0;
}

static void sync_m68k_pc_noreset (void)
{
	sync_m68k_pc ();
	m68k_pc_offset = m68k_pc_offset_old;
}

static void next_level_000 (void)
{
	if (next_cpu_level < 0)
		next_cpu_level = 0;
}

/* getv == 1: fetch data; getv != 0: check for odd address. If movem != 0,
* the calling routine handles Apdi and Aipi modes.
* gb-- movem == 2 means the same thing but for a MOVE16 instruction */

/* fixup indicates if we want to fix up address registers in pre decrement
* or post increment mode now (0) or later (1). A value of 2 will then be
* used to do the actual fix up. This allows to do all memory readings
* before any register is modified, and so to rerun operation without
* side effect in case a bus fault is generated by any memory access.
* XJ - 2006/11/13 */

static void genamode2x (amodes mode, const char *reg, wordsizes size, const char *name, int getv, int movem, int flags, int fetchmode)
{
	char namea[100];
	bool rmw = false;
	int pc_68000_offset = m68k_pc_offset;
	int pc_68000_offset_fetch = 0;
	int pc_68000_offset_store = 0;

	sprintf (namea, "%sa", name);

	if (mode == Ad8r || mode == PC8r) {
		genamode8r_offset[genamode_cnt] = m68k_pc_total + m68k_pc_offset;
		genamode_cnt++;
	}

	start_brace ();

	switch (mode) {
	case Dreg:
		if (movem)
			term ();
		if (getv == 1)
			switch (size) {
			case sz_byte:
#ifdef USE_DUBIOUS_BIGENDIAN_OPTIMIZATION
				/* This causes the target compiler to generate better code on few systems */
				printf ("\tuae_s8 %s = ((uae_u8*)&m68k_dreg (regs, %s))[3];\n", name, reg);
#else
				printf ("\tuae_s8 %s = m68k_dreg (regs, %s);\n", name, reg);
#endif
				break;
			case sz_word:
#ifdef USE_DUBIOUS_BIGENDIAN_OPTIMIZATION
				printf ("\tuae_s16 %s = ((uae_s16*)&m68k_dreg (regs, %s))[1];\n", name, reg);
#else
				printf ("\tuae_s16 %s = m68k_dreg (regs, %s);\n", name, reg);
#endif
				break;
			case sz_long:
				printf ("\tuae_s32 %s = m68k_dreg (regs, %s);\n", name, reg);
				break;
			default:
				term ();
		}
		return;
	case Areg:
		if (movem)
			term ();
		if (getv == 1)
			switch (size) {
			case sz_word:
				printf ("\tuae_s16 %s = m68k_areg (regs, %s);\n", name, reg);
				break;
			case sz_long:
				printf ("\tuae_s32 %s = m68k_areg (regs, %s);\n", name, reg);
				break;
			default:
				term ();
		}
		return;
	case Aind: // (An)
		printf ("\tuaecptr %sa;\n", name);
		printf ("\t%sa = m68k_areg (regs, %s);\n", name, reg);
		break;
	case Aipi: // (An)+
		printf ("\tuaecptr %sa;\n", name);
		printf ("\t%sa = m68k_areg (regs, %s);\n", name, reg);
		break;
	case Apdi: // -(An)
		printf ("\tuaecptr %sa;\n", name);
		switch (size) {
		case sz_byte:
			if (movem)
				printf ("\t%sa = m68k_areg (regs, %s);\n", name, reg);
			else
				printf ("\t%sa = m68k_areg (regs, %s) - areg_byteinc[%s];\n", name, reg, reg);
			break;
		case sz_word:
			printf ("\t%sa = m68k_areg (regs, %s) - %d;\n", name, reg, movem ? 0 : 2);
			break;
		case sz_long:
			printf ("\t%sa = m68k_areg (regs, %s) - %d;\n", name, reg, movem ? 0 : 4);
			break;
		default:
			term ();
		}
		if (!(flags & GF_APDI)) {
			addcycles000 (2);
			insn_n_cycles += 2;
			count_cycles_ea += 2;
			pc_68000_offset_fetch += 2;
		}
		break;
	case Ad16: // (d16,An)
		printf ("\tuaecptr %sa;\n", name);
		printf ("\t%sa = m68k_areg (regs, %s) + (uae_s32)(uae_s16)%s;\n", name, reg, gen_nextiword (flags));
		count_read_ea++; 
		break;
	case PC16: // (d16,PC,Xn)
		printf ("\tuaecptr %sa;\n", name);
		printf ("\t%sa = %s + %d;\n", name, getpc, m68k_pc_offset);
		printf ("\t%sa += (uae_s32)(uae_s16)%s;\n", name, gen_nextiword (flags));
		break;
	case Ad8r: // (d8,An,Xn)
		printf ("\tuaecptr %sa;\n", name);
		if (cpu_level > 1) {
			if (next_cpu_level < 1)
				next_cpu_level = 1;
			sync_m68k_pc ();
			start_brace ();
			/* This would ordinarily be done in gen_nextiword, which we bypass.  */
			insn_n_cycles += 4;
			printf ("\t%sa = %s (m68k_areg (regs, %s), %d);\n", name, disp020, reg, disp020cnt++);
		} else {
			if (!(flags & GF_AD8R)) {
				addcycles000 (2);
				insn_n_cycles += 2;
				count_cycles_ea += 2;
			}
			if ((flags & GF_NOREFILL) && using_prefetch) {
				printf ("\t%sa = %s (m68k_areg (regs, %s), regs.irc);\n", name, disp000, reg);
			} else {
				printf ("\t%sa = %s (m68k_areg (regs, %s), %s);\n", name, disp000, reg, gen_nextiword (flags));
			}
			count_read_ea++; 
		}
		break;
	case PC8r: // (d8,PC,Xn)
		printf ("\tuaecptr tmppc;\n");
		printf ("\tuaecptr %sa;\n", name);
		if (cpu_level > 1) {
			if (next_cpu_level < 1)
				next_cpu_level = 1;
			sync_m68k_pc ();
			start_brace ();
			/* This would ordinarily be done in gen_nextiword, which we bypass.  */
			insn_n_cycles += 4;
			printf ("\ttmppc = %s;\n", getpc);
			printf ("\t%sa = %s (tmppc, %d);\n", name, disp020, disp020cnt++);
		} else {
			printf ("\ttmppc = %s + %d;\n", getpc, m68k_pc_offset);
			if (!(flags & GF_PC8R)) {
				addcycles000 (2);
				insn_n_cycles += 2;
				count_cycles_ea += 2;
			}
			if ((flags & GF_NOREFILL) && using_prefetch) {
				printf ("\t%sa = %s (tmppc, regs.irc);\n", name, disp000);
			} else {
				printf ("\t%sa = %s (tmppc, %s);\n", name, disp000, gen_nextiword (flags));
			}
		}

		break;
	case absw:
		printf ("\tuaecptr %sa;\n", name);
		printf ("\t%sa = (uae_s32)(uae_s16)%s;\n", name, gen_nextiword (flags));
		pc_68000_offset_fetch += 2;
		break;
	case absl:
		gen_nextilong2 ("uaecptr", namea, flags, movem);
		count_read_ea += 2;
		pc_68000_offset_fetch += 4;
		pc_68000_offset_store += 2;
		break;
	case imm:
		// fetch immediate address
		if (getv != 1)
			term ();
		switch (size) {
		case sz_byte:
			printf ("\tuae_s8 %s = %s;\n", name, gen_nextibyte (flags));
			count_read_ea++;
			break;
		case sz_word:
			printf ("\tuae_s16 %s = %s;\n", name, gen_nextiword (flags));
			count_read_ea++;
			break;
		case sz_long:
			gen_nextilong ("uae_s32", name, flags);
			count_read_ea += 2;
			break;
		default:
			term ();
		}
		return;
	case imm0:
		if (getv != 1)
			term ();
		printf ("\tuae_s8 %s = %s;\n", name, gen_nextibyte (flags));
		count_read_ea++;
		return;
	case imm1:
		if (getv != 1)
			term ();
		printf ("\tuae_s16 %s = %s;\n", name, gen_nextiword (flags));
		count_read_ea++;
		return;
	case imm2:
		if (getv != 1)
			term ();
		gen_nextilong ("uae_s32", name, flags);
		count_read_ea += 2;
		return;
	case immi:
		if (getv != 1)
			term ();
		printf ("\tuae_u32 %s = %s;\n", name, reg);
		return;
	default:
		term ();
	}

	/* We get here for all non-reg non-immediate addressing modes to
	* actually fetch the value. */

	int exception_pc_offset = 0;
	if (getv == 2) {
		// store
		if (pc_68000_offset) {
			exception_pc_offset = pc_68000_offset + pc_68000_offset_store + 2;
		}
	} else {
		// fetch
		exception_pc_offset = pc_68000_offset + pc_68000_offset_fetch;
	}

	if (using_prefetch && using_exception_3 && getv != 0 && size != sz_byte) {
		printf ("\tif (%sa & 1) {\n", name);
		if (exception_pc_offset)
			incpc("%d", exception_pc_offset);
		// MOVE.L EA,-(An) causing address error: stacked value is original An - 2, not An - 4.
		if ((flags & (GF_REVERSE | GF_REVERSE2)) && size == sz_long && mode == Apdi)
			printf("\t\t%sa += %d;\n", name, flags & GF_REVERSE2 ? -2 : 2);
		printf ("\t\texception3_%s(opcode, %sa);\n", getv == 2 ? "write" : "read", name);
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t}\n");
		start_brace ();
	}

	if (flags & GF_PREFETCH)
		fill_prefetch_next ();
	else if (flags & GF_IR2IRC)
		irc2ir (true);

	if (getv == 1) {
		start_brace ();
		if (using_prefetch) {
			switch (size) {
			case sz_byte: insn_n_cycles += 4; printf ("\tuae_s8 %s = %s (%sa);\n", name, srcb, name); count_read++; break;
			case sz_word: insn_n_cycles += 4; printf ("\tuae_s16 %s = %s (%sa);\n", name, srcw, name); count_read++; break;
			case sz_long: {
        insn_n_cycles += 8; 
				if ((flags & GF_REVERSE) && mode == Apdi)
					printf("\tuae_s32 %s = %s (%sa + 2); %s |= %s (%sa) << 16;\n", name, srcw, name, name, srcw, name);
				else
          printf ("\tuae_s32 %s = %s (%sa) << 16; %s |= %s (%sa + 2);\n", name, srcw, name, name, srcw, name); 
        count_read += 2; 
        break;
      }
			default: term ();
			}
		} else {
			switch (size) {
			case sz_byte: insn_n_cycles += 4; printf ("\tuae_s8 %s = %s (%sa);\n", name, srcb, name); count_read++; break;
			case sz_word: insn_n_cycles += 4; printf ("\tuae_s16 %s = %s (%sa);\n", name, srcw, name); count_read++; break;
			case sz_long: insn_n_cycles += 8; printf ("\tuae_s32 %s = %s (%sa);\n", name, srcl, name); count_read += 2; break;
			default: term ();
			}
		}
	}

	/* We now might have to fix up the register for pre-dec or post-inc
	* addressing modes. */
	if (!movem)
		switch (mode) {
		case Aipi:
			switch (size) {
			case sz_byte:
				printf ("\tm68k_areg (regs, %s) += areg_byteinc[%s];\n", reg, reg);
				break;
			case sz_word:
				printf ("\tm68k_areg (regs, %s) += 2;\n", reg);
				break;
			case sz_long:
				printf ("\tm68k_areg (regs, %s) += 4;\n", reg);
				break;
			default:
				term ();
			}
			break;
		case Apdi:
			printf ("\tm68k_areg (regs, %s) = %sa;\n", reg, name);
			break;
		default:
			break;
	}

	if (movem == 3) {
		close_brace ();
	}
}

static void genamode2 (amodes mode, const char *reg, wordsizes size, const char *name, int getv, int movem, int flags)
{
	genamode2x (mode, reg, size, name, getv, movem, flags, -1);
}

static void genamode (instr *curi, amodes mode, const char *reg, wordsizes size, const char *name, int getv, int movem, int flags)
{
	genamode2 (mode, reg, size, name, getv, movem, flags);
}

static void genamode3 (instr *curi, amodes mode, const char *reg, wordsizes size, const char *name, int getv, int movem, int flags)
{
	genamode2x (mode, reg, size, name, getv, movem, flags, curi ? curi->fetchmode : -1);
}

static void genamodedual (instr *curi, amodes smode, const char *sreg, wordsizes ssize, const char *sname, int sgetv, int sflags,
					   amodes dmode, const char *dreg, wordsizes dsize, const char *dname, int dgetv, int dflags)
{
	int subhead = 0;
	bool eadmode = false;

	genamode3 (curi, smode, sreg, ssize, sname, sgetv, 0, sflags);
	genamode3 (NULL, dmode, dreg, dsize, dname, dgetv, 0, dflags | (eadmode == true ? GF_OPCE020 : 0));
}

static void genastore_2 (const char *from, amodes mode, const char *reg, wordsizes size, const char *to, int store_dir, int flags)
{
	switch (mode) {
	case Dreg:
		switch (size) {
		case sz_byte:
			printf ("\tm68k_dreg (regs, %s) = (m68k_dreg (regs, %s) & ~0xff) | ((%s) & 0xff);\n", reg, reg, from);
			break;
		case sz_word:
			printf ("\tm68k_dreg (regs, %s) = (m68k_dreg (regs, %s) & ~0xffff) | ((%s) & 0xffff);\n", reg, reg, from);
			break;
		case sz_long:
			printf ("\tm68k_dreg (regs, %s) = (%s);\n", reg, from);
			break;
		default:
			term ();
		}
		break;
	case Areg:
		switch (size) {
		case sz_word:
			printf ("\tm68k_areg (regs, %s) = (uae_s32)(uae_s16)(%s);\n", reg, from);
			break;
		case sz_long:
			printf ("\tm68k_areg (regs, %s) = (%s);\n", reg, from);
			break;
		default:
			term ();
		}
		break;
	case Aind:
	case Aipi:
	case Apdi:
	case Ad16:
	case Ad8r:
	case absw:
	case absl:
	case PC16:
	case PC8r:
		if (using_prefetch) {
			switch (size) {
			case sz_byte:
				insn_n_cycles += 4;
				printf ("\t%s (%sa, %s);\n", dstb, to, from);
				count_write++;
				break;
			case sz_word:
				insn_n_cycles += 4;
				if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
					term ();
				printf ("\t%s (%sa, %s);\n", dstw, to, from);
				count_write++;
				break;
			case sz_long:
				insn_n_cycles += 8;
				if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
					term ();
				if (store_dir)
					printf ("\t%s (%sa + 2, %s); %s (%sa, %s >> 16);\n", dstw, to, from, dstw, to, from);
				else
					printf ("\t%s (%sa, %s >> 16); %s (%sa + 2, %s);\n", dstw, to, from, dstw, to, from);
				count_write += 2;
				break;
			default:
				term ();
			}
		} else {
			switch (size) {
			case sz_byte:
				insn_n_cycles += 4;
				printf ("\t%s (%sa, %s);\n", dstb, to, from);
				count_write++;
				break;
			case sz_word:
				insn_n_cycles += 4;
				if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
					term ();
				printf ("\t%s (%sa, %s);\n", dstw, to, from);
				count_write++;
				break;
			case sz_long:
				insn_n_cycles += 8;
				if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
					term ();
				printf ("\t%s (%sa, %s);\n", dstl, to, from);
				count_write += 2;
				break;
			default:
				term ();
			}
		}
		break;
	case imm:
	case imm0:
	case imm1:
	case imm2:
	case immi:
		term ();
		break;
	default:
		term ();
	}
}

static void genastore (const char *from, amodes mode, const char *reg, wordsizes size, const char *to)
{
	genastore_2 (from, mode, reg, size, to, 0, 0);
}
static void genastore_tas (const char *from, amodes mode, const char *reg, wordsizes size, const char *to)
{
	genastore_2 (from, mode, reg, size, to, 0, GF_LRMW);
}
static void genastore_cas (const char *from, amodes mode, const char *reg, wordsizes size, const char *to)
{
	genastore_2 (from, mode, reg, size, to, 0, GF_LRMW | GF_NOFAULTPC);
}
static void genastore_rev (const char *from, amodes mode, const char *reg, wordsizes size, const char *to)
{
	genastore_2 (from, mode, reg, size, to, 1, 0);
}
static void genastore_fc (const char *from, amodes mode, const char *reg, wordsizes size, const char *to)
{
	genastore_2 (from, mode, reg, size, to, 1, GF_FC);
}

static void genmovemel (uae_u16 opcode)
{
	char getcode[100];
	int size = table68k[opcode].size == sz_long ? 4 : 2;

	if (table68k[opcode].size == sz_long) {
		sprintf (getcode, "%s (srca)", srcld);
	} else {
		sprintf (getcode, "(uae_s32)(uae_s16)%s (srca)", srcwd);
	}
	count_read += table68k[opcode].size == sz_long ? 2 : 1;
	printf ("\tuae_u16 mask = %s;\n", gen_nextiword (0));
	printf ("\tuae_u32 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
	genamode (NULL, table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1, GF_MOVE);
	start_brace ();
	printf ("\twhile (dmask) {\n");
	printf ("\t\tm68k_dreg (regs, movem_index1[dmask]) = %s; srca += %d; dmask = movem_next[dmask];\n", getcode, size);
	printf ("\t}\n");
	printf ("\twhile (amask) {\n");
	printf ("\t\tm68k_areg (regs, movem_index1[amask]) = %s; srca += %d; amask = movem_next[amask];\n", getcode, size);
	printf ("\t}\n");
	if (table68k[opcode].dmode == Aipi) {
		printf ("\tm68k_areg (regs, dstreg) = srca;\n");
		count_read++;
	}
	count_ncycles++;
	fill_prefetch_next ();
}

static void genmovemel_ce (uae_u16 opcode)
{
	int size = table68k[opcode].size == sz_long ? 4 : 2;
	printf ("\tuae_u16 mask = %s;\n", gen_nextiword (0));
	printf ("\tuae_u32 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
	if (using_prefetch && table68k[opcode].size == sz_long)
		printf ("\tuae_u32 v;\n");
	genamode (NULL, table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1, GF_AA | GF_MOVE);
	if (table68k[opcode].dmode == Ad8r || table68k[opcode].dmode == PC8r)
		addcycles000 (2);
	start_brace ();
	if (table68k[opcode].size == sz_long) {
		printf ("\twhile (dmask) {\n");
		if (using_prefetch) {
		  printf ("\t\tv = %s (srca) << 16;\n", srcw);
		  printf ("\t\tv |= %s (srca + 2);\n", srcw);
  	  printf ("\t\tm68k_dreg (regs, movem_index1[dmask]) = v;\n");
		}	else
	    printf ("\t\tm68k_dreg (regs, movem_index1[dmask]) = %s (srca);\n", srcl);
		printf ("\t\tsrca += %d;\n", size);
		printf ("\t\tdmask = movem_next[dmask];\n");
		addcycles000_nonce("\t\t", cpu_level > 1 ? 4 : 8);
		printf ("\t}\n");
		printf ("\twhile (amask) {\n");
		if (using_prefetch) {
		  printf ("\t\tv = %s (srca) << 16;\n", srcw);
		  printf ("\t\tv |= %s (srca + 2);\n", srcw);
		  printf ("\t\tm68k_areg (regs, movem_index1[amask]) = v;\n");
		} else 
	    printf ("\t\tm68k_areg (regs, movem_index1[amask]) = %s (srca);\n", srcl);
		printf ("\t\tsrca += %d;\n", size);
		printf ("\t\tamask = movem_next[amask];\n");
		addcycles000_nonce("\t\t", cpu_level > 1 ? 4 : 8);
		printf ("\t}\n");
	} else {
		printf ("\twhile (dmask) {\n");
		printf ("\t\tm68k_dreg (regs, movem_index1[dmask]) = (uae_s32)(uae_s16)%s (srca);\n", srcw);
		printf ("\t\tsrca += %d;\n", size);
		printf ("\t\tdmask = movem_next[dmask];\n");
		addcycles000_nonce("\t\t", 4);
		printf ("\t}\n");
		printf ("\twhile (amask) {\n");
		printf ("\t\tm68k_areg (regs, movem_index1[amask]) = (uae_s32)(uae_s16)%s (srca);\n", srcw);
		printf ("\t\tsrca += %d;\n", size);
		printf ("\t\tamask = movem_next[amask];\n");
		addcycles000_nonce("\t\t", 4);
		printf ("\t}\n");
	}
	printf ("\t%s (srca);\n", srcw); // and final extra word fetch that goes nowhere..
	count_read++;
	if (table68k[opcode].dmode == Aipi)
		printf ("\tm68k_areg (regs, dstreg) = srca;\n");
	count_ncycles++;
	fill_prefetch_next ();
}

static void genmovemle (uae_u16 opcode)
{
	char putcode[100];
	int size = table68k[opcode].size == sz_long ? 4 : 2;

	if (table68k[opcode].size == sz_long) {
		sprintf (putcode, "%s (srca", dstld);
	} else {
		sprintf (putcode, "%s (srca", dstwd);
	}
	count_write += table68k[opcode].size == sz_long ? 2 : 1;

	printf ("\tuae_u16 mask = %s;\n", gen_nextiword (0));
	genamode (NULL, table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1, GF_MOVE);
	start_brace ();
	if (table68k[opcode].dmode == Apdi) {
		printf ("\tuae_u16 amask = mask & 0xff, dmask = (mask >> 8) & 0xff;\n");
		printf ("\tint type = get_cpu_model () >= 68020;\n");
		printf ("\twhile (amask) {\n");
		printf ("\t\tsrca -= %d;\n", size);

		printf ("\t\tif (!type || movem_index2[amask] != dstreg)\n");
		printf ("\t\t\t%s, m68k_areg (regs, movem_index2[amask]));\n", putcode);
		printf ("\t\telse\n");
		printf ("\t\t\t%s, m68k_areg (regs, movem_index2[amask]) - %d);\n", putcode, size);

		printf ("\t\tamask = movem_next[amask];\n");
		printf ("\t}\n");
		printf ("\twhile (dmask) { srca -= %d; %s, m68k_dreg (regs, movem_index2[dmask])); dmask = movem_next[dmask]; }\n",
			size, putcode);
		printf ("\tm68k_areg (regs, dstreg) = srca;\n");
	} else {
		printf ("\tuae_u16 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
		printf ("\twhile (dmask) { %s, m68k_dreg (regs, movem_index1[dmask])); srca += %d; dmask = movem_next[dmask]; }\n",
			putcode, size);
		printf ("\twhile (amask) { %s, m68k_areg (regs, movem_index1[amask])); srca += %d; amask = movem_next[amask]; }\n",
			putcode, size);
	}
	count_ncycles++;
	fill_prefetch_next ();
}

static void genmovemle_ce (uae_u16 opcode)
{
	int size = table68k[opcode].size == sz_long ? 4 : 2;

	printf ("\tuae_u16 mask = %s;\n", gen_nextiword (0));
	genamode (NULL, table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1, GF_AA | GF_MOVE | GF_REVERSE | GF_REVERSE2);
	if (table68k[opcode].dmode == Ad8r || table68k[opcode].dmode == PC8r)
		addcycles000 (2);
	start_brace ();
	if (table68k[opcode].size == sz_long) {
		if (table68k[opcode].dmode == Apdi) {
			printf ("\tuae_u16 amask = mask & 0xff, dmask = (mask >> 8) & 0xff;\n");
			printf ("\twhile (amask) {\n");
			printf ("\t\tsrca -= %d;\n", size);
      if(cpu_level >= 2)
        printf ("\t\tm68k_areg (regs, dstreg) = srca;\n");
			if (using_prefetch) {
		    printf ("\t\t%s (srca, m68k_areg (regs, movem_index2[amask]) >> 16);\n", dstw);
		    printf ("\t\t%s (srca + 2, m68k_areg (regs, movem_index2[amask]));\n", dstw);
			} else
	      printf ("\t\t%s (srca, m68k_areg (regs, movem_index2[amask]));\n", dstl);
			printf ("\t\tamask = movem_next[amask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 8);
			printf ("\t}\n");
			printf ("\twhile (dmask) {\n");
			printf ("\t\tsrca -= %d;\n", size);
			if (using_prefetch) {
		    printf ("\t\t%s (srca, m68k_dreg (regs, movem_index2[dmask]) >> 16);\n", dstw);
		    printf ("\t\t%s (srca + 2, m68k_dreg (regs, movem_index2[dmask]));\n", dstw);
			} else
	      printf ("\t\t%s (srca, m68k_dreg (regs, movem_index2[dmask]));\n", dstl);
			printf ("\t\tdmask = movem_next[dmask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 8);
			printf ("\t}\n");
			printf ("\tm68k_areg (regs, dstreg) = srca;\n");
		} else {
			printf ("\tuae_u16 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
			printf ("\twhile (dmask) {\n");
			if (using_prefetch) {
		    printf ("\t\t%s (srca, m68k_dreg (regs, movem_index1[dmask]) >> 16);\n", dstw);
		    printf ("\t\t%s (srca + 2, m68k_dreg (regs, movem_index1[dmask]));\n", dstw);
			} else
	      printf ("\t\t%s (srca, m68k_dreg (regs, movem_index1[dmask]));\n", dstl);
			printf ("\t\tsrca += %d;\n", size);
			printf ("\t\tdmask = movem_next[dmask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 8);
			printf ("\t}\n");
			printf ("\twhile (amask) {\n");
			if (using_prefetch) {
		    printf ("\t\t%s (srca, m68k_areg (regs, movem_index1[amask]) >> 16);\n", dstw);
		    printf ("\t\t%s (srca + 2, m68k_areg (regs, movem_index1[amask]));\n", dstw);
			} else
	      printf ("\t\t%s (srca, m68k_areg (regs, movem_index1[amask]));\n", dstl);
			printf ("\t\tsrca += %d;\n", size);
			printf ("\t\tamask = movem_next[amask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 8);
			printf ("\t}\n");
		}
	} else {
		if (table68k[opcode].dmode == Apdi) {
			printf ("\tuae_u16 amask = mask & 0xff, dmask = (mask >> 8) & 0xff;\n");
      printf ("\twhile (amask) {\n");
			printf ("\t\tsrca -= %d;\n", size);
      if(cpu_level >= 2)
  			printf ("\t\tm68k_areg (regs, dstreg) = srca;\n");
      printf ("\t\t%s (srca, m68k_areg (regs, movem_index2[amask]));\n", dstw);
			printf ("\tamask = movem_next[amask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 4);
			printf ("\t}\n");
			printf ("\twhile (dmask) {\n");
			printf ("\t\tsrca -= %d;\n", size);
			printf ("\t\t%s (srca, m68k_dreg (regs, movem_index2[dmask]));\n", dstw);
			printf ("\t\tdmask = movem_next[dmask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 4);
			printf ("\t}\n");
			printf ("\tm68k_areg (regs, dstreg) = srca;\n");
		} else {
			printf ("\tuae_u16 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
			printf ("\twhile (dmask) {\n");
			printf ("\t\t%s (srca, m68k_dreg (regs, movem_index1[dmask]));\n", dstw);
			printf ("\t\tsrca += %d;\n", size);
			printf ("\t\tdmask = movem_next[dmask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 4);
			printf ("\t}\n");
			printf ("\twhile (amask) {\n");
			printf ("\t\t%s (srca, m68k_areg (regs, movem_index1[amask]));\n", dstw);
			printf ("\t\tsrca += %d;\n", size);
			printf ("\t\tamask = movem_next[amask];\n");
			addcycles000_nonce("\t\t", cpu_level > 1 ? 3 : 4);
			printf ("\t}\n");
		}
	}
	count_ncycles++;
	fill_prefetch_next ();
}

static void duplicate_carry (int n)
{
	int i;
	for (i = 0; i <= n; i++)
		printf ("\t");
	printf ("COPY_CARRY ();\n");
}

typedef enum
{
	flag_logical_noclobber, flag_logical, flag_add, flag_sub, flag_cmp, flag_addx, flag_subx, flag_z, flag_zn,
	flag_av, flag_sv
}
flagtypes;

static void genflags_normal (flagtypes type, wordsizes size, const char *value, const char *src, const char *dst)
{
	char vstr[100], sstr[100], dstr[100];
	char usstr[100], udstr[100];
	char unsstr[100], undstr[100];

	switch (size) {
	case sz_byte:
		strcpy (vstr, "((uae_s8)(");
		strcpy (usstr, "((uae_u8)(");
		break;
	case sz_word:
		strcpy (vstr, "((uae_s16)(");
		strcpy (usstr, "((uae_u16)(");
		break;
	case sz_long:
		strcpy (vstr, "((uae_s32)(");
		strcpy (usstr, "((uae_u32)(");
		break;
	default:
		term ();
	}
	strcpy (unsstr, usstr);

	strcpy (sstr, vstr);
	strcpy (dstr, vstr);
	strcat (vstr, value);
	strcat (vstr, "))");
	strcat (dstr, dst);
	strcat (dstr, "))");
	strcat (sstr, src);
	strcat (sstr, "))");

	strcpy (udstr, usstr);
	strcat (udstr, dst);
	strcat (udstr, "))");
	strcat (usstr, src);
	strcat (usstr, "))");

	strcpy (undstr, unsstr);
	strcat (unsstr, "-");
	strcat (undstr, "~");
	strcat (undstr, dst);
	strcat (undstr, "))");
	strcat (unsstr, src);
	strcat (unsstr, "))");

	switch (type) {
	case flag_logical_noclobber:
	case flag_logical:
	case flag_z:
	case flag_zn:
	case flag_av:
	case flag_sv:
	case flag_addx:
	case flag_subx:
		break;

	case flag_add:
		start_brace ();
		printf ("uae_u32 %s = %s + %s;\n", value, udstr, usstr);
		break;
	case flag_sub:
	case flag_cmp:
		start_brace ();
		printf ("uae_u32 %s = %s - %s;\n", value, udstr, usstr);
		break;
	}

	switch (type) {
	case flag_logical_noclobber:
	case flag_logical:
	case flag_zn:
		break;

	case flag_add:
	case flag_sub:
	case flag_addx:
	case flag_subx:
	case flag_cmp:
	case flag_av:
	case flag_sv:
		start_brace ();
		printf ("\t" BOOL_TYPE " flgs = %s < 0;\n", sstr);
		printf ("\t" BOOL_TYPE " flgo = %s < 0;\n", dstr);
		printf ("\t" BOOL_TYPE " flgn = %s < 0;\n", vstr);
		break;
	}

	switch (type) {
	case flag_logical:
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tSET_ZFLG   (%s == 0);\n", vstr);
		printf ("\tSET_NFLG   (%s < 0);\n", vstr);
		break;
	case flag_logical_noclobber:
		printf ("\tSET_ZFLG (%s == 0);\n", vstr);
		printf ("\tSET_NFLG (%s < 0);\n", vstr);
		break;
	case flag_av:
		printf ("\tSET_VFLG ((flgs ^ flgn) & (flgo ^ flgn));\n");
		break;
	case flag_sv:
		printf ("\tSET_VFLG ((flgs ^ flgo) & (flgn ^ flgo));\n");
		break;
	case flag_z:
		printf ("\tSET_ZFLG (GET_ZFLG () & (%s == 0));\n", vstr);
		break;
	case flag_zn:
		printf ("\tSET_ZFLG (GET_ZFLG () & (%s == 0));\n", vstr);
		printf ("\tSET_NFLG (%s < 0);\n", vstr);
		break;
	case flag_add:
		printf ("\tSET_ZFLG (%s == 0);\n", vstr);
		printf ("\tSET_VFLG ((flgs ^ flgn) & (flgo ^ flgn));\n");
		printf ("\tSET_CFLG (%s < %s);\n", undstr, usstr);
		duplicate_carry (0);
		printf ("\tSET_NFLG (flgn != 0);\n");
		break;
	case flag_sub:
		printf ("\tSET_ZFLG (%s == 0);\n", vstr);
		printf ("\tSET_VFLG ((flgs ^ flgo) & (flgn ^ flgo));\n");
		printf ("\tSET_CFLG (%s > %s);\n", usstr, udstr);
		duplicate_carry (0);
		printf ("\tSET_NFLG (flgn != 0);\n");
		break;
	case flag_addx:
		printf ("\tSET_VFLG ((flgs ^ flgn) & (flgo ^ flgn));\n"); /* minterm SON: 0x42 */
		printf ("\tSET_CFLG (flgs ^ ((flgs ^ flgo) & (flgo ^ flgn)));\n"); /* minterm SON: 0xD4 */
		duplicate_carry (0);
		break;
	case flag_subx:
		printf ("\tSET_VFLG ((flgs ^ flgo) & (flgo ^ flgn));\n"); /* minterm SON: 0x24 */
		printf ("\tSET_CFLG (flgs ^ ((flgs ^ flgn) & (flgo ^ flgn)));\n"); /* minterm SON: 0xB2 */
		duplicate_carry (0);
		break;
	case flag_cmp:
		printf ("\tSET_ZFLG (%s == 0);\n", vstr);
		printf ("\tSET_VFLG ((flgs != flgo) && (flgn != flgo));\n");
		printf ("\tSET_CFLG (%s > %s);\n", usstr, udstr);
		printf ("\tSET_NFLG (flgn != 0);\n");
		break;
	}
}

static void genflags (flagtypes type, wordsizes size, const char *value, const char *src, const char *dst)
{
	/* Temporarily deleted 68k/ARM flag optimizations.  I'd prefer to have
	them in the appropriate m68k.h files and use just one copy of this
	code here.  The API can be changed if necessary.  */
	if (optimized_flags) {
		switch (type) {
		case flag_add:
		case flag_sub:
			start_brace ();
			printf ("\tuae_u32 %s;\n", value);
			break;

		default:
			break;
		}

		/* At least some of those casts are fairly important! */
		switch (type) {
		case flag_logical_noclobber:
			printf ("\t{uae_u32 oldcznv = GET_CZNV & ~(FLAGVAL_Z | FLAGVAL_N);\n");
			if (strcmp (value, "0") == 0) {
				printf ("\tSET_CZNV (olcznv | FLAGVAL_Z);\n");
			} else {
				switch (size) {
				case sz_byte: printf ("\toptflag_testb (regs, (uae_s8)(%s));\n", value); break;
				case sz_word: printf ("\toptflag_testw (regs, (uae_s16)(%s));\n", value); break;
				case sz_long: printf ("\toptflag_testl (regs, (uae_s32)(%s));\n", value); break;
				}
				printf ("\tIOR_CZNV (oldcznv);\n");
			}
			printf ("\t}\n");
			return;
		case flag_logical:
			if (strcmp (value, "0") == 0) {
				printf ("\tSET_CZNV (FLAGVAL_Z);\n");
			} else {
				switch (size) {
				case sz_byte: printf ("\toptflag_testb (regs, (uae_s8)(%s));\n", value); break;
				case sz_word: printf ("\toptflag_testw (regs, (uae_s16)(%s));\n", value); break;
				case sz_long: printf ("\toptflag_testl (regs, (uae_s32)(%s));\n", value); break;
				}
			}
			return;

		case flag_add:
			switch (size) {
			case sz_byte: printf ("\toptflag_addb (regs, %s, (uae_s8)(%s), (uae_s8)(%s));\n", value, src, dst); break;
			case sz_word: printf ("\toptflag_addw (regs, %s, (uae_s16)(%s), (uae_s16)(%s));\n", value, src, dst); break;
			case sz_long: printf ("\toptflag_addl (regs, %s, (uae_s32)(%s), (uae_s32)(%s));\n", value, src, dst); break;
			}
			return;

		case flag_sub:
			switch (size) {
			case sz_byte: printf ("\toptflag_subb (regs, %s, (uae_s8)(%s), (uae_s8)(%s));\n", value, src, dst); break;
			case sz_word: printf ("\toptflag_subw (regs, %s, (uae_s16)(%s), (uae_s16)(%s));\n", value, src, dst); break;
			case sz_long: printf ("\toptflag_subl (regs, %s, (uae_s32)(%s), (uae_s32)(%s));\n", value, src, dst); break;
			}
			return;

		case flag_cmp:
			switch (size) {
			case sz_byte: printf ("\toptflag_cmpb (regs, (uae_s8)(%s), (uae_s8)(%s));\n", src, dst); break;
			case sz_word: printf ("\toptflag_cmpw (regs, (uae_s16)(%s), (uae_s16)(%s));\n", src, dst); break;
			case sz_long: printf ("\toptflag_cmpl (regs, (uae_s32)(%s), (uae_s32)(%s));\n", src, dst); break;
			}
			return;

		default:
			break;
		}
	}

	genflags_normal (type, size, value, src, dst);
}

static void force_range_for_rox (const char *var, wordsizes size)
{
	/* Could do a modulo operation here... which one is faster? */
	switch (size) {
	case sz_long:
		printf ("\tif (%s >= 33) %s -= 33;\n", var, var);
		break;
	case sz_word:
		printf ("\tif (%s >= 34) %s -= 34;\n", var, var);
		printf ("\tif (%s >= 17) %s -= 17;\n", var, var);
		break;
	case sz_byte:
		printf ("\tif (%s >= 36) %s -= 36;\n", var, var);
		printf ("\tif (%s >= 18) %s -= 18;\n", var, var);
		printf ("\tif (%s >= 9) %s -= 9;\n", var, var);
		break;
	}
}

static const char *cmask (wordsizes size)
{
	switch (size) {
	case sz_byte: return "0x80";
	case sz_word: return "0x8000";
	case sz_long: return "0x80000000";
	default: term ();
	}
}

static int source_is_imm1_8 (struct instr *i)
{
	return i->stype == 3;
}

static void shift_ce (amodes dmode, int size)
{
	if (isreg (dmode)) {
		int c = size == sz_long ? 4 : 2;
		addcycles000_nonces("\t", "2 * ccnt");
		count_cycles += c;
		count_ncycles++;
	}
}

// BCHG/BSET/BCLR Dx,Dx or #xx,Dx adds 2 cycles if bit number > 15 
static void bsetcycles (struct instr *curi)
{
	if (curi->size == sz_byte) {
		printf ("\tsrc &= 7;\n");
	} else {
		printf ("\tsrc &= 31;\n");
		if (isreg (curi->dmode)) {
			addcycles000 (2);
			if (curi->mnemo != i_BTST) {
				addcycles000_nonce("\tif (src > 15) ", 2);
				count_ncycles++;
			}
		}
	}
}

static int islongimm (struct instr *curi)
{
	return (curi->size == sz_long && (curi->smode == Dreg || curi->smode == imm || curi->smode == Areg));
}


static void resetvars (void)
{
	insn_n_cycles = using_prefetch ? 0 : 4;
	genamode_cnt = 0;
	genamode8r_offset[0] = genamode8r_offset[1] = 0;
	m68k_pc_total = 0;
	branch_inst = 0;

	ir2irc = 0;
	disp020cnt = 0;
	
	prefetch_long = NULL;
	prefetch_opcode = NULL;
	srcli = NULL;
	srcbi = NULL;
	disp000 = "get_disp_ea_000";
	disp020 = "get_disp_ea_020";
	nextw = NULL;
	nextl = NULL;
	do_cycles = "do_cycles";
	srcwd = srcld = NULL;
	dstwd = dstld = NULL;
	srcblrmw = NULL;
	srcwlrmw = NULL;
	srcllrmw = NULL;
	dstblrmw = NULL;
	dstwlrmw = NULL;
	dstllrmw = NULL;
	getpc = "m68k_getpc ()";

	if (using_prefetch) {
		// 68000 prefetch
		prefetch_word = "get_word_000_prefetch";
		prefetch_long = "get_long_000_prefetch";
		srcwi = "get_wordi_000";
		srcl = "get_long_000";
		dstl = "put_long_000";
		srcw = "get_word_000";
		dstw = "put_word_000";
		srcb = "get_byte_000";
		dstb = "put_byte_000";
		getpc = "m68k_getpci ()";
	} else {
		// generic + direct
		prefetch_long = "get_dilong";
		prefetch_word = "get_diword";
		nextw = "next_diword";
		nextl = "next_dilong";
		srcli = "get_dilong";
		srcwi = "get_diword";
		srcbi = "get_dibyte";
		if (using_indirect < 0) {
			srcl = "get_long_jit";
			dstl = "put_long_jit";
			srcw = "get_word_jit";
			dstw = "put_word_jit";
			srcb = "get_byte_jit";
			dstb = "put_byte_jit";
		} else {
		srcl = "get_long";
		dstl = "put_long";
		srcw = "get_word";
		dstw = "put_word";
		srcb = "get_byte";
		dstb = "put_byte";
	}
	}
	if (!dstld)
		dstld = dstl;
	if (!dstwd)
		dstwd = dstw;
	if (!srcld)
		srcld = srcl;
	if (!srcwd)
		srcwd = srcw;
	if (!srcblrmw) {
		srcblrmw = srcb;
		srcwlrmw = srcw;
		srcllrmw = srcl;
		dstblrmw = dstb;
		dstwlrmw = dstw;
		dstllrmw = dstl;
	}
	if (!prefetch_opcode)
		prefetch_opcode = prefetch_word;
}

static void gen_opcode (unsigned int opcode)
{
	struct instr *curi = table68k + opcode;

	resetvars ();
	
  if(curi->mnemo == i_DIVL)
     printf ("\tuae_u32 cyc = 0;\n");

	start_brace ();

	m68k_pc_offset = 2;

	// do not unnecessarily create useless mmuop030
	// functions when CPU is not 68030
	if (curi->mnemo == i_MMUOP030 && cpu_level != 3 && !cpu_generic) {
		printf("\top_illg (opcode);\n");
		did_prefetch = -1;
		goto end;
	}

	switch (curi->plev) {
	case 0: /* not privileged */
		break;
	case 1: /* unprivileged only on 68000 */
		if (cpu_level == 0)
			break;
		if (next_cpu_level < 0)
			next_cpu_level = 0;

		/* fall through */
	case 2: /* priviledged */
		printf ("if (!regs.s) { Exception (8); return 4 * CYCLE_UNIT / 2; }\n");
		start_brace ();
		break;
	case 3: /* privileged if size == word */
		if (curi->size == sz_byte)
			break;
		printf ("if (!regs.s) { Exception (8); return 4 * CYCLE_UNIT / 2; }\n");
		start_brace ();
		break;
	}
	switch (curi->mnemo) {
	case i_OR:
	case i_AND:
	case i_EOR:
	{
		// documentaion error: and.l #imm,dn = 2 idle, not 1 idle (same as OR and EOR)
		int c = 0;
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, GF_RMW);
//		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
//		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_RMW);
		printf ("\tsrc %c= dst;\n", curi->mnemo == i_OR ? '|' : curi->mnemo == i_AND ? '&' : '^');
		genflags (flag_logical, curi->size, "src", "", "");
		if (curi->dmode == Dreg && curi->size == sz_long) {
			c += 2;
			if (curi->smode == imm || curi->smode == Dreg)
				c += 2;
		}
		fill_prefetch_next ();
		if (c > 0)
			addcycles000 (c);
		genastore_rev ("src", curi->dmode, "dstreg", curi->size, "dst");
		break;
	}
	// all SR/CCR modifications does full prefetch
	case i_ORSR:
	case i_EORSR:
		printf ("\tMakeSR ();\n");
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		if (curi->size == sz_byte) {
			printf ("\tsrc &= 0xFF;\n");
		}
		addcycles000 (8);
		printf ("\tregs.sr %c= src;\n", curi->mnemo == i_EORSR ? '^' : '|');
		makefromsr_t0();
		sync_m68k_pc ();
		fill_prefetch_full_ntx();
		break;
	case i_ANDSR:
		printf ("\tMakeSR ();\n");
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		if (curi->size == sz_byte) {
			printf ("\tsrc |= 0xFF00;\n");
		}
		addcycles000 (8);
		printf ("\tregs.sr &= src;\n");
		makefromsr_t0();
		sync_m68k_pc ();
		fill_prefetch_full_ntx();
		break;
	case i_SUB:
	{
		int c = 0;
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_RMW);
		if (curi->dmode == Dreg) {
			if (curi->size == sz_long) {
				c += 2;
				if (curi->smode == imm || curi->smode == immi || curi->smode == Dreg || curi->smode == Areg)
					c += 2;
			}
		}
		fill_prefetch_next ();
		if (c > 0)
			addcycles000 (c);
		start_brace ();
		genflags (flag_sub, curi->size, "newv", "src", "dst");
		genastore_rev ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	}
	case i_SUBA:
	{
		int c = 0;
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", sz_long, "dst", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", sz_long, "dst", 1, 0, GF_RMW);
		if (curi->smode == immi) {
			// SUBAQ.x is always 8 cycles
			c += 4;
		} else {
			c = curi->size == sz_long ? 2 : 4;
			if (islongimm (curi))
				c += 2;
		}
		fill_prefetch_next ();
		if (c > 0)
			addcycles000 (c);
		start_brace ();
		printf ("\tuae_u32 newv = dst - src;\n");
		genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		break;
	}
	case i_SUBX:
		if (!isreg (curi->smode))
			addcycles000 (2);
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA | GF_REVERSE);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_AA | GF_REVERSE | GF_RMW);
		fill_prefetch_next ();
		if (curi->size == sz_long && isreg (curi->smode))
			addcycles000 (4);
		start_brace ();
		printf ("\tuae_u32 newv = dst - src - (GET_XFLG () ? 1 : 0);\n");
		genflags (flag_subx, curi->size, "newv", "src", "dst");
		genflags (flag_zn, curi->size, "newv", "", "");
		genastore ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_SBCD:
		if (!isreg (curi->smode))
			addcycles000 (2);
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_AA | GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tuae_u16 newv_lo = (dst & 0xF) - (src & 0xF) - (GET_XFLG () ? 1 : 0);\n");
		printf ("\tuae_u16 newv_hi = (dst & 0xF0) - (src & 0xF0);\n");
		printf ("\tuae_u16 newv, tmp_newv;\n");
		printf ("\tint bcd = 0;\n");
		printf ("\tnewv = tmp_newv = newv_hi + newv_lo;\n");
		printf ("\tif (newv_lo & 0xF0) { newv -= 6; bcd = 6; };\n");
		printf ("\tif ((((dst & 0xFF) - (src & 0xFF) - (GET_XFLG () ? 1 : 0)) & 0x100) > 0xFF) { newv -= 0x60; }\n");
		printf ("\tSET_CFLG ((((dst & 0xFF) - (src & 0xFF) - bcd - (GET_XFLG () ? 1 : 0)) & 0x300) > 0xFF);\n");
		duplicate_carry (0);
		/* Manual says bits NV are undefined though a real 68030 doesn't change V and 68040/060 don't change both */
		if (cpu_level >= xBCD_KEEPS_N_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_N_FLAG)
				next_cpu_level = xBCD_KEEPS_N_FLAG - 1;
			genflags (flag_z, curi->size, "newv", "", "");
		} else {
			genflags (flag_zn, curi->size, "newv", "", "");
		}
		if (cpu_level >= xBCD_KEEPS_V_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_V_FLAG)
				next_cpu_level = xBCD_KEEPS_V_FLAG - 1;
		} else {
			printf ("\tSET_VFLG ((tmp_newv & 0x80) != 0 && (newv & 0x80) == 0);\n");
		}
		if (isreg (curi->smode)) {
			addcycles000 (2);
		}
		genastore ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_ADD:
	{
		int c = 0;
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_RMW);
		if (curi->dmode == Dreg) {
			if (curi->size == sz_long) {
				c += 2;
				if (curi->smode == imm || curi->smode == immi || curi->smode == Dreg || curi->smode == Areg)
					c += 2;
			}
		}
		fill_prefetch_next ();
		if (c > 0)
			addcycles000 (c);
		start_brace ();
		genflags (flag_add, curi->size, "newv", "src", "dst");
		genastore_rev ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	}
	case i_ADDA:
	{
		int c = 0;
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", sz_long, "dst", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", sz_long, "dst", 1, 0, GF_RMW);
		if (curi->smode == immi) {
			// ADDAQ.x is always 8 cycles
			c += 4;
		} else {
			c = curi->size == sz_long ? 2 : 4;
			if (islongimm (curi))
				c += 2;
		}
		fill_prefetch_next ();
		if (c > 0)
			addcycles000 (c);
		start_brace ();
		printf ("\tuae_u32 newv = dst + src;\n");
		genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		break;
	}
	case i_ADDX:
		if (!isreg (curi->smode))
			addcycles000 (2);
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA | GF_REVERSE);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_AA | GF_REVERSE | GF_RMW);
		fill_prefetch_next ();
		if (curi->size == sz_long && isreg (curi->smode))
			addcycles000 (4);
		start_brace ();
		printf ("\tuae_u32 newv = dst + src + (GET_XFLG () ? 1 : 0);\n");
		genflags (flag_addx, curi->size, "newv", "src", "dst");
		genflags (flag_zn, curi->size, "newv", "", "");
		genastore ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_ABCD:
		if (!isreg (curi->smode))
			addcycles000 (2);
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_AA | GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tuae_u16 newv_lo = (src & 0xF) + (dst & 0xF) + (GET_XFLG () ? 1 : 0);\n");
		printf ("\tuae_u16 newv_hi = (src & 0xF0) + (dst & 0xF0);\n");
		printf ("\tuae_u16 newv, tmp_newv;\n");
		printf ("\tint cflg;\n");
		printf ("\tnewv = tmp_newv = newv_hi + newv_lo;\n");
		printf ("\tif (newv_lo > 9) { newv += 6; }\n");
		printf ("\tcflg = (newv & 0x3F0) > 0x90;\n");
		printf ("\tif (cflg) newv += 0x60;\n");
		printf ("\tSET_CFLG (cflg);\n");
		duplicate_carry (0);
		/* Manual says bits NV are undefined though a real 68030 doesn't change V and 68040/060 don't change both */
		if (cpu_level >= xBCD_KEEPS_N_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_N_FLAG)
				next_cpu_level = xBCD_KEEPS_N_FLAG - 1;
			genflags (flag_z, curi->size, "newv", "", "");
		} else {
			genflags (flag_zn, curi->size, "newv", "", "");
		}
		if (cpu_level >= xBCD_KEEPS_V_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_V_FLAG)
				next_cpu_level = xBCD_KEEPS_V_FLAG - 1;
		} else {
			printf ("\tSET_VFLG ((tmp_newv & 0x80) == 0 && (newv & 0x80) != 0);\n");
		}
		if (isreg (curi->smode)) {
			addcycles000 (2);
		}
		genastore ("newv", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_NEG:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_RMW);
		fill_prefetch_next ();
		if (isreg (curi->smode) && curi->size == sz_long)
			addcycles000 (2);
		start_brace ();
		genflags (flag_sub, curi->size, "dst", "src", "0");
		genastore_rev ("dst", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_NEGX:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_RMW);
		fill_prefetch_next ();
		if (isreg (curi->smode) && curi->size == sz_long)
			addcycles000 (2);
		start_brace ();
		printf ("\tuae_u32 newv = 0 - src - (GET_XFLG () ? 1 : 0);\n");
		genflags (flag_subx, curi->size, "newv", "src", "0");
		genflags (flag_zn, curi->size, "newv", "", "");
		genastore_rev ("newv", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_NBCD:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_RMW);
		if (isreg (curi->smode))
			addcycles000 (2);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tuae_u16 newv_lo = - (src & 0xF) - (GET_XFLG () ? 1 : 0);\n");
		printf ("\tuae_u16 newv_hi = - (src & 0xF0);\n");
		printf ("\tuae_u16 newv;\n");
		printf ("\tint cflg, tmp_newv;\n");
		printf ("\ttmp_newv = newv_hi + newv_lo;\n");
		printf ("\tif (newv_lo > 9) { newv_lo -= 6; }\n");
		printf ("\tnewv = newv_hi + newv_lo;\n");
		printf ("\tcflg = (newv & 0x1F0) > 0x90;\n");
		printf ("\tif (cflg) newv -= 0x60;\n");
		printf ("\tSET_CFLG (cflg);\n");
		duplicate_carry(0);
		/* Manual says bits NV are undefined though a real 68030 doesn't change V and 68040/060 don't change both */
		if (cpu_level >= xBCD_KEEPS_N_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_N_FLAG)
				next_cpu_level = xBCD_KEEPS_N_FLAG - 1;
			genflags (flag_z, curi->size, "newv", "", "");
		} else {
			genflags (flag_zn, curi->size, "newv", "", "");
		}
		if (cpu_level >= xBCD_KEEPS_V_FLAG) {
			if (next_cpu_level < xBCD_KEEPS_V_FLAG)
				next_cpu_level = xBCD_KEEPS_V_FLAG - 1;
		} else {
			printf ("\tSET_VFLG ((tmp_newv & 0x80) != 0 && (newv & 0x80) == 0);\n");
		}
		genastore ("newv", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_CLR:
		next_level_000 ();
		genamode (curi, curi->smode, "srcreg", curi->size, "src", cpu_level == 0 ? 1 : 2, 0, 0);
		fill_prefetch_next ();
    if(!using_prefetch && curi->smode != Dreg) {
      if(curi->size != sz_long)
        addcycles000(4);
      else
        addcycles000(8);
    }
		if (isreg (curi->smode) && curi->size == sz_long)
			addcycles000 (2);
		genflags (flag_logical, curi->size, "0", "", "");
		genastore_rev ("0", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_NOT:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_RMW);
		fill_prefetch_next ();
		if (isreg (curi->smode) && curi->size == sz_long)
			addcycles000 (2);
		start_brace ();
		printf ("\tuae_u32 dst = ~src;\n");
		genflags (flag_logical, curi->size, "dst", "", "");
		genastore_rev ("dst", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_TST:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		fill_prefetch_next ();
		genflags (flag_logical, curi->size, "src", "", "");
		break;
	case i_BTST:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, 0);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_IR2IRC);
		fill_prefetch_next ();
		bsetcycles (curi);
		printf ("\tSET_ZFLG (1 ^ ((dst >> src) & 1));\n");
		break;
	case i_BCHG:
	case i_BCLR:
	case i_BSET:
		// on 68000 these have weird side-effect, if EA points to write-only custom register
		//during instruction's read access CPU data lines appear as zero to outside world,
		// (normally previously fetched data appears in data lines if reading write-only register)
		// this allows stupid things like bset #2,$dff096 to work "correctly"
		// NOTE: above can't be right.
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_IR2IRC | GF_RMW);
		fill_prefetch_next ();
		bsetcycles (curi);
		// bclr needs 1 extra cycle
		if (curi->mnemo == i_BCLR && curi->dmode == Dreg)
			addcycles000 (2);
		if (curi->mnemo == i_BCHG) {
			printf ("\tdst ^= (1 << src);\n");
			printf ("\tSET_ZFLG (((uae_u32)dst & (1 << src)) >> src);\n");
		} else if (curi->mnemo == i_BCLR) {
			printf ("\tSET_ZFLG (1 ^ ((dst >> src) & 1));\n");
			printf ("\tdst &= ~(1 << src);\n");
		} else if (curi->mnemo == i_BSET) {
			printf ("\tSET_ZFLG (1 ^ ((dst >> src) & 1));\n");
			printf ("\tdst |= (1 << src);\n");
		}
		genastore ("dst", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_CMPM:
		// confirmed
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, GF_AA,
			curi->dmode, "dstreg", curi->size, "dst", 1, GF_AA);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_AA);
		fill_prefetch_next ();
		start_brace ();
		genflags (flag_cmp, curi->size, "newv", "src", "dst");
		break;
	case i_CMP:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, 0);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, 0);
		fill_prefetch_next ();
		if (curi->dmode == Dreg && curi->size == sz_long)
			addcycles000 (2);
		start_brace ();
		genflags (flag_cmp, curi->size, "newv", "src", "dst");
		break;
	case i_CMPA:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", sz_long, "dst", 1, 0);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", sz_long, "dst", 1, 0, 0);
		fill_prefetch_next ();
		addcycles000 (2);
		start_brace ();
		genflags (flag_cmp, sz_long, "newv", "src", "dst");
		break;
		/* The next two are coded a little unconventional, but they are doing
		* weird things... */
	case i_MVPRM: // MOVEP R->M
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		printf ("\tuaecptr memp = m68k_areg (regs, dstreg) + (uae_s32)(uae_s16)%s;\n", gen_nextiword (0));
		if (curi->size == sz_word) {
			printf ("\t%s (memp, src >> 8);\n\t%s (memp + 2, src);\n", dstb, dstb);
			count_write += 2;
		} else {
			printf ("\t%s (memp, src >> 24);\n\t%s (memp + 2, src >> 16);\n", dstb, dstb);
			printf ("\t%s (memp + 4, src >> 8);\n\t%s (memp + 6, src);\n", dstb, dstb);
			count_write += 4;
		}
		fill_prefetch_next ();
		break;
	case i_MVPMR: // MOVEP M->R
		printf ("\tuaecptr memp = m68k_areg (regs, srcreg) + (uae_s32)(uae_s16)%s;\n", gen_nextiword (0));
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 2, 0, 0);
		if (curi->size == sz_word) {
			printf ("\tuae_u16 val  = (%s (memp) & 0xff) << 8;\n", srcb);
			printf ("\t        val |= (%s (memp + 2) & 0xff);\n", srcb);
			count_read += 2;
		} else {
			printf ("\tuae_u32 val  = (%s (memp) & 0xff) << 24;\n", srcb);
			printf ("\t        val |= (%s (memp + 2) & 0xff) << 16;\n", srcb);
			printf ("\t        val |= (%s (memp + 4) & 0xff) << 8;\n", srcb);
			printf ("\t        val |= (%s (memp + 6) & 0xff);\n", srcb);
			count_read += 4;
		}
		fill_prefetch_next ();
		genastore ("val", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_MOVE:
	case i_MOVEA:
		{
			/* 2 MOVE instruction variants have special prefetch sequence:
			* - MOVE <ea>,-(An) = prefetch is before writes (Apdi)
			* - MOVE memory,(xxx).L = 2 prefetches after write
			* - move.x #imm = prefetch is done before write
			* - all others = prefetch is done after writes
			*
			* - move.x xxx,[at least 1 extension word here] = fetch 1 extension word before (xxx)
			*
			*/
			int prefetch_done = 0, flags;
			int dualprefetch = curi->dmode == absl && (curi->smode != Dreg && curi->smode != Areg && curi->smode != imm);

			genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
			flags = GF_MOVE | GF_APDI;
			//if (curi->size == sz_long && (curi->smode == Dreg || curi->smode == Areg))
			//	flags &= ~GF_APDI;
			flags |= dualprefetch ? GF_NOREFILL : 0;
			if (curi->dmode == Apdi && curi->size == sz_long)
				flags |= GF_REVERSE;
			genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 2, 0, flags);
			if (curi->mnemo == i_MOVEA && curi->size == sz_word)
				printf ("\tsrc = (uae_s32)(uae_s16)src;\n");
			if (curi->dmode == Apdi) {
				fill_prefetch_next ();
				prefetch_done = 1;
			}
			if (curi->mnemo == i_MOVE)
				genflags (flag_logical, curi->size, "src", "", "");

			// MOVE EA,-(An) long writes are always reversed. Reads are normal.
			if (curi->dmode == Apdi && curi->size == sz_long) {
				genastore_rev("src", curi->dmode, "dstreg", curi->size, "dst");
			} else {
			  genastore ("src", curi->dmode, "dstreg", curi->size, "dst");
			}
			sync_m68k_pc ();
			if (dualprefetch) {
				fill_prefetch_full_000 ();
				prefetch_done = 1;
			}
			if (!prefetch_done)
				fill_prefetch_next ();
		}
		break;
	case i_MVSR2: // MOVE FROM SR
		genamode (curi, curi->smode, "srcreg", sz_word, "src", 2, 0, 0);
		if (isreg (curi->smode)) {
			fill_prefetch_next ();
			addcycles000 (2);
		} else {
			// write to memory, dummy write to same address, X-flag seems to be always set
			if (cpu_level <= 1 && curi->size == sz_word) {
				printf ("\t%s (srca, regs.sr | 0x0010);\n", dstw);
				count_write++;
			}
			fill_prefetch_next ();
		}
		printf ("\tMakeSR ();\n");
		// real write
		if (curi->size == sz_byte)
			genastore ("regs.sr & 0xff", curi->smode, "srcreg", sz_word, "src");
		else
			genastore ("regs.sr", curi->smode, "srcreg", sz_word, "src");
		break;
	case i_MV2SR: // MOVE TO SR
		genamode (curi, curi->smode, "srcreg", sz_word, "src", 1, 0, 0);
		if (curi->size == sz_byte) {
			// MOVE TO CCR
			addcycles000 (4);
			printf ("\tMakeSR ();\n\tregs.sr &= 0xFF00;\n\tregs.sr |= src & 0xFF;\n");
		} else {
			// MOVE TO SR
			addcycles000 (4);
			printf ("\tregs.sr = src;\n");
		}
		makefromsr_t0();
		// does full prefetch because S-bit change may change memory mapping under the CPU
		sync_m68k_pc ();
		fill_prefetch_full_ntx();
		break;
	case i_SWAP:
		genamode (curi, curi->smode, "srcreg", sz_long, "src", 1, 0, 0);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tuae_u32 dst = ((src >> 16)&0xFFFF) | ((src&0xFFFF)<<16);\n");
		genflags (flag_logical, sz_long, "dst", "", "");
		genastore ("dst", curi->smode, "srcreg", sz_long, "src");
		break;
	case i_EXG:
		// confirmed
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, 0,
			curi->dmode, "dstreg", curi->size, "dst", 1, 0);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, 0);
		fill_prefetch_next ();
		addcycles000 (2);
		genastore ("dst", curi->smode, "srcreg", curi->size, "src");
		genastore ("src", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_EXT:
		// confirmed
		genamode (curi, curi->smode, "srcreg", sz_long, "src", 1, 0, 0);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 dst = (uae_s32)(uae_s8)src;\n"); break;
		case sz_word: printf ("\tuae_u16 dst = (uae_s16)(uae_s8)src;\n"); break;
		case sz_long: printf ("\tuae_u32 dst = (uae_s32)(uae_s16)src;\n"); break;
		default: term ();
		}
		genflags (flag_logical,
			curi->size == sz_word ? sz_word : sz_long, "dst", "", "");
		genastore ("dst", curi->smode, "srcreg",
			curi->size == sz_word ? sz_word : sz_long, "src");
		break;
	case i_MVMEL:
		// confirmed
		//if (using_prefetch)
			genmovemel_ce (opcode);
		//else
		//	genmovemel (opcode);
		break;
	case i_MVMLE:
		// confirmed
		//if (using_prefetch)
			genmovemle_ce (opcode);
		//else
		//	genmovemle (opcode);
		break;
	case i_TRAP:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tException_cpu(src + 32);\n");
		did_prefetch = 1;
		clear_m68k_offset();
		break;
	case i_MVR2USP:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		fill_prefetch_next ();
		printf ("\tregs.usp = src;\n");
		break;
	case i_MVUSP2R:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 2, 0, 0);
		fill_prefetch_next ();
		genastore ("regs.usp", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_RESET:
		fill_prefetch_next ();
		printf ("\tcpureset ();\n");
		sync_m68k_pc ();
		addcycles000 (128);
		if (using_prefetch) {
			printf ("\t%s (2);\n", prefetch_word);
			clear_m68k_offset();
		}
		break;
	case i_NOP:
		fill_prefetch_next ();
		break;
	case i_STOP:
		next_level_000();
		if (using_prefetch) {
			printf("\tuae_u16 sr = regs.irc;\n");
			m68k_pc_offset += 2;
		} else {
			genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
			printf("\tuae_u16 sr = src;\n");
		}
		// STOP undocumented features:
		// if SR is not set:
		// 68000 (68010?): Update SR, increase PC and then cause privilege violation exception (handled in newcpu)
		// 68000 (68010?): Traced STOP also runs 4 cycles faster.
		// 68020 68030: STOP works normally
		// 68040 68060: Immediate privilege violation exception
		if (cpu_level >= 4) {
			printf("\tif (!(sr & 0x2000)) {\n");
			incpc("%d", m68k_pc_offset);
			printf("\t\tException(8); return 4 * CYCLE_UNIT / 2;\n");
			printf("\t}\n");
		}
		printf("\tregs.sr = sr;\n");
		makefromsr ();
		printf ("\tm68k_setstopped ();\n");
		sync_m68k_pc ();
		// STOP does not prefetch anything
		did_prefetch = -1;
		next_cpu_level = cpu_level - 1;
		break;
	case i_LPSTOP: /* 68060 */
		printf ("\tuae_u16 sw = %s (2);\n", srcwi);
		printf ("\tif (sw != (0x100|0x80|0x40)) { Exception (4); return 4 * CYCLE_UNIT / 2; }\n");
		printf("\tif (!(regs.sr & 0x2000)) {\n");
		printf("\t\tException(8); return 4 * CYCLE_UNIT / 2;\n");
		printf("\t}\n");
		printf("\tregs.sr = %s (4);\n", srcwi);
		makefromsr ();
		printf ("\tm68k_setstopped();\n");
		m68k_pc_offset += 4;
		sync_m68k_pc ();
		fill_prefetch_full_ntx();
		break;
	case i_RTE:
		next_level_000 ();
		if (cpu_level == 0) {
			genamode (NULL, Aipi, "7", sz_word, "sr", 1, 0, GF_NOREFILL);
			genamode (NULL, Aipi, "7", sz_long, "pc", 1, 0, GF_NOREFILL);
			printf ("\tregs.sr = sr;\n");
			printf ("\tif (pc & 1) {\n");
			printf ("\t\texception3i (0x%04X, pc);\n", opcode);
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
			setpc ("pc");
			makefromsr ();
		} else if (cpu_level == 1 && using_prefetch) {
	    int old_brace_level = n_braces;
	    printf ("\tuae_u16 newsr; uae_u32 newpc;\n");
			printf ("\tfor (;;) {\n");
			printf ("\t\tuaecptr a = m68k_areg (regs, 7);\n");
			printf ("\t\tuae_u16 sr = %s (a);\n", srcw);
			count_read++;
			printf ("\t\tuae_u32 pc = %s (a + 2) << 16; pc |= %s (a + 4);\n", srcw, srcw);
			count_read += 2;
			printf ("\t\tuae_u16 format = %s (a + 2 + 4);\n", srcw);
			count_read++;
			printf ("\t\tint frame = format >> 12;\n");
			printf ("\t\tint offset = 8;\n");
			printf ("\t\tnewsr = sr; newpc = pc;\n");
	    printf ("\t\tif (frame == 0x0) { m68k_areg (regs, 7) += offset; break; }\n");
    	printf ("\t\telse if (frame == 0x8) { m68k_areg (regs, 7) += offset + 50; break; }\n");
	    printf ("\t\telse { m68k_areg (regs, 7) += offset; Exception_cpu(14); return 4 * CYCLE_UNIT / 2; }\n");
	    printf ("\t\tregs.sr = newsr;\n");
			makefromsr ();
			printf ("}\n");
	    pop_braces (old_brace_level);
	    printf ("\tregs.sr = newsr;\n");
			makefromsr ();
	    printf ("\tif (newpc & 1) {\n");
	    printf ("\t\texception3i (0x%04X, newpc);\n", opcode);
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
	    setpc ("newpc");
		} else {
		    int old_brace_level = n_braces;
		    printf ("\tuae_u16 newsr; uae_u32 newpc;\n");
			printf ("\tfor (;;) {\n");
			printf ("\t\tuaecptr a = m68k_areg (regs, 7);\n");
			printf ("\t\tuae_u16 sr = %s (a);\n", srcw);
			count_read++;
			printf ("\t\tuae_u32 pc = %s (a + 2);\n", srcl);
			count_read += 2;
			printf ("\t\tuae_u16 format = %s (a + 2 + 4);\n", srcw);
			count_read++;
			printf ("\t\tint frame = format >> 12;\n");
			printf ("\t\tint offset = 8;\n");
			printf ("\t\tnewsr = sr; newpc = pc;\n");
		    printf ("\t\tif (frame == 0x0) { m68k_areg (regs, 7) += offset; break; }\n");
		    printf ("\t\telse if (frame == 0x1) { m68k_areg (regs, 7) += offset; }\n");
  	    printf ("\t\telse if (frame == 0x2) { m68k_areg (regs, 7) += offset + 4; break; }\n");
	    if (cpu_level >= 4) {
				printf ("\t\telse if (frame == 0x3) { m68k_areg (regs, 7) += offset + 4; break; }\n");
			}
   		if (cpu_level >= 4) {
			  printf ("\t\telse if (frame == 0x4) { m68k_areg (regs, 7) += offset + 8; break; }\n");
			}
			if (cpu_level == 1) // 68010 only
		    printf ("\t\telse if (frame == 0x8) { m68k_areg (regs, 7) += offset + 50; break; }\n");
			if (cpu_level >= 4) {
		    	printf ("\t\telse if (frame == 0x7) { m68k_areg (regs, 7) += offset + 52; break; }\n");
			}
			if (cpu_level == 2 || cpu_level == 3) { // 68020/68030 only
			  printf ("\t\telse if (frame == 0x9) { m68k_areg (regs, 7) += offset + 12; break; }\n");
			  printf ("\t\telse if (frame == 0xa) { m68k_areg (regs, 7) += offset + 24; break; }\n");
	      printf ("\t\telse if (frame == 0xb) { m68k_areg (regs, 7) += offset + 84; break; }\n");
      }
	    printf ("\t\telse { m68k_areg (regs, 7) += offset; Exception_cpu(14); return 4 * CYCLE_UNIT / 2; }\n");
		    printf ("\t\tregs.sr = newsr;\n");
			makefromsr_t0();
			printf ("}\n");
		    pop_braces (old_brace_level);
		    printf ("\tregs.sr = newsr;\n");
			makefromsr_t0();
		    printf ("\tif (newpc & 1) {\n");
		    printf ("\t\texception3i (0x%04X, newpc);\n", opcode);
			printf ("\t\treturn 4 * CYCLE_UNIT / 2;\n");
			printf ("\t}\n");
		    setpc ("newpc");
		}
		/* PC is set and prefetch filled. */
		clear_m68k_offset();
		fill_prefetch_full_ntx();
		branch_inst = 1;
		next_cpu_level = cpu_level - 1;
		break;
	case i_RTD:
		genamode (NULL, Aipi, "7", sz_long, "pc", 1, 0, 0);
		genamode (curi, curi->smode, "srcreg", curi->size, "offs", 1, 0, 0);
		printf ("\tm68k_areg (regs, 7) += offs;\n");
		printf ("\tif (pc & 1) {\n");
		printf ("\t\texception3i (0x%04X, pc);\n", opcode);
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t}\n");
		setpc ("pc");
		/* PC is set and prefetch filled. */
		clear_m68k_offset();
		fill_prefetch_full ();
		branch_inst = 1;
		break;
	case i_LINK:
		// ce confirmed
		genamode (NULL, Apdi, "7", sz_long, "old", 2, 0, GF_AA);
		genamode (NULL, curi->smode, "srcreg", sz_long, "src", 1, 0, GF_AA);
		genamode (NULL, curi->dmode, "dstreg", curi->size, "offs", 1, 0, 0);
		genastore ("src", Apdi, "7", sz_long, "old");
		genastore ("m68k_areg (regs, 7)", curi->smode, "srcreg", sz_long, "src");
		printf ("\tm68k_areg (regs, 7) += offs;\n");
		fill_prefetch_next ();
		break;
	case i_UNLK:
		// ce confirmed
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		printf ("\tm68k_areg (regs, 7) = src;\n");
		genamode (NULL, Aipi, "7", sz_long, "old", 1, 0, 0);
		fill_prefetch_next ();
		genastore ("old", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_RTS:
		printf ("\tuaecptr pc = %s;\n", getpc);
		if (using_prefetch) {
			printf ("\tm68k_do_rtsi ();\n");
		} else {
			printf ("\tm68k_do_rts ();\n");
		}
	  printf ("\tif (%s & 1) {\n", getpc);
		printf ("\t\tuaecptr faultpc = %s;\n", getpc);
		setpc ("pc");
		printf ("\t\texception3i (0x%04X, faultpc);\n", opcode);
		printf ("\t\treturn 8 * CYCLE_UNIT / 2;\n");
		printf ("\t}\n");
		count_read += 2;
		clear_m68k_offset();
		fill_prefetch_full ();
		branch_inst = 1;
		break;
	case i_TRAPV:
		sync_m68k_pc ();
		fill_prefetch_next ();
		printf ("\tif (GET_VFLG ()) {\n");
		printf ("\t\tException_cpu(7);\n");
		printf ("\t\treturn 4 * CYCLE_UNIT / 2;\n");
		printf ("\t}\n");
		break;
	case i_RTR:
		printf ("\tuaecptr oldpc = %s;\n", getpc);
		printf ("\tMakeSR ();\n");
		genamode (NULL, Aipi, "7", sz_word, "sr", 1, 0, 0);
		genamode (NULL, Aipi, "7", sz_long, "pc", 1, 0, 0);
		printf ("\tregs.sr &= 0xFF00; sr &= 0xFF;\n");
		printf ("\tregs.sr |= sr;\n");
		setpc ("pc");
		makefromsr ();
		printf ("\tif (%s & 1) {\n", getpc);
		printf ("\t\tuaecptr faultpc = %s;\n", getpc);
		setpc ("oldpc");
		printf ("\t\texception3i (0x%04X, faultpc);\n", opcode);
		printf ("\t\treturn 8 * CYCLE_UNIT / 2;\n");
		printf ("\t}\n");
		clear_m68k_offset();
		fill_prefetch_full ();
		branch_inst = 1;
		break;
	case i_JSR:
		// possible idle cycle, prefetch from new address, stack high return addr, stack low, prefetch
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 0, 0, GF_AA|GF_NOREFILL);
		start_brace ();
		printf ("\tuaecptr nextpc = %s + %d;\n", getpc, m68k_pc_offset);
		if (using_exception_3) {
			printf ("\tif (srca & 1) {\n");
			printf ("\t\texception3i (opcode, srca);\n");
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
		}
		if (curi->smode == Ad16 || curi->smode == absw || curi->smode == PC16)
			addcycles000 (2);
		if (curi->smode == Ad8r || curi->smode == PC8r) {
			addcycles000 (6);
			if (cpu_level <= 1 && using_prefetch)
				printf ("\tnextpc += 2;\n");
		}
		setpc ("srca");
		clear_m68k_offset();
		fill_prefetch_1 (0);
		printf ("\tm68k_areg (regs, 7) -= 4;\n");
		if (using_prefetch) {
			printf ("\t%s (m68k_areg (regs, 7), nextpc >> 16);\n", dstw);
			printf ("\t%s (m68k_areg (regs, 7) + 2, nextpc);\n", dstw);
		} else {
			printf ("\t%s (m68k_areg (regs, 7), nextpc);\n", dstl);
		}
		count_write += 2;
    if(!using_prefetch && curi->smode == Aind)
      addcycles000 (4);
		fill_prefetch_next ();
		branch_inst = 1;
		break;
	case i_JMP:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 0, 0, GF_AA|GF_NOREFILL);
		if (using_prefetch && (curi->smode == Ad8r || curi->smode == PC8r))
			addcycles000 (4);
		if (using_exception_3) {
			printf ("\tif (srca & 1) {\n");
			printf ("\t\texception3i (opcode, srca);\n");
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
		}
    if(!(using_prefetch)) {
		  if (curi->smode != Aind && curi->smode != Ad8r && curi->smode != PC8r)
			  count_read--;
    }
		if (curi->smode == Ad16 || curi->smode == Ad8r || curi->smode == absw || curi->smode == PC16 || curi->smode == PC8r)
			addcycles000 (2);
		setpc ("srca");
		clear_m68k_offset();
		fill_prefetch_full ();
		branch_inst = 1;
		break;
	case i_BSR:
		// .b/.w = idle cycle, store high, store low, 2xprefetch
		printf ("\tuae_s32 s;\n");
		if (curi->size == sz_long) {
			if (next_cpu_level < 1)
				next_cpu_level = 1;
		}
		if (curi->size == sz_long && cpu_level < 2) {
			printf ("\tuae_u32 src = 0xffffffff;\n");
		} else {
			genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA|GF_NOREFILL);
		}
		printf ("\ts = (uae_s32)src + 2;\n");
		if (using_exception_3) {
			printf ("\tif (src & 1) {\n");
			printf ("\t\texception3b (opcode, %s + s, 0, 1, %s + s);\n", getpc, getpc);
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
		}
		addcycles000 (2);
		if (using_prefetch) {
			printf ("\tm68k_do_bsri (%s + %d, s);\n", getpc, m68k_pc_offset);
		} else {
			printf ("\tm68k_do_bsr (%s + %d, s);\n", getpc, m68k_pc_offset);
		}
		count_write += 2;
    if(!using_prefetch && curi->smode == imm1)
      count_write--;
		clear_m68k_offset();
		fill_prefetch_full ();
		branch_inst = 1;
		break;
	case i_Bcc:
		// bcc.b branch: idle cycle, prefetch, prefetch
		// bcc.b not branch: 2 idle cycles, prefetch
		if (curi->size == sz_long) {
			if (cpu_level < 2) {
				addcycles000 (2);
				printf ("\tif (cctrue (%d)) {\n", curi->cc);
				printf ("\t\texception3i (opcode, %s + 1);\n", getpc);
				returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
				printf ("\t}\n");
				sync_m68k_pc ();
				addcycles000 (2);
				irc2ir ();
				fill_prefetch_2 ();
				goto bccl_not68020;
			} else {
				if (next_cpu_level < 1)
					next_cpu_level = 1;
			}
		}
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA | (cpu_level < 2 ? GF_NOREFILL : 0));
		addcycles000 (2);
		printf ("\tif (!cctrue (%d)) goto didnt_jump;\n", curi->cc);
		if (using_exception_3) {
			printf ("\tif (src & 1) {\n");
			printf ("\t\texception3i (opcode, %s + 2 + (uae_s32)src);\n", getpc);
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t}\n");
		}
		push_ins_cnt();
		if (using_prefetch) {
			incpc ("(uae_s32)src + 2");
			fill_prefetch_full_000 ();
			printf ("\treturn 10 * CYCLE_UNIT / 2;\n");
		} else {
			incpc ("(uae_s32)src + 2");
			returncycles ("\t", 10);
		}
		pop_ins_cnt();
		printf ("didnt_jump:;\n");
		sync_m68k_pc ();
		if (curi->size == sz_byte) {
			addcycles000 (2);
			irc2ir ();
			fill_prefetch_2 ();
		} else if (curi->size == sz_word) {
			addcycles000 (2);
			fill_prefetch_full_000 ();
		} else {
			fill_prefetch_full_000 ();
		}
		if (using_prefetch) {
			count_read = curi->size == sz_byte ? 1 : 2;
		} else {
			count_read = 0;
			count_cycles = 0;
 		}
		insn_n_cycles = curi->size == sz_byte ? 8 : 12;
		branch_inst = 1;
bccl_not68020:
		break;
	case i_LEA:
		if (curi->smode == Ad8r || curi->smode == PC8r)
			addcycles000 (2);
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 0, GF_AA,
			curi->dmode, "dstreg", curi->size, "dst", 2, GF_AA);
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 0, 0, GF_AA);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 2, 0, GF_AA);
		if (curi->smode == Ad8r || curi->smode == PC8r)
			addcycles000 (2);
		fill_prefetch_next ();
		genastore ("srca", curi->dmode, "dstreg", curi->size, "dst");
		break;
	case i_PEA:
		if (curi->smode == Ad8r || curi->smode == PC8r)
			addcycles000 (2);
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 0, 0, GF_AA);
		genamode (NULL, Apdi, "7", sz_long, "dst", 2, 0, GF_AA);
		if (!(curi->smode == absw || curi->smode == absl))
			fill_prefetch_next ();
		if (curi->smode == Ad8r || curi->smode == PC8r)
			addcycles000 (2);
		genastore ("srca", Apdi, "7", sz_long, "dst");
		if ((curi->smode == absw || curi->smode == absl))
			fill_prefetch_next ();
		break;
	case i_DBcc:
		// cc true: idle cycle, prefetch
		// cc false, counter expired: idle cycle, prefetch (from branch address), 2xprefetch (from next address)
		// cc false, counter not expired: idle cycle, prefetch
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "src", 1, GF_AA | (cpu_level < 2 ? GF_NOREFILL : 0),
			curi->dmode, "dstreg", curi->size, "offs", 1, GF_AA | (cpu_level < 2 ? GF_NOREFILL : 0));
		//genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_AA | (cpu_level < 2 ? GF_NOREFILL : 0));
		//genamode (curi, curi->dmode, "dstreg", curi->size, "offs", 1, 0, GF_AA | (cpu_level < 2 ? GF_NOREFILL : 0));
		printf ("\tuaecptr oldpc = %s;\n", getpc);
		addcycles000 (2);
		push_ins_cnt();
		printf ("\tif (!cctrue (%d)) {\n", curi->cc);
		incpc ("(uae_s32)offs + 2");
		printf ("\t");
		fill_prefetch_1 (0);
		printf ("\t");
		genastore ("(src - 1)", curi->smode, "srcreg", curi->size, "src");

		printf ("\t\tif (src) {\n");
		if (using_exception_3) {
			printf ("\t\t\tif (offs & 1) {\n");
			printf ("\t\t\t\texception3i (opcode, %s + 2 + (uae_s32)offs + 2);\n", getpc);
			returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
			printf ("\t\t\t}\n");
		}
		irc2ir ();
		fill_prefetch_1 (2);
		returncycles ("\t\t\t", 10);
		printf ("\t\t}\n");
		addcycles000_nonce("\t\t", 2 + 2);
		printf ("\t} else {\n");
		// cc == true
		addcycles000_nonce("\t\t", 2);
		printf ("\t}\n");
		pop_ins_cnt();
		setpc ("oldpc + %d", m68k_pc_offset);
		clear_m68k_offset();
		fill_prefetch_full_000 ();
		insn_n_cycles = 12;
		if (using_prefetch)
			count_read = 2;
		else
			count_read = 1;
		count_write = 0;
		branch_inst = 1;
		break;
	case i_Scc:
		// confirmed
		next_level_000 ();
		genamode (curi, curi->smode, "srcreg", curi->size, "src", cpu_level == 0 ? 1 : 2, 0, 0);
		start_brace ();
		fill_prefetch_next();
		start_brace ();
		printf ("\tint val = cctrue (%d) ? 0xff : 0;\n", curi->cc);
		if (isreg (curi->smode)) {
			addcycles000_3 ("\t");
			addcycles000_nonces("\t", "(val ? 2 : 0)");
		}
		genastore ("val", curi->smode, "srcreg", curi->size, "src");
    if(!using_prefetch && curi->smode != Dreg && cpu_level == 1)
      addcycles000 (2);
		break;
	case i_DIVU:
		genamodedual (curi,
			curi->smode, "srcreg", sz_word, "src", 1, 0,
			curi->dmode, "dstreg", sz_long, "dst", 1, 0);
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tif (src == 0) {\n");
		if (cpu_level > 1)
			printf ("\t\tdivbyzero_special (0, dst);\n");
		incpc ("%d", m68k_pc_offset);
		printf ("\t\tException_cpu(5);\n");
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t} else {\n");
		printf ("\t\tuae_u32 newv = (uae_u32)dst / (uae_u32)(uae_u16)src;\n");
		printf ("\t\tuae_u32 rem = (uae_u32)dst %% (uae_u32)(uae_u16)src;\n");
		addcycles000_nonces("\t\t", "(getDivu68kCycles((uae_u32)dst, (uae_u16)src)) - 4");
		fill_prefetch_next ();
		/* The N flag appears to be set each time there is an overflow.
		* Weird. but 68020 only sets N when dst is negative.. */
		printf ("\t\tif (newv > 0xffff) {\n");
		printf ("\t\t\tsetdivuoverflowflags((uae_u32)dst, (uae_u16)src);\n");
		printf ("\t\t} else {\n");
		printf ("\t\t"); genflags (flag_logical, sz_word, "newv", "", "");
		printf ("\t\t\tnewv = (newv & 0xffff) | ((uae_u32)rem << 16);\n");
		printf ("\t\t"); genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		printf ("\t\t}\n");
		sync_m68k_pc ();
		printf ("\t}\n");
		count_ncycles++;
		//insn_n_cycles += 136 - (136 - 76) / 2; /* average */
		break;
	case i_DIVS:
		genamodedual (curi,
			curi->smode, "srcreg", sz_word, "src", 1, 0,
			curi->dmode, "dstreg", sz_long, "dst", 1, 0);
		printf ("\tif (src == 0) {\n");
		if (cpu_level > 1)
			printf ("\t\tdivbyzero_special (1, dst);\n");
		incpc ("%d", m68k_pc_offset);
		printf ("\t\tException_cpu(5);\n");
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t}\n");
		printf ("\tCLEAR_CZNV ();\n");
		addcycles000_nonces("\t\t", "(getDivs68kCycles((uae_s32)dst, (uae_s16)src)) - 4");
		fill_prefetch_next ();
		printf ("\tif (dst == 0x80000000 && src == -1) {\n");
		printf ("\t\tsetdivsoverflowflags((uae_s32)dst, (uae_s16)src);\n");
		printf ("\t} else {\n");
		printf ("\t\tuae_s32 newv = (uae_s32)dst / (uae_s32)(uae_s16)src;\n");
		printf ("\t\tuae_u16 rem = (uae_s32)dst %% (uae_s32)(uae_s16)src;\n");
		printf ("\t\tif ((newv & 0xffff8000) != 0 && (newv & 0xffff8000) != 0xffff8000) {\n");
		printf ("\t\t\tsetdivsoverflowflags((uae_s32)dst, (uae_s16)src);\n");
		printf ("\t\t} else {\n");
		printf ("\t\t\tif (((uae_s16)rem < 0) != ((uae_s32)dst < 0)) rem = -rem;\n");
		genflags (flag_logical, sz_word, "newv", "", "");
		printf ("\t\t\tnewv = (newv & 0xffff) | ((uae_u32)rem << 16);\n");
		printf ("\t\t"); genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		printf ("\t\t}\n");
		printf ("\t}\n");
		sync_m68k_pc ();
		count_ncycles++;
		//insn_n_cycles += 156 - (156 - 120) / 2; /* average */
		break;
	case i_MULU:
		genamodedual (curi,
			curi->smode, "srcreg", sz_word, "src", 1, 0,
			curi->dmode, "dstreg", sz_word, "dst", 1, 0);
		fill_prefetch_next();
		start_brace ();
		printf ("\tuae_u32 newv = (uae_u32)(uae_u16)dst * (uae_u32)(uae_u16)src;\n");
		genflags (flag_logical, sz_long, "newv", "", "");
    addcycles000_nonces("\t", "bitset_count16(src) * 2");
		genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		sync_m68k_pc ();
		count_cycles += 38 - 4;
		count_ncycles++;
		insn_n_cycles += (70 - 38) / 2 + 38; /* average */
		break;
	case i_MULS:
		genamodedual (curi,
			curi->smode, "srcreg", sz_word, "src", 1, 0,
			curi->dmode, "dstreg", sz_word, "dst", 1, 0);
		fill_prefetch_next();
		start_brace ();
		printf ("\tuae_u32 newv = (uae_s32)(uae_s16)dst * (uae_s32)(uae_s16)src;\n");
		genflags (flag_logical, sz_long, "newv", "", "");
		addcycles000_nonces("\t", "bitset_count16(src ^ (src << 1)) * 2");
		genastore ("newv", curi->dmode, "dstreg", sz_long, "dst");
		count_cycles += 38 - 4;
		count_ncycles++;
		insn_n_cycles += (70 - 38) / 2 + 38; /* average */
		break;
	case i_CHK:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, 0);
		sync_m68k_pc ();
		addcycles000 (4);
		printf ("\tif (dst > src) {\n");
		printf ("\t\tSET_NFLG (0);\n");
		printf ("\t\tException_cpu(6);\n");
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t}\n");
		addcycles000 (2);
		printf ("\tif ((uae_s32)dst < 0) {\n");
		printf ("\t\tSET_NFLG (1);\n");
		printf ("\t\tException_cpu(6);\n");
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf ("\t}\n");
		fill_prefetch_next ();
		break;
	case i_CHK2:
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 2, 0, 0);
		fill_prefetch_0 ();
		printf ("\t{uae_s32 upper,lower,reg = regs.regs[(extra >> 12) & 15];\n");
		switch (curi->size) {
		case sz_byte:
			printf ("\tlower = (uae_s32)(uae_s8)%s (dsta); upper = (uae_s32)(uae_s8)%s (dsta + 1);\n", srcb, srcb);
			printf ("\tif ((extra & 0x8000) == 0) reg = (uae_s32)(uae_s8)reg;\n");
			break;
		case sz_word:
			printf ("\tlower = (uae_s32)(uae_s16)%s (dsta); upper = (uae_s32)(uae_s16)%s (dsta + 2);\n", srcw, srcw);
			printf ("\tif ((extra & 0x8000) == 0) reg = (uae_s32)(uae_s16)reg;\n");
			break;
		case sz_long:
			printf ("\tlower = %s (dsta); upper = %s (dsta + 4);\n", srcl, srcl);
			break;
		default:
			term ();
		}
		printf ("\tSET_ZFLG (upper == reg || lower == reg);\n");
		printf ("\tSET_CFLG_ALWAYS (lower <= upper ? reg < lower || reg > upper : reg > upper || reg < lower);\n");
		printf ("\tif ((extra & 0x800) && GET_CFLG ()) { Exception_cpu(6); \n");
		returncycles_exception ("", (count_read + 1 + count_write) * 4 + count_cycles);
		printf("\t}\n}\n");
		break;

	case i_ASR:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 sign = (%s & val) >> %d;\n", cmask (curi->size), bit_size (curi->size) - 1);
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tif (cnt >= %d) {\n", bit_size (curi->size));
		printf ("\t\tval = %s & (uae_u32)-sign;\n", bit_mask (curi->size));
		printf ("\t\tSET_CFLG (sign);\n");
		duplicate_carry (1);
		if (source_is_imm1_8 (curi))
			printf ("\t} else {\n");
		else
			printf ("\t} else if (cnt > 0) {\n");
		printf ("\t\tval >>= cnt - 1;\n");
		printf ("\t\tSET_CFLG (val & 1);\n");
		duplicate_carry (1);
		printf ("\t\tval >>= 1;\n");
		printf ("\t\tval |= (%s << (%d - cnt)) & (uae_u32)-sign;\n",
			bit_mask (curi->size),
			bit_size (curi->size));
		printf ("\t\tval &= %s;\n", bit_mask (curi->size));
		printf ("\t}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ASL:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tif (cnt >= %d) {\n", bit_size (curi->size));
		printf ("\t\tSET_VFLG (val != 0);\n");
		printf ("\t\tSET_CFLG (cnt == %d ? val & 1 : 0);\n",
			bit_size (curi->size));
		duplicate_carry (1);
		printf ("\t\tval = 0;\n");
		if (source_is_imm1_8 (curi))
			printf ("\t} else {\n");
		else
			printf ("\t} else if (cnt > 0) {\n");
		printf ("\t\tuae_u32 mask = (%s << (%d - cnt)) & %s;\n",
			bit_mask (curi->size),
			bit_size (curi->size) - 1,
			bit_mask (curi->size));
		printf ("\t\tSET_VFLG ((val & mask) != mask && (val & mask) != 0);\n");
		printf ("\t\tval <<= cnt - 1;\n");
		printf ("\t\tSET_CFLG ((val & %s) >> %d);\n", cmask (curi->size), bit_size (curi->size) - 1);
		duplicate_carry (1);
		printf ("\t\tval <<= 1;\n");
		printf ("\t\tval &= %s;\n", bit_mask (curi->size));
		printf ("\t}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_LSR:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tif (cnt >= %d) {\n", bit_size (curi->size));
		printf ("\t\tSET_CFLG ((cnt == %d) & (val >> %d));\n",
			bit_size (curi->size), bit_size (curi->size) - 1);
		duplicate_carry (1);
		printf ("\t\tval = 0;\n");
		if (source_is_imm1_8 (curi))
			printf ("\t} else {\n");
		else
			printf ("\t} else if (cnt > 0) {\n");
		printf ("\t\tval >>= cnt - 1;\n");
		printf ("\t\tSET_CFLG (val & 1);\n");
		duplicate_carry (1);
		printf ("\t\tval >>= 1;\n");
		printf ("\t}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_LSL:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		printf ("\tif (cnt >= %d) {\n", bit_size (curi->size));
		printf ("\t\tSET_CFLG (cnt == %d ? val & 1 : 0);\n", bit_size (curi->size));
		duplicate_carry (1);
		printf ("\t\tval = 0;\n");
		if (source_is_imm1_8 (curi))
			printf ("\t} else {\n");
		else
			printf ("\t} else if (cnt > 0) {\n");
		printf ("\t\tval <<= (cnt - 1);\n");
		printf ("\t\tSET_CFLG ((val & %s) >> %d);\n", cmask (curi->size), bit_size (curi->size) - 1);
		duplicate_carry (1);
		printf ("\t\tval <<= 1;\n");
		printf ("\tval &= %s;\n", bit_mask (curi->size));
		printf ("\t}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ROL:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		if (source_is_imm1_8 (curi))
			printf ("{");
		else
			printf ("\tif (cnt > 0) {\n");
		printf ("\tuae_u32 loval;\n");
		printf ("\tcnt &= %d;\n", bit_size (curi->size) - 1);
		printf ("\tloval = val >> (%d - cnt);\n", bit_size (curi->size));
		printf ("\tval <<= cnt;\n");
		printf ("\tval |= loval;\n");
		printf ("\tval &= %s;\n", bit_mask (curi->size));
		printf ("\tSET_CFLG (val & 1);\n");
		printf ("}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ROR:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		if (source_is_imm1_8 (curi))
			printf ("{");
		else
			printf ("\tif (cnt > 0) {");
		printf ("\tuae_u32 hival;\n");
		printf ("\tcnt &= %d;\n", bit_size (curi->size) - 1);
		printf ("\thival = val << (%d - cnt);\n", bit_size (curi->size));
		printf ("\tval >>= cnt;\n");
		printf ("\tval |= hival;\n");
		printf ("\tval &= %s;\n", bit_mask (curi->size));
		printf ("\tSET_CFLG ((val & %s) >> %d);\n", cmask (curi->size), bit_size (curi->size) - 1);
		printf ("\t}\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ROXL:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		if (source_is_imm1_8 (curi))
			printf ("{");
		else {
			force_range_for_rox ("cnt", curi->size);
			printf ("\tif (cnt > 0) {\n");
		}
		printf ("\tcnt--;\n");
		printf ("\t{\n\tuae_u32 carry;\n");
		printf ("\tuae_u32 loval = val >> (%d - cnt);\n", bit_size (curi->size) - 1);
		printf ("\tcarry = loval & 1;\n");
		printf ("\tval = (((val << 1) | GET_XFLG ()) << cnt) | (loval >> 1);\n");
		printf ("\tSET_XFLG (carry);\n");
		printf ("\tval &= %s;\n", bit_mask (curi->size));
		printf ("\t} }\n");
		printf ("\tSET_CFLG (GET_XFLG ());\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ROXR:
		genamodedual (curi,
			curi->smode, "srcreg", curi->size, "cnt", 1, 0,
			curi->dmode, "dstreg", curi->size, "data", 1, GF_RMW);
		//genamode (curi, curi->smode, "srcreg", curi->size, "cnt", 1, 0, 0);
		//genamode (curi, curi->dmode, "dstreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tint ccnt = cnt & 63;\n");
		printf ("\tcnt &= 63;\n");
		printf ("\tCLEAR_CZNV ();\n");
		if (source_is_imm1_8 (curi))
			printf ("{");
		else {
			force_range_for_rox ("cnt", curi->size);
			printf ("\tif (cnt > 0) {\n");
		}
		printf ("\tcnt--;\n");
		printf ("\t{\n\tuae_u32 carry;\n");
		printf ("\tuae_u32 hival = (val << 1) | GET_XFLG ();\n");
		printf ("\thival <<= (%d - cnt);\n", bit_size (curi->size) - 1);
		printf ("\tval >>= cnt;\n");
		printf ("\tcarry = val & 1;\n");
		printf ("\tval >>= 1;\n");
		printf ("\tval |= hival;\n");
		printf ("\tSET_XFLG (carry);\n");
		printf ("\tval &= %s;\n", bit_mask (curi->size));
		printf ("\t} }\n");
		printf ("\tSET_CFLG (GET_XFLG ());\n");
		genflags (flag_logical_noclobber, curi->size, "val", "", "");
		shift_ce (curi->dmode, curi->size);
		genastore ("val", curi->dmode, "dstreg", curi->size, "data");
		break;
	case i_ASRW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 sign = %s & val;\n", cmask (curi->size));
		printf ("\tuae_u32 cflg = val & 1;\n");
		printf ("\tval = (val >> 1) | sign;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (cflg);\n");
		duplicate_carry (0);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_ASLW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 sign = %s & val;\n", cmask (curi->size));
		printf ("\tuae_u32 sign2;\n");
		printf ("\tval <<= 1;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tsign2 = %s & val;\n", cmask (curi->size));
		printf ("\tSET_CFLG (sign != 0);\n");
		duplicate_carry (0);
		printf ("\tSET_VFLG (GET_VFLG () | (sign2 != sign));\n");
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_LSRW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u32 val = (uae_u8)data;\n"); break;
		case sz_word: printf ("\tuae_u32 val = (uae_u16)data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & 1;\n");
		printf ("\tval >>= 1;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry);\n");
		duplicate_carry (0);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_LSLW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u8 val = data;\n"); break;
		case sz_word: printf ("\tuae_u16 val = data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & %s;\n", cmask (curi->size));
		printf ("\tval <<= 1;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry >> %d);\n", bit_size (curi->size) - 1);
		duplicate_carry (0);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_ROLW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u8 val = data;\n"); break;
		case sz_word: printf ("\tuae_u16 val = data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & %s;\n", cmask (curi->size));
		printf ("\tval <<= 1;\n");
		printf ("\tif (carry)  val |= 1;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry >> %d);\n", bit_size (curi->size) - 1);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_RORW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u8 val = data;\n"); break;
		case sz_word: printf ("\tuae_u16 val = data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & 1;\n");
		printf ("\tval >>= 1;\n");
		printf ("\tif (carry) val |= %s;\n", cmask (curi->size));
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry);\n");
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_ROXLW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u8 val = data;\n"); break;
		case sz_word: printf ("\tuae_u16 val = data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & %s;\n", cmask (curi->size));
		printf ("\tval <<= 1;\n");
		printf ("\tif (GET_XFLG ()) val |= 1;\n");
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry >> %d);\n", bit_size (curi->size) - 1);
		duplicate_carry (0);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_ROXRW:
		genamode (curi, curi->smode, "srcreg", curi->size, "data", 1, 0, GF_RMW);
		fill_prefetch_next ();
		start_brace ();
		switch (curi->size) {
		case sz_byte: printf ("\tuae_u8 val = data;\n"); break;
		case sz_word: printf ("\tuae_u16 val = data;\n"); break;
		case sz_long: printf ("\tuae_u32 val = data;\n"); break;
		default: term ();
		}
		printf ("\tuae_u32 carry = val & 1;\n");
		printf ("\tval >>= 1;\n");
		printf ("\tif (GET_XFLG ()) val |= %s;\n", cmask (curi->size));
		genflags (flag_logical, curi->size, "val", "", "");
		printf ("\tSET_CFLG (carry);\n");
		duplicate_carry (0);
		genastore ("val", curi->smode, "srcreg", curi->size, "data");
		break;
	case i_MOVEC2:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tint regno = (src >> 12) & 15;\n");
		printf ("\tuae_u32 *regp = regs.regs + regno;\n");
		printf ("\tif (! m68k_movec2(src & 0xFFF, regp)) goto %s;\n", endlabelstr);
		need_endlabel = 1;
		break;
	case i_MOVE2C:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, 0);
		fill_prefetch_next ();
		start_brace ();
		printf ("\tint regno = (src >> 12) & 15;\n");
		printf ("\tuae_u32 *regp = regs.regs + regno;\n");
		printf ("\tif (! m68k_move2c(src & 0xFFF, regp)) goto %s;\n", endlabelstr);
		need_endlabel = 1;
		break;
	case i_CAS:
		{
			int old_brace_level;
			genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_LRMW);
			genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, GF_LRMW);
			fill_prefetch_0 ();
			start_brace ();
			printf ("\tint ru = (src >> 6) & 7;\n");
			printf ("\tint rc = src & 7;\n");
			genflags (flag_cmp, curi->size, "newv", "m68k_dreg (regs, rc)", "dst");
			printf ("\tif (GET_ZFLG ())");
			old_brace_level = n_braces;
			start_brace ();
			printf ("\n\t");
			genastore_cas ("(m68k_dreg (regs, ru))", curi->dmode, "dstreg", curi->size, "dst");
			printf ("\t");
			pop_braces (old_brace_level);
			printf ("else");
			start_brace ();
			printf ("\n");
			if (cpu_level >= 4) {
				// apparently 68040/060 needs to always write at the end of RMW cycle
				printf ("\t");
				genastore_cas ("dst", curi->dmode, "dstreg", curi->size, "dst");
			}
  			switch (curi->size) {
			    case sz_byte:
				printf ("\t\tm68k_dreg(regs, rc) = (m68k_dreg(regs, rc) & ~0xff) | (dst & 0xff);\n");
				break;
			    case sz_word:
				printf ("\t\tm68k_dreg(regs, rc) = (m68k_dreg(regs, rc) & ~0xffff) | (dst & 0xffff);\n");
				break;
	    		default:
				printf ("\t\tm68k_dreg(regs, rc) = dst;\n");
				break;
			}	
			pop_braces (old_brace_level);
		}
		break;
	case i_CAS2:
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, GF_LRMW);
		printf ("\tuae_u32 rn1 = regs.regs[(extra >> 28) & 15];\n");
		printf ("\tuae_u32 rn2 = regs.regs[(extra >> 12) & 15];\n");
		if (curi->size == sz_word) {
			int old_brace_level = n_braces;
			printf ("\tuae_u16 dst1 = %s (rn1), dst2 = %s (rn2);\n", srcwlrmw, srcwlrmw);
			genflags (flag_cmp, curi->size, "newv", "m68k_dreg (regs, (extra >> 16) & 7)", "dst1");
			printf ("\tif (GET_ZFLG ()) {\n");
			genflags (flag_cmp, curi->size, "newv", "m68k_dreg (regs, extra & 7)", "dst2");
			printf ("\tif (GET_ZFLG ()) {\n");
			printf ("\t%s (rn1, m68k_dreg (regs, (extra >> 22) & 7));\n", dstwlrmw);
			printf ("\t%s (rn2, m68k_dreg (regs, (extra >> 6) & 7));\n", dstwlrmw);
			printf ("\t}}\n");
			pop_braces (old_brace_level);
			printf ("\tif (! GET_ZFLG ()) {\n");
			printf ("\tm68k_dreg (regs, (extra >> 0) & 7) = (m68k_dreg (regs, (extra >> 6) & 7) & ~0xffff) | (dst2 & 0xffff);\n");
			printf ("\tm68k_dreg (regs, (extra >> 16) & 7) = (m68k_dreg (regs, (extra >> 22) & 7) & ~0xffff) | (dst1 & 0xffff);\n");
			printf ("\t}\n");
		} else {
			int old_brace_level = n_braces;
			printf ("\tuae_u32 dst1 = %s (rn1), dst2 = %s (rn2);\n", srcllrmw, srcllrmw);
			genflags (flag_cmp, curi->size, "newv", "m68k_dreg (regs, (extra >> 16) & 7)", "dst1");
			printf ("\tif (GET_ZFLG ()) {\n");
			genflags (flag_cmp, curi->size, "newv", "m68k_dreg (regs, extra & 7)", "dst2");
			printf ("\tif (GET_ZFLG ()) {\n");
			printf ("\t%s (rn1, m68k_dreg (regs, (extra >> 22) & 7));\n", dstllrmw);
			printf ("\t%s (rn2, m68k_dreg (regs, (extra >> 6) & 7));\n", dstllrmw);
			printf ("\t}}\n");
			pop_braces (old_brace_level);
			printf ("\tif (! GET_ZFLG ()) {\n");
			printf ("\tm68k_dreg (regs, (extra >> 0) & 7) = dst2;\n");
			printf ("\tm68k_dreg (regs, (extra >> 16) & 7) = dst1;\n");
			printf ("\t}\n");
		}
		break;
	case i_MOVES: /* ignore DFC and SFC when using_mmu == false */
		{
			int old_brace_level;
			genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
			printf ("\tif (extra & 0x800)\n");
			{
				int old_m68k_pc_offset = m68k_pc_offset;
				old_brace_level = n_braces;
				start_brace ();
				printf ("\tuae_u32 src = regs.regs[(extra >> 12) & 15];\n");
				genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 2, 0, 0);
				genastore_fc ("src", curi->dmode, "dstreg", curi->size, "dst");
				pop_braces (old_brace_level);
				m68k_pc_offset = old_m68k_pc_offset;
			}
			printf ("else");
			{
				start_brace ();
				genamode (curi, curi->dmode, "dstreg", curi->size, "src", 1, 0, GF_FC);
				printf ("\tif (extra & 0x8000) {\n");
				switch (curi->size) {
				case sz_byte: printf ("\tm68k_areg (regs, (extra >> 12) & 7) = (uae_s32)(uae_s8)src;\n"); break;
				case sz_word: printf ("\tm68k_areg (regs, (extra >> 12) & 7) = (uae_s32)(uae_s16)src;\n"); break;
				case sz_long: printf ("\tm68k_areg (regs, (extra >> 12) & 7) = src;\n"); break;
				default: term ();
				}
				printf ("\t} else {\n");
				genastore ("src", Dreg, "(extra >> 12) & 7", curi->size, "");
				printf ("\t}\n");
				pop_braces (old_brace_level);
			}
		}
		break;
	case i_BKPT:		/* only needed for hardware emulators */
		sync_m68k_pc ();
		printf ("\top_illg (opcode);\n");
		did_prefetch = -1;
		break;
	case i_CALLM:		/* not present in 68030 */
		sync_m68k_pc ();
		printf ("\top_illg (opcode);\n");
		did_prefetch = -1;
		break;
	case i_RTM:		/* not present in 68030 */
		sync_m68k_pc ();
		printf ("\top_illg (opcode);\n");
		did_prefetch = -1;
		break;
	case i_TRAPcc:
		if (curi->smode != am_unknown && curi->smode != am_illg)
			genamode (curi, curi->smode, "srcreg", curi->size, "dummy", 1, 0, 0);
		fill_prefetch_0 ();
		printf ("\tif (cctrue (%d)) { Exception_cpu(7); goto %s; }\n", curi->cc, endlabelstr);
		need_endlabel = 1;
		break;
	case i_DIVL:
    genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
    printf ("\tif (extra & 0x800) cyc = 12 * CYCLE_UNIT / 2;\n");
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, 0);
		sync_m68k_pc ();
    printf ("\tif (dst == 0) {\n");
    printf ("\t\tException_cpu(5);\n");
	  printf ("\t\treturn 4 * CYCLE_UNIT / 2;\n");
    printf ("\t}\n");
		printf ("\tm68k_divl(opcode, dst, extra);\n");
		break;
	case i_MULL:
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
		genamode (curi, curi->dmode, "dstreg", curi->size, "dst", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tm68k_mull(opcode, dst, extra);\n");
		break;
	case i_BFTST:
	case i_BFEXTU:
	case i_BFCHG:
	case i_BFEXTS:
	case i_BFCLR:
	case i_BFFFO:
	case i_BFSET:
	case i_BFINS:
		{
			const char *getb, *putb;

			getb = "get_bitfield";
			putb = "put_bitfield";

			genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
			genamode (curi, curi->dmode, "dstreg", sz_long, "dst", 2, 0, 0);
			start_brace ();
			printf ("\tuae_u32 bdata[2];\n");
			printf ("\tuae_s32 offset = extra & 0x800 ? m68k_dreg(regs, (extra >> 6) & 7) : (extra >> 6) & 0x1f;\n");
			printf ("\tint width = (((extra & 0x20 ? m68k_dreg(regs, extra & 7) : extra) -1) & 0x1f) +1;\n");
			if (curi->mnemo == i_BFFFO)
				printf("\tuae_u32 offset2 = offset;\n");
			if (curi->dmode == Dreg) {
				printf ("\tuae_u32 tmp = m68k_dreg(regs, dstreg);\n");
				printf ("\toffset &= 0x1f;\n");
				printf ("\ttmp = (tmp << offset) | (tmp >> (32 - offset));\n");
				printf ("\tbdata[0] = tmp & ((1 << (32 - width)) - 1);\n");
			} else {
				printf ("\tuae_u32 tmp;\n");
				printf ("\tdsta += offset >> 3;\n");
				printf ("\ttmp = %s (dsta, bdata, offset, width);\n", getb);
			}
			printf ("\tSET_NFLG_ALWAYS (((uae_s32)tmp) < 0 ? 1 : 0);\n");
			if (curi->mnemo == i_BFEXTS)
				printf ("\ttmp = (uae_s32)tmp >> (32 - width);\n");
			else
				printf ("\ttmp >>= (32 - width);\n");
			printf ("\tSET_ZFLG (tmp == 0); SET_VFLG (0); SET_CFLG (0);\n");
			switch (curi->mnemo) {
			case i_BFTST:
				break;
			case i_BFEXTU:
			case i_BFEXTS:
				printf ("\tm68k_dreg (regs, (extra >> 12) & 7) = tmp;\n");
				break;
			case i_BFCHG:
				printf ("\ttmp = tmp ^ (0xffffffffu >> (32 - width));\n");
				break;
			case i_BFCLR:
				printf ("\ttmp = 0;\n");
				break;
			case i_BFFFO:
				printf ("\t{ uae_u32 mask = 1 << (width - 1);\n");
				printf ("\twhile (mask) { if (tmp & mask) break; mask >>= 1; offset2++; }}\n");
				printf ("\tm68k_dreg (regs, (extra >> 12) & 7) = offset2;\n");
				break;
			case i_BFSET:
				printf ("\ttmp = 0xffffffffu >> (32 - width);\n");
				break;
			case i_BFINS:
				printf ("\ttmp = m68k_dreg (regs, (extra >> 12) & 7);\n");
				printf ("\ttmp = tmp & (0xffffffffu >> (32 - width));\n");
				printf ("\tSET_NFLG (tmp & (1 << (width - 1)) ? 1 : 0);\n");
				printf ("\tSET_ZFLG (tmp == 0);\n");
				break;
			default:
				break;
			}
			if (curi->mnemo == i_BFCHG
				|| curi->mnemo == i_BFCLR
				|| curi->mnemo == i_BFSET
				|| curi->mnemo == i_BFINS) {
					if (curi->dmode == Dreg) {
						printf ("\ttmp = bdata[0] | (tmp << (32 - width));\n");
						printf ("\tm68k_dreg(regs, dstreg) = (tmp >> offset) | (tmp << (32 - offset));\n");
					} else {
						printf ("\t%s(dsta, bdata, tmp, offset, width);\n", putb);
					}
			}
		}
		break;
	case i_PACK:
		if (curi->smode == Dreg) {
			printf ("\tuae_u16 val = m68k_dreg (regs, srcreg) + %s;\n", gen_nextiword (0));
			printf ("\tm68k_dreg (regs, dstreg) = (m68k_dreg (regs, dstreg) & 0xffffff00) | ((val >> 4) & 0xf0) | (val & 0xf);\n");
		} else {
			printf ("\tuae_u16 val;\n");
			printf ("\tm68k_areg (regs, srcreg) -= areg_byteinc[srcreg];\n");
			printf ("\tval = (uae_u16)(%s (m68k_areg (regs, srcreg)) & 0xff);\n", srcb);
			printf ("\tm68k_areg (regs, srcreg) -= areg_byteinc[srcreg];\n");
			printf ("\tval = val | ((uae_u16)(%s (m68k_areg (regs, srcreg)) & 0xff) << 8);\n", srcb);
			printf ("\tval += %s;\n", gen_nextiword(0));
			printf ("\tm68k_areg (regs, dstreg) -= areg_byteinc[dstreg];\n");
			printf ("\t%s (m68k_areg (regs, dstreg),((val >> 4) & 0xf0) | (val & 0xf));\n", dstb);
		}
		break;
	case i_UNPK:
		if (curi->smode == Dreg) {
			printf ("\tuae_u16 val = m68k_dreg (regs, srcreg);\n");
			printf ("\tval = ((val << 4) & 0xf00) | (val & 0xf);\n");
			printf ("\tval += %s;\n", gen_nextiword(0));
			printf ("\tm68k_dreg (regs, dstreg) = (m68k_dreg (regs, dstreg) & 0xffff0000) | (val & 0xffff);\n");
		} else {
			printf ("\tuae_u16 val;\n");
			printf ("\tm68k_areg (regs, srcreg) -= areg_byteinc[srcreg];\n");
			printf ("\tval = (uae_u16)(%s (m68k_areg (regs, srcreg)) & 0xff);\n", srcb);
			printf ("\tval = (((val << 4) & 0xf00) | (val & 0xf)) + %s;\n", gen_nextiword (0));
			if (cpu_level >= 2) {
				printf ("\tm68k_areg (regs, dstreg) -= 2 * areg_byteinc[dstreg];\n");
				printf ("\t%s (m68k_areg (regs, dstreg) + areg_byteinc[dstreg], val);\n", dstb);
				printf ("\t%s (m68k_areg (regs, dstreg), val >> 8);\n", dstb);
			} else {
				printf ("\tm68k_areg (regs, dstreg) -= areg_byteinc[dstreg];\n");
				printf ("\t%s (m68k_areg (regs, dstreg),val);\n", dstb);
				printf ("\tm68k_areg (regs, dstreg) -= areg_byteinc[dstreg];\n");
				printf ("\t%s (m68k_areg (regs, dstreg),val >> 8);\n", dstb);
			}
		}
		break;
	case i_TAS:
		genamode (curi, curi->smode, "srcreg", curi->size, "src", 1, 0, GF_LRMW);
		genflags (flag_logical, curi->size, "src", "", "");
		if (!isreg (curi->smode))
			addcycles000 (6);
		fill_prefetch_next ();
		printf ("\tsrc |= 0x80;\n");
		if (next_cpu_level < 2)
			next_cpu_level = 2 - 1;
		genastore_tas ("src", curi->smode, "srcreg", curi->size, "src");
		break;
	case i_FPP:
		fpulimit();
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tfpuop_arithmetic(opcode, extra);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			need_endlabel = 1;
		}
		break;
	case i_FDBcc:
		fpulimit();
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tfpuop_dbcc (opcode, extra);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			printf ("\tif (regs.fp_branch) {\n");
			printf ("\t\tregs.fp_branch = false;\n");
			printf ("\t\tfill_prefetch();\n");
			printf ("\t\tgoto %s;\n", endlabelstr);
			printf ("\t}\n");
			need_endlabel = 1;
		} else {
			printf ("\tif (regs.fp_branch) {\n");
			printf ("\t\tregs.fp_branch = false;\n");
			printf ("\t\tif(regs.t0) check_t0_trace();\n");
			printf ("\t}\n");
		}
		break;
	case i_FScc:
		fpulimit();
		genamode (curi, curi->smode, "srcreg", curi->size, "extra", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tfpuop_scc (opcode, extra);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			need_endlabel = 1;
		}
		break;
	case i_FTRAPcc:
		fpulimit();
		printf ("\tuaecptr oldpc = %s;\n", getpc);
		printf ("\tuae_u16 extra = %s;\n", gen_nextiword (0));
		if (curi->smode != am_unknown && curi->smode != am_illg)
			genamode (curi, curi->smode, "srcreg", curi->size, "dummy", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tfpuop_trapcc (opcode, oldpc, extra);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			need_endlabel = 1;
		}
		break;
	case i_FBcc:
		fpulimit();
		sync_m68k_pc ();
		start_brace ();
		printf ("\tuaecptr pc = %s;\n", getpc);
		genamode (curi, curi->dmode, "srcreg", curi->size, "extra", 1, 0, 0);
		sync_m68k_pc ();
		printf ("\tfpuop_bcc (opcode, pc,extra);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			printf ("\tif (regs.fp_branch) {\n");
			printf ("\t\tregs.fp_branch = false;\n");
			printf ("\t\tfill_prefetch();\n");
			printf ("\t\tgoto %s;\n", endlabelstr);
			printf ("\t}\n");
			need_endlabel = 1;
		} else {
			printf ("\tif (regs.fp_branch) {\n");
			printf ("\t\tregs.fp_branch = false;\n");
			printf ("\t\tif(regs.t0) check_t0_trace();\n");
			printf ("\t}\n");
		}
		break;
	case i_FSAVE:
		fpulimit();
		sync_m68k_pc ();
		printf ("\tfpuop_save (opcode);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			need_endlabel = 1;
		}
		break;
	case i_FRESTORE:
		fpulimit();
		sync_m68k_pc ();
		printf ("\tfpuop_restore (opcode);\n");
		if (using_prefetch) {
			printf ("\tif (regs.fp_exception) goto %s;\n", endlabelstr);
			need_endlabel = 1;
		}
		break;

	case i_CINVL:
	case i_CINVP:
	case i_CINVA:
	case i_CPUSHL:
	case i_CPUSHP:
	case i_CPUSHA:
		printf ("\tflush_cpu_caches_040(opcode);\n");
		printf ("\tif (opcode & 0x80)\n");
		printf ("\t\tflush_icache((opcode >> 6) & 3);\n");
		break;

	case i_MOVE16:
		{
			if ((opcode & 0xfff8) == 0xf620) {
				/* MOVE16 (Ax)+,(Ay)+ */
				printf ("\tuae_u32 v[4];\n");
				printf ("\tuaecptr mems = m68k_areg (regs, srcreg) & ~15, memd;\n");
				printf ("\tdstreg = (%s >> 12) & 7;\n", gen_nextiword (0));
				printf ("\tmemd = m68k_areg (regs, dstreg) & ~15;\n");
				printf ("\tv[0] = %s (mems);\n", srcl);
				printf ("\tv[1] = %s (mems + 4);\n", srcl);
				printf ("\tv[2] = %s (mems + 8);\n", srcl);
				printf ("\tv[3] = %s (mems + 12);\n", srcl);
				printf ("\t%s (memd , v[0]);\n", dstl);
				printf ("\t%s (memd + 4, v[1]);\n", dstl);
				printf ("\t%s (memd + 8, v[2]);\n", dstl);
				printf ("\t%s (memd + 12, v[3]);\n", dstl);
				printf ("\tif (srcreg != dstreg)\n");
				printf ("\t\tm68k_areg (regs, srcreg) += 16;\n");
				printf ("\tm68k_areg (regs, dstreg) += 16;\n");
			} else {
				/* Other variants */
				printf ("\tuae_u32 v[4];\n");
				genamode (curi, curi->smode, "srcreg", curi->size, "mems", 0, 2, 0);
				genamode (curi, curi->dmode, "dstreg", curi->size, "memd", 0, 2, 0);
				printf ("\tmemsa &= ~15;\n");
				printf ("\tmemda &= ~15;\n");
				printf ("\tv[0] = %s (memsa);\n", srcl);
				printf ("\tv[1] = %s (memsa + 4);\n", srcl);
				printf ("\tv[2] = %s (memsa + 8);\n", srcl);
				printf ("\tv[3] = %s (memsa + 12);\n", srcl);
				printf ("\t%s (memda , v[0]);\n", dstl);
				printf ("\t%s (memda + 4, v[1]);\n", dstl);
				printf ("\t%s (memda + 8, v[2]);\n", dstl);
				printf ("\t%s (memda + 12, v[3]);\n", dstl);
				if ((opcode & 0xfff8) == 0xf600)
					printf ("\tm68k_areg (regs, srcreg) += 16;\n");
				else if ((opcode & 0xfff8) == 0xf608)
					printf ("\tm68k_areg (regs, dstreg) += 16;\n");
			}
		}
		break;

	case i_PFLUSHN:
	case i_PFLUSH:
	case i_PFLUSHAN:
	case i_PFLUSHA:
	case i_PLPAR:
	case i_PLPAW:
	case i_PTESTR:
	case i_PTESTW:
		sync_m68k_pc ();
		printf ("\tmmu_op (opcode, 0);\n");
		break;
	case i_MMUOP030:
		printf ("\tuaecptr pc = %s;\n", getpc);
		printf ("\tuae_u16 extra = %s (2);\n", prefetch_word);
		m68k_pc_offset += 2;
		sync_m68k_pc ();
		if (curi->smode == Areg || curi->smode == Dreg)
			printf("\tuae_u16 extraa = 0;\n");
		else
			genamode (curi, curi->smode, "srcreg", curi->size, "extra", 0, 0, 0);
		sync_m68k_pc ();
		printf ("\tmmu_op30 (pc, opcode, extra, extraa);\n");
		break;
	default:
		term ();
		break;
	}
end:
	finish_braces ();
	if (limit_braces) {
		printf ("\n#endif\n");
		n_braces = limit_braces;
		limit_braces = 0;
		finish_braces ();
	}
	if (did_prefetch >= 0)
		fill_prefetch_finish ();
	sync_m68k_pc ();
	did_prefetch = 0;
	if (cpu_level >= 2) {
		int v = curi->clocks;
		if (v < 4)
			v = 4;
		count_cycles = insn_n_cycles = v;
	}
}

static void generate_includes (FILE * f, int id)
{
	fprintf (f, "#include \"sysconfig.h\"\n");
	fprintf (f, "#include \"sysdeps.h\"\n");
	fprintf (f, "#include \"options.h\"\n");
	fprintf (f, "#include \"memory.h\"\n");
	fprintf (f, "#include \"custom.h\"\n");
	fprintf (f, "#include \"newcpu.h\"\n");
	fprintf (f, "#include \"cpu_prefetch.h\"\n");
	fprintf (f, "#include \"cputbl.h\"\n");

	fprintf (f, "#define CPUFUNC(x) x##_ff\n"
		"#define SET_CFLG_ALWAYS(x) SET_CFLG(x)\n"
		"#define SET_NFLG_ALWAYS(x) SET_NFLG(x)\n"
		"#ifdef NOFLAGS\n"
		"#include \"noflags.h\"\n"
		"#endif\n");
}

static int postfix;


static char *decodeEA (amodes mode, wordsizes size)
{
	static char buffer[80];

	buffer[0] = 0;
	switch (mode){
	case Dreg:
		strcpy (buffer,"Dn");
		break;
	case Areg:
		strcpy (buffer,"An");
		break;
	case Aind:
		strcpy (buffer,"(An)");
		break;
	case Aipi:
		strcpy (buffer,"(An)+");
		break;
	case Apdi:
		strcpy (buffer,"-(An)");
		break;
	case Ad16:
		strcpy (buffer,"(d16,An)");
		break;
	case Ad8r:
		strcpy (buffer,"(d8,An,Xn)");
		break;
	case PC16:
		strcpy (buffer,"(d16,PC)");
		break;
	case PC8r:
		strcpy (buffer,"(d8,PC,Xn)");
		break;
	case absw:
		strcpy (buffer,"(xxx).W");
		break;
	case absl:
		strcpy (buffer,"(xxx).L");
		break;
	case imm:
		switch (size){
		case sz_byte:
			strcpy (buffer,"#<data>.B");
			break;
		case sz_word:
			strcpy (buffer,"#<data>.W");
			break;
		case sz_long:
			strcpy (buffer,"#<data>.L");
			break;
		default:
			break;
		}
		break;
	case imm0:
		strcpy (buffer,"#<data>.B");
		break;
	case imm1:
		strcpy (buffer,"#<data>.W");
		break;
	case imm2:
		strcpy (buffer,"#<data>.L");
		break;
	case immi:
		strcpy (buffer,"#<data>");
		break;

	default:
		break;
	}
	return buffer;
}

static const char *m68k_cc[] = {
	"T",
	"F",
	"HI",
	"LS",
	"CC",
	"CS",
	"NE",
	"EQ",
	"VC",
	"VS",
	"PL",
	"MI",
	"GE",
	"LT",
	"GT",
	"LE"
};

static char *outopcode (int opcode)
{
	static char out[100];
	struct instr *ins;
	int i;

	ins = &table68k[opcode];
	for (i = 0; lookuptab[i].name[0]; i++) {
		if (ins->mnemo == lookuptab[i].mnemo)
			break;
	}
	{
		char *s = ua (lookuptab[i].name);
		strcpy (out, s);
		xfree (s);
	}
	if (ins->smode == immi)
		strcat (out, "Q");
	if (ins->size == sz_byte)
		strcat (out,".B");
	if (ins->size == sz_word)
		strcat (out,".W");
	if (ins->size == sz_long)
		strcat (out,".L");
	strcat (out," ");
	if (ins->suse)
		strcat (out, decodeEA (ins->smode, ins->size));
	if (ins->duse) {
		if (ins->suse) strcat (out,",");
		strcat (out, decodeEA (ins->dmode, ins->size));
	}
	if (ins->mnemo == i_DBcc || ins->mnemo == i_Scc || ins->mnemo == i_Bcc || ins->mnemo == i_TRAPcc) {
		strcat (out, " (");
		strcat (out, m68k_cc[table68k[opcode].cc]);
		strcat (out, ")");
	}

	return out;
}

struct cputbl_tmp
{
	uae_s16 length;
	uae_s8 disp020[2];
	uae_u8 branch;
};
static struct cputbl_tmp cputbltmp[65536];

static int count_required(int opcode)
{
  struct instr *curi = table68k + opcode;

  switch(curi->mnemo) {
    case i_MVMLE:
    case i_MVMEL:
      return 1;
  	case i_ASR:
  	case i_ASL:
  	case i_LSR:
  	case i_LSL:
    case i_ROXR:
    case i_ROR:
    case i_ROXL:
    case i_ROL:
    case i_MULU:
    case i_MULS:
    case i_DIVU:
    case i_DIVS:
    case i_DBcc:
      if(cpu_level <= 1)
      	return 1;
      break;
    case i_Scc:
      if(curi->smode == Dreg && cpu_level <= 1)
        return 1;
      break;
  }
  return 0;
}

static void generate_one_opcode (int rp, const char *extra)
{
	int idx;
	uae_u16 smsk, dmsk;
	unsigned int opcode = opcode_map[rp];
	int i68000 = table68k[opcode].clev > 0;

	if (table68k[opcode].mnemo == i_ILLG
		|| table68k[opcode].clev > cpu_level)
		return;

	for (idx = 0; lookuptab[idx].name[0]; idx++) {
		if (table68k[opcode].mnemo == lookuptab[idx].mnemo)
			break;
	}

	if (table68k[opcode].handler != -1)
		return;

	if (opcode_next_clev[rp] != cpu_level) {
		char *name = ua (lookuptab[idx].name);
		if (generate_stbl)
			fprintf (stblfile, "{ CPUFUNC(op_%04x_%d%s), 0x%04x, %d, { %d, %d }, %d }, /* %s */\n",
			opcode, opcode_last_postfix[rp],
				extra, opcode,
				cputbltmp[opcode].length, cputbltmp[opcode].disp020[0], cputbltmp[opcode].disp020[1], cputbltmp[opcode].branch, name);
		xfree (name);
		return;
	}
	fprintf (headerfile, "extern cpuop_func op_%04x_%d%s_nf;\n", opcode, postfix, extra);
	fprintf (headerfile, "extern cpuop_func op_%04x_%d%s_ff;\n", opcode, postfix, extra);
	printf ("/* %s */\n", outopcode (opcode));
	printf ("uae_u32 REGPARAM2 CPUFUNC(op_%04x_%d%s)(uae_u32 opcode)\n{\n", opcode, postfix, extra);
  int org_using_simple_cycles = using_simple_cycles;
  if(count_required(opcode))
    using_simple_cycles = 1;
	if (using_simple_cycles)
		printf("\tint count_cycles = 0;\n");

	switch (table68k[opcode].stype) {
	case 0: smsk = 7; break;
	case 1: smsk = 255; break;
	case 2: smsk = 15; break;
	case 3: smsk = 7; break;
	case 4: smsk = 7; break;
	case 5: smsk = 63; break;
  case 6: smsk = 255; break;
	case 7: smsk = 3; break;
	default: term ();
	}
	dmsk = 7;

	next_cpu_level = -1;
	if (table68k[opcode].suse
		&& table68k[opcode].smode != imm && table68k[opcode].smode != imm0
		&& table68k[opcode].smode != imm1 && table68k[opcode].smode != imm2
		&& table68k[opcode].smode != absw && table68k[opcode].smode != absl
		&& table68k[opcode].smode != PC8r && table68k[opcode].smode != PC16
	/* gb-- We don't want to fetch the EmulOp code since the EmulOp()
	   routine uses the whole opcode value. Maybe all the EmulOps
	   could be expanded out but I don't think it is an improvement */
	&& table68k[opcode].stype != 6
	)
	{
		if (table68k[opcode].spos == -1) {
			if (((int) table68k[opcode].sreg) >= 128)
				printf ("\tuae_u32 srcreg = (uae_s32)(uae_s8)%d;\n", (int) table68k[opcode].sreg);
			else
				printf ("\tuae_u32 srcreg = %d;\n", (int) table68k[opcode].sreg);
		} else {
			char source[100];
			int pos = table68k[opcode].spos;

			if (pos)
				sprintf (source, "((opcode >> %d) & %d)", pos, smsk);
			else
				sprintf (source, "(opcode & %d)", smsk);

			if (table68k[opcode].stype == 3)
				printf ("\tuae_u32 srcreg = imm8_table[%s];\n", source);
			else if (table68k[opcode].stype == 1)
				printf ("\tuae_u32 srcreg = (uae_s32)(uae_s8)%s;\n", source);
			else
				printf ("\tuae_u32 srcreg = %s;\n", source);
		}
	}
	if (table68k[opcode].duse
		/* Yes, the dmode can be imm, in case of LINK or DBcc */
		&& table68k[opcode].dmode != imm && table68k[opcode].dmode != imm0
		&& table68k[opcode].dmode != imm1 && table68k[opcode].dmode != imm2
		&& table68k[opcode].dmode != absw && table68k[opcode].dmode != absl)
	{
		if (table68k[opcode].dpos == -1) {
			if (((int) table68k[opcode].dreg) >= 128)
				printf ("\tuae_u32 dstreg = (uae_s32)(uae_s8)%d;\n", (int) table68k[opcode].dreg);
			else
				printf ("\tuae_u32 dstreg = %d;\n", (int) table68k[opcode].dreg);
		} else {
			int pos = table68k[opcode].dpos;
			if (pos)
				printf ("\tuae_u32 dstreg = (opcode >> %d) & %d;\n",
				pos, dmsk);
			else
				printf ("\tuae_u32 dstreg = opcode & %d;\n", dmsk);
		}
	}
	need_endlabel = 0;
	endlabelno++;
	sprintf (endlabelstr, "l_%d", endlabelno);
	count_read = count_write = count_ncycles = count_cycles = 0;
	count_read_ea = count_write_ea = count_cycles_ea = 0;
	gen_opcode (opcode);
	if (need_endlabel)
		printf ("%s: ;\n", endlabelstr);
	if (using_prefetch) {
		if (count_read + count_write + count_cycles == 0)
			count_cycles = 4;
		returncycles ("", (count_read + count_write) * 4 + count_cycles);
		printf ("}");
		printf (" /* %d%s (%d/%d)",
			(count_read + count_write) * 4 + count_cycles, count_ncycles ? "+" : "", count_read, count_write);
		printf (" */\n");
	} else if (count_read + count_write + count_cycles) {
		returncycles ("", (count_read + count_write) * 4 + count_cycles + 4);
		printf ("}");
		printf("\n");
	} else {
		returncycles ("", insn_n_cycles);
		printf ("}");
		printf("\n");
	}
	printf ("\n");
	using_simple_cycles = org_using_simple_cycles;

	opcode_next_clev[rp] = next_cpu_level;
	opcode_last_postfix[rp] = postfix;

	if ((opcode & 0xf000) == 0xf000)
		m68k_pc_total = -1;
	cputbltmp[opcode].length = m68k_pc_total;

	cputbltmp[opcode].disp020[0] = 0;
	if (genamode8r_offset[0] > 0)
		cputbltmp[opcode].disp020[0] = m68k_pc_total - genamode8r_offset[0] + 2;
	cputbltmp[opcode].disp020[1] = 0;
	if (genamode8r_offset[1] > 0)
		cputbltmp[opcode].disp020[1] = m68k_pc_total - genamode8r_offset[1] + 2;

	cputbltmp[opcode].branch = branch_inst;

	if (generate_stbl) {
		char *name = ua (lookuptab[idx].name);
		fprintf (stblfile, "{ CPUFUNC(op_%04x_%d%s), 0x%04x, %d, { %d, %d }, %d }, /* %s */\n",
			opcode, postfix, extra, opcode,
			cputbltmp[opcode].length, cputbltmp[opcode].disp020[0], cputbltmp[opcode].disp020[1], cputbltmp[opcode].branch, name);
		xfree (name);
	}
}

static void generate_func (const char *extra)
{
	int j, rp;

	/* sam: this is for people with low memory (eg. me :)) */
	printf ("\n"
		"#if !defined(PART_1) && !defined(PART_2) && "
		"!defined(PART_3) && !defined(PART_4) && "
		"!defined(PART_5) && !defined(PART_6) && "
		"!defined(PART_7) && !defined(PART_8)"
		"\n"
		"#define PART_1 1\n"
		"#define PART_2 1\n"
		"#define PART_3 1\n"
		"#define PART_4 1\n"
		"#define PART_5 1\n"
		"#define PART_6 1\n"
		"#define PART_7 1\n"
		"#define PART_8 1\n"
		"#endif\n\n");

	rp = 0;
	for(j = 1; j <= 8; ++j) {
		int k = (j * nr_cpuop_funcs) / 8;
		printf ("#ifdef PART_%d\n",j);
		for (; rp < k; rp++)
			generate_one_opcode (rp, extra);
		printf ("#endif\n\n");
	}

	if (generate_stbl)
		fprintf (stblfile, "{ 0, 0 }};\n");
}

static void generate_cpu (int id, int mode)
{
	char fname[100];
	const char *extra, *extraup;
	static int postfix2 = -1;
	int rp;

	extra = "";
	extraup = "";

	postfix = id;
	if (id == 0 || id == 4 || id == 11 || id == 40 || id == 44) {
		if (generate_stbl && id != 4 && id != 44)
			fprintf (stblfile, "#ifdef CPUEMU_%d%s\n", postfix, extraup);
		postfix2 = postfix;
		sprintf (fname, "cpuemu_%d%s.cpp", postfix, extra);
		if (freopen (fname, "wb", stdout) == NULL) {
			abort ();
		}
		generate_includes (stdout, id);
	}

	using_exception_3 = 1;
	using_prefetch = 0;
	using_simple_cycles = 0;
	using_indirect = 0;
	cpu_generic = false;

	if (id == 11 || id == 12) { // 11 = 68010 prefetch, 12 = 68000 prefetch
		cpu_level = id == 11 ? 1 : 0;
		using_prefetch = 1;
		using_exception_3 = 1;
		using_simple_cycles = 1;
		if (id == 11) {
			read_counts ();
			for (rp = 0; rp < nr_cpuop_funcs; rp++)
				opcode_next_clev[rp] = cpu_level;
		}
	} else if (id < 6) {
		cpu_level = 5 - (id - 0); // "generic"
		cpu_generic = true;
	} else if (id >= 40 && id < 46) {
		cpu_level = 5 - (id - 40); // "generic" + direct
		cpu_generic = true;
		if (id == 40) {
				read_counts();
			for (rp = 0; rp < nr_cpuop_funcs; rp++)
				opcode_next_clev[rp] = cpu_level;
		}
		using_indirect = -1;
	}
	if (id == 4 || id == 44) {
		cpu_level = 1;
		for (rp = 0; rp < nr_cpuop_funcs; rp++) {
			opcode_next_clev[rp] = cpu_level;
		}
	}

	if (generate_stbl) {
		fprintf (stblfile, "const struct cputbl CPUFUNC(op_smalltbl_%d%s)[] = {\n", postfix, extra);
	}
	endlabelno = id * 10000;
	generate_func (extra);
	if (generate_stbl) {
		if (postfix2 >= 0 && postfix2 != 4 && postfix2 != 44)
			fprintf (stblfile, "#endif /* CPUEMU_%d%s */\n", postfix2, extraup);
	}
	postfix2 = -1;
}

int main(int argc, char *argv[])
{
	int i;

	read_table68k ();
	do_merges ();

	opcode_map =  xmalloc (int, nr_cpuop_funcs);
	opcode_last_postfix = xmalloc (int, nr_cpuop_funcs);
	opcode_next_clev = xmalloc (int, nr_cpuop_funcs);
	counts = xmalloc (unsigned long, 65536);
	read_counts ();

	/* It would be a lot nicer to put all in one file (we'd also get rid of
	* cputbl.h that way), but cpuopti can't cope.  That could be fixed, but
	* I don't dare to touch the 68k version.  */

	headerfile = fopen ("cputbl.h", "wb");

	stblfile = fopen ("cpustbl.cpp", "wb");
	generate_includes (stblfile, 0);

	for (i = 0; i <= 45; i++) {
		if ((i >= 6 && i < 11) || (i > 12 && i < 40))
			continue;
		generate_stbl = 1;
		generate_cpu (i, 0);
	}

	free (table68k);
  fclose(headerfile);
  fclose(stblfile);
	return 0;
}

void write_log (const TCHAR *format,...)
{
}
