/*> interp.c <*/
/* Simulator for the MIPS architecture.

   This file is part of the MIPS sim

		THIS SOFTWARE IS NOT COPYRIGHTED

   Cygnus offers the following for use in the public domain.  Cygnus
   makes no warranty with regard to the software or it's performance
   and the user accepts the software "AS IS" with all faults.

   CYGNUS DISCLAIMS ANY WARRANTIES, EXPRESS OR IMPLIED, WITH REGARD TO
   THIS SOFTWARE INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

   $Revision$
     $Author$
       $Date$             

NOTEs:

We only need to take account of the target endianness when moving data
between the simulator and the host. We do not need to worry about the
endianness of the host, since this sim code and GDB are executing in
the same process.

The IDT monitor (found on the VR4300 board), seems to lie about
register contents. It seems to treat the registers as sign-extended
32-bit values. This cause *REAL* problems when single-stepping 64-bit
code on the hardware.

*/

/* The TRACE and PROFILE manifests enable the provision of extra
   features. If they are not defined then a simpler (quicker)
   simulator is constructed without the required run-time checks,
   etc. */
#if 1 /* 0 to allow user build selection, 1 to force inclusion */
#define TRACE (1)
#define PROFILE (1)
#endif

#include "bfd.h"
#include "sim-main.h"
#include "sim-utils.h"
#include "sim-options.h"
#include "sim-assert.h"

#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <ansidecl.h>
#include <signal.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#endif

#include "getopt.h"
#include "libiberty.h"
#include "bfd.h"
#include "callback.h"   /* GDB simulator callback interface */
#include "remote-sim.h" /* GDB simulator interface */

#include "support.h"    /* internal support manifests */

#include "sysdep.h"

#ifndef PARAMS
#define PARAMS(x) 
#endif

char* pr_addr PARAMS ((SIM_ADDR addr));
char* pr_uword64 PARAMS ((uword64 addr));

#ifndef SIGBUS
#define SIGBUS SIGSEGV
#endif

/* Get the simulator engine description, without including the code: */
#define SIM_MANIFESTS
#include "engine.c"
#undef SIM_MANIFESTS

struct sim_state simulator;

/* The following reserved instruction value is used when a simulator
   trap is required. NOTE: Care must be taken, since this value may be
   used in later revisions of the MIPS ISA. */
#define RSVD_INSTRUCTION           (0x00000005)
#define RSVD_INSTRUCTION_MASK      (0xFC00003F)

#define RSVD_INSTRUCTION_ARG_SHIFT 6
#define RSVD_INSTRUCTION_ARG_MASK  0xFFFFF  


/* NOTE: These numbers depend on the processor architecture being
   simulated: */
#define Interrupt               (0)
#define TLBModification         (1)
#define TLBLoad                 (2)
#define TLBStore                (3)
#define AddressLoad             (4)
#define AddressStore            (5)
#define InstructionFetch        (6)
#define DataReference           (7)
#define SystemCall              (8)
#define BreakPoint              (9)
#define ReservedInstruction     (10)
#define CoProcessorUnusable     (11)
#define IntegerOverflow         (12)    /* Arithmetic overflow (IDT monitor raises SIGFPE) */
#define Trap                    (13)
#define FPE                     (15)
#define Watch                   (23)

/* The following exception code is actually private to the simulator
   world. It is *NOT* a processor feature, and is used to signal
   run-time errors in the simulator. */
#define SimulatorFault      (0xFFFFFFFF)

/* The following are generic to all versions of the MIPS architecture
   to date: */
/* Memory Access Types (for CCA): */
#define Uncached                (0)
#define CachedNoncoherent       (1)
#define CachedCoherent          (2)
#define Cached                  (3)

#define isINSTRUCTION   (1 == 0) /* FALSE */
#define isDATA          (1 == 1) /* TRUE */

#define isLOAD          (1 == 0) /* FALSE */
#define isSTORE         (1 == 1) /* TRUE */

#define isREAL          (1 == 0) /* FALSE */
#define isRAW           (1 == 1) /* TRUE */

#define isTARGET        (1 == 0) /* FALSE */
#define isHOST          (1 == 1) /* TRUE */

/* The "AccessLength" specifications for Loads and Stores. NOTE: This
   is the number of bytes minus 1. */
#define AccessLength_BYTE       (0)
#define AccessLength_HALFWORD   (1)
#define AccessLength_TRIPLEBYTE (2)
#define AccessLength_WORD       (3)
#define AccessLength_QUINTIBYTE (4)
#define AccessLength_SEXTIBYTE  (5)
#define AccessLength_SEPTIBYTE  (6)
#define AccessLength_DOUBLEWORD (7)
#define AccessLength_QUADWORD   (15)

#if defined(HASFPU)
/* FPU registers must be one of the following types. All other values
   are reserved (and undefined). */
typedef enum {
 fmt_single  = 0,
 fmt_double  = 1,
 fmt_word    = 4,
 fmt_long    = 5,
 /* The following are well outside the normal acceptable format
    range, and are used in the register status vector. */
 fmt_unknown       = 0x10000000,
 fmt_uninterpreted = 0x20000000,
} FP_formats;
#endif /* HASFPU */

/* NOTE: We cannot avoid globals, since the GDB "sim_" interface does
   not allow a private variable to be passed around. This means that
   simulators under GDB can only be single-threaded. However, it would
   be possible for the simulators to be multi-threaded if GDB allowed
   for a private pointer to be maintained. i.e. a general "void **ptr"
   variable that GDB passed around in the argument list to all of
   sim_xxx() routines. It could be initialised to NULL by GDB, and
   then updated by sim_open() and used by the other sim_xxx() support
   functions. This would allow new features in the simulator world,
   like storing a context - continuing execution to gather a result,
   and then going back to the point where the context was saved and
   changing some state before continuing. i.e. the ability to perform
   UNDOs on simulations. It would also allow the simulation of
   shared-memory multi-processor systems.

   [NOTE: This is now partially implemented] */

static host_callback *callback = NULL; /* handle onto the current callback structure */

/* This is nasty, since we have to rely on matching the register
   numbers used by GDB. Unfortunately, depending on the MIPS target
   GDB uses different register numbers. We cannot just include the
   relevant "gdb/tm.h" link, since GDB may not be configured before
   the sim world, and also the GDB header file requires too much other
   state. */
/* TODO: Sort out a scheme for *KNOWING* the mapping between real
   registers, and the numbers that GDB uses. At the moment due to the
   order that the tools are built, we cannot rely on a configured GDB
   world whilst constructing the simulator. This means we have to
   assume the GDB register number mapping. */
#ifndef TM_MIPS_H
#define LAST_EMBED_REGNUM (89)
#endif

/* To keep this default simulator simple, and fast, we use a direct
   vector of registers. The internal simulator engine then uses
   manifests to access the correct slot. */
static ut_reg registers[LAST_EMBED_REGNUM + 1];
static int register_widths[LAST_EMBED_REGNUM + 1];

#define GPR     (&registers[0])
#if defined(HASFPU)
#define FGRIDX  (38)
#define FGR     (&registers[FGRIDX])
#endif /* HASFPU */
#define LO      (registers[33])
#define HI      (registers[34])
#define PC      (registers[37])
#define CAUSE   (registers[36])
#define SRIDX   (32)
#define SR      (registers[SRIDX])      /* CPU status register */
#define FCR0IDX  (71)
#define FCR0    (registers[FCR0IDX])    /* really a 32bit register */
#define FCR31IDX (70)
#define FCR31   (registers[FCR31IDX])   /* really a 32bit register */
#define FCSR    (FCR31)
#define COCIDX  (LAST_EMBED_REGNUM + 2) /* special case : outside the normal range */

/* The following are pseudonyms for standard registers */
#define ZERO    (registers[0])
#define V0      (registers[2])
#define A0      (registers[4])
#define A1      (registers[5])
#define A2      (registers[6])
#define A3      (registers[7])
#define SP      (registers[29])
#define RA      (registers[31])


/* start-sanitize-r5900 */
/* 
The R5900 has 128 bit registers, but the hi 64 bits are only touched by 
multimedia (MMI) instructions.  The normal mips instructions just use the
lower 64 bits.  To avoid changing the older parts of the simulator to 
handle this weirdness, the high 64 bits of each register are kept in 
a separate array (registers1).  The high 64 bits of any register are by
convention refered by adding a '1' to the end of the normal register's 
name.  So LO still refers to the low 64 bits of the LO register, LO1
refers to the high 64 bits of that same register.
*/

/* The high part of each register */
static ut_reg registers1[LAST_EMBED_REGNUM + 1];

#define GPR1     (&registers1[0])

#define LO1      (registers1[33])
#define HI1      (registers1[34])

#define BYTES_IN_MMI_REGS       (sizeof(registers[0])+sizeof(registers1[0]))
#define HALFWORDS_IN_MMI_REGS   (BYTES_IN_MMI_REGS/2)
#define WORDS_IN_MMI_REGS       (BYTES_IN_MMI_REGS/4)
#define DOUBLEWORDS_IN_MMI_REGS (BYTES_IN_MMI_REGS/8)

#define BYTES_IN_MIPS_REGS       (sizeof(registers[0]))
#define HALFWORDS_IN_MIPS_REGS   (BYTES_IN_MIPS_REGS/2)
#define WORDS_IN_MIPS_REGS       (BYTES_IN_MIPS_REGS/4)
#define DOUBLEWORDS_IN_MIPS_REGS (BYTES_IN_MIPS_REGS/8)


/*
SUB_REG_FETCH - return as lvalue some sub-part of a "register"
   T  - type of the sub part
   TC - # of T's in the mips part of the "register"
   I  - index (from 0) of desired sub part
   A  - low part of "register"
   A1 - high part of register
*/
#define SUB_REG_FETCH(T,TC,A,A1,I) (*(((T*)(((I) < (TC)) ? (A) : (A1))) + ((I) % (TC))))

/* 
GPR_<type>(R,I) - return, as lvalue, the I'th <type> of general register R 
            where <type> has two letters:
                  1 is S=signed or U=unsigned
                  2 is B=byte H=halfword W=word D=doubleword 
*/

#define SUB_REG_SB(A,A1,I) SUB_REG_FETCH(signed char,       BYTES_IN_MIPS_REGS,       A, A1, I)
#define SUB_REG_SH(A,A1,I) SUB_REG_FETCH(signed short,      HALFWORDS_IN_MIPS_REGS,   A, A1, I)
#define SUB_REG_SW(A,A1,I) SUB_REG_FETCH(signed int,        WORDS_IN_MIPS_REGS,       A, A1, I)
#define SUB_REG_SD(A,A1,I) SUB_REG_FETCH(signed long long,  DOUBLEWORDS_IN_MIPS_REGS, A, A1, I)

#define SUB_REG_UB(A,A1,I) SUB_REG_FETCH(unsigned char,     BYTES_IN_MIPS_REGS,       A, A1, I)
#define SUB_REG_UH(A,A1,I) SUB_REG_FETCH(unsigned short,    HALFWORDS_IN_MIPS_REGS,   A, A1, I)
#define SUB_REG_UW(A,A1,I) SUB_REG_FETCH(unsigned int,      WORDS_IN_MIPS_REGS,       A, A1, I)
#define SUB_REG_UD(A,A1,I) SUB_REG_FETCH(unsigned long long,DOUBLEWORDS_IN_MIPS_REGS, A, A1, I)



#define GPR_SB(R,I) SUB_REG_SB(&registers[R], &registers1[R], I)
#define GPR_SH(R,I) SUB_REG_SH(&registers[R], &registers1[R], I)
#define GPR_SW(R,I) SUB_REG_SW(&registers[R], &registers1[R], I)
#define GPR_SD(R,I) SUB_REG_SD(&registers[R], &registers1[R], I)

#define GPR_UB(R,I) SUB_REG_UB(&registers[R], &registers1[R], I)
#define GPR_UH(R,I) SUB_REG_UH(&registers[R], &registers1[R], I)
#define GPR_UW(R,I) SUB_REG_UW(&registers[R], &registers1[R], I)
#define GPR_UD(R,I) SUB_REG_UD(&registers[R], &registers1[R], I)


#define RS_SB(I) SUB_REG_SB(&rs_reg, &rs_reg1, I)
#define RS_SH(I) SUB_REG_SH(&rs_reg, &rs_reg1, I)
#define RS_SW(I) SUB_REG_SW(&rs_reg, &rs_reg1, I)
#define RS_SD(I) SUB_REG_SD(&rs_reg, &rs_reg1, I)

#define RS_UB(I) SUB_REG_UB(&rs_reg, &rs_reg1, I)
#define RS_UH(I) SUB_REG_UH(&rs_reg, &rs_reg1, I)
#define RS_UW(I) SUB_REG_UW(&rs_reg, &rs_reg1, I)
#define RS_UD(I) SUB_REG_UD(&rs_reg, &rs_reg1, I)

#define RT_SB(I) SUB_REG_SB(&rt_reg, &rt_reg1, I)
#define RT_SH(I) SUB_REG_SH(&rt_reg, &rt_reg1, I)
#define RT_SW(I) SUB_REG_SW(&rt_reg, &rt_reg1, I)
#define RT_SD(I) SUB_REG_SD(&rt_reg, &rt_reg1, I)

#define RT_UB(I) SUB_REG_UB(&rt_reg, &rt_reg1, I)
#define RT_UH(I) SUB_REG_UH(&rt_reg, &rt_reg1, I)
#define RT_UW(I) SUB_REG_UW(&rt_reg, &rt_reg1, I)
#define RT_UD(I) SUB_REG_UD(&rt_reg, &rt_reg1, I)



#define LO_SB(I) SUB_REG_SB(&LO, &LO1, I)
#define LO_SH(I) SUB_REG_SH(&LO, &LO1, I)
#define LO_SW(I) SUB_REG_SW(&LO, &LO1, I)
#define LO_SD(I) SUB_REG_SD(&LO, &LO1, I)

#define LO_UB(I) SUB_REG_UB(&LO, &LO1, I)
#define LO_UH(I) SUB_REG_UH(&LO, &LO1, I)
#define LO_UW(I) SUB_REG_UW(&LO, &LO1, I)
#define LO_UD(I) SUB_REG_UD(&LO, &LO1, I)

#define HI_SB(I) SUB_REG_SB(&HI, &HI1, I)
#define HI_SH(I) SUB_REG_SH(&HI, &HI1, I)
#define HI_SW(I) SUB_REG_SW(&HI, &HI1, I)
#define HI_SD(I) SUB_REG_SD(&HI, &HI1, I)

#define HI_UB(I) SUB_REG_UB(&HI, &HI1, I)
#define HI_UH(I) SUB_REG_UH(&HI, &HI1, I)
#define HI_UW(I) SUB_REG_UW(&HI, &HI1, I)
#define HI_UD(I) SUB_REG_UD(&HI, &HI1, I)
/* end-sanitize-r5900 */


/* start-sanitize-r5900 */
static ut_reg SA;        /* the shift amount register */
/* end-sanitize-r5900 */

static ut_reg EPC = 0; /* Exception PC */

#if defined(HASFPU)
/* Keep the current format state for each register: */
static FP_formats fpr_state[32];
#endif /* HASFPU */

/* The following are internal simulator state variables: */
static ut_reg IPC = 0; /* internal Instruction PC */
static ut_reg DSPC = 0;  /* delay-slot PC */


/* TODO : these should be the bitmasks for these bits within the
   status register. At the moment the following are VR4300
   bit-positions: */
#define status_KSU_mask  (0x3)          /* mask for KSU bits */
#define status_KSU_shift (3)            /* shift for field */
#define ksu_kernel       (0x0)
#define ksu_supervisor   (0x1)
#define ksu_user         (0x2)
#define ksu_unknown      (0x3)

#define status_IE	 (1 <<  0)      /* Interrupt enable */
#define status_EXL	 (1 <<  1)	/* Exception level */
#define status_RE        (1 << 25)      /* Reverse Endian in user mode */
#define status_FR        (1 << 26)      /* enables MIPS III additional FP registers */
#define status_SR        (1 << 20)      /* soft reset or NMI */
#define status_BEV       (1 << 22)      /* Location of general exception vectors */
#define status_TS        (1 << 21)      /* TLB shutdown has occurred */
#define status_ERL       (1 <<  2)      /* Error level */
#define status_RP        (1 << 27)      /* Reduced Power mode */

#define cause_BD        ((unsigned)1 << 31)     /* Exception in branch delay slot */

#if defined(HASFPU)
/* Macro to update FPSR condition-code field. This is complicated by
   the fact that there is a hole in the index range of the bits within
   the FCSR register. Also, the number of bits visible depends on the
   MIPS ISA version being supported. */
#define SETFCC(cc,v) {\
                    int bit = ((cc == 0) ? 23 : (24 + (cc)));\
                    FCSR = ((FCSR & ~(1 << bit)) | ((v) << bit));\
                  }
#define GETFCC(cc) (((((cc) == 0) ? (FCSR & (1 << 23)) : (FCSR & (1 << (24 + (cc))))) != 0) ? 1 : 0)

/* This should be the COC1 value at the start of the preceding
   instruction: */
#define PREVCOC1() ((state & simPCOC1) ? 1 : 0)
#endif /* HASFPU */

/* Standard FCRS bits: */
#define IR (0) /* Inexact Result */
#define UF (1) /* UnderFlow */
#define OF (2) /* OverFlow */
#define DZ (3) /* Division by Zero */
#define IO (4) /* Invalid Operation */
#define UO (5) /* Unimplemented Operation */

/* Get masks for individual flags: */
#if 1 /* SAFE version */
#define FP_FLAGS(b)  (((unsigned)(b) < 5) ? (1 << ((b) + 2)) : 0)
#define FP_ENABLE(b) (((unsigned)(b) < 5) ? (1 << ((b) + 7)) : 0)
#define FP_CAUSE(b)  (((unsigned)(b) < 6) ? (1 << ((b) + 12)) : 0)
#else
#define FP_FLAGS(b)  (1 << ((b) + 2))
#define FP_ENABLE(b) (1 << ((b) + 7))
#define FP_CAUSE(b)  (1 << ((b) + 12))
#endif

#define FP_FS         (1 << 24) /* MIPS III onwards : Flush to Zero */

#define FP_MASK_RM    (0x3)
#define FP_SH_RM      (0)
#define FP_RM_NEAREST (0) /* Round to nearest        (Round) */
#define FP_RM_TOZERO  (1) /* Round to zero           (Trunc) */
#define FP_RM_TOPINF  (2) /* Round to Plus infinity  (Ceil) */
#define FP_RM_TOMINF  (3) /* Round to Minus infinity (Floor) */
#define GETRM()       (int)((FCSR >> FP_SH_RM) & FP_MASK_RM)

/* Slots for delayed register updates. For the moment we just have a
   fixed number of slots (rather than a more generic, dynamic
   system). This keeps the simulator fast. However, we only allow for
   the register update to be delayed for a single instruction
   cycle. */
#define PSLOTS (5) /* Maximum number of instruction cycles */
static int    pending_in;
static int    pending_out;
static int    pending_total;
static int    pending_slot_count[PSLOTS];
static int    pending_slot_reg[PSLOTS];
static ut_reg pending_slot_value[PSLOTS];

/*---------------------------------------------------------------------------*/
/*-- GDB simulator interface ------------------------------------------------*/
/*---------------------------------------------------------------------------*/

static void dotrace PARAMS((FILE *tracefh,int type,SIM_ADDR address,int width,char *comment,...));
static void sim_warning PARAMS((char *fmt,...));
extern void sim_error PARAMS((char *fmt,...));
static void ColdReset PARAMS((void));
static int AddressTranslation PARAMS((uword64 vAddr,int IorD,int LorS,uword64 *pAddr,int *CCA,int host,int raw));
static void StoreMemory PARAMS((int CCA,int AccessLength,uword64 MemElem,uword64 MemElem1,uword64 pAddr,uword64 vAddr,int raw));
static void LoadMemory PARAMS((uword64*memvalp,uword64*memval1p,int CCA,int AccessLength,uword64 pAddr,uword64 vAddr,int IorD,int raw));
static void SignalException PARAMS((int exception,...));
static long getnum PARAMS((char *value));
extern void sim_set_profile PARAMS((int frequency));
static unsigned int power2 PARAMS((unsigned int value));

/*---------------------------------------------------------------------------*/

/* The following are not used for MIPS IV onwards: */
#define PENDING_FILL(r,v) {\
/* printf("DBG: FILL BEFORE pending_in = %d, pending_out = %d, pending_total = %d\n",pending_in,pending_out,pending_total); */\
                            if (pending_slot_reg[pending_in] != (LAST_EMBED_REGNUM + 1))\
                             sim_warning("Attempt to over-write pending value");\
                            pending_slot_count[pending_in] = 2;\
                            pending_slot_reg[pending_in] = (r);\
                            pending_slot_value[pending_in] = (uword64)(v);\
/*printf("DBG: FILL        reg %d value = 0x%s\n",(r),pr_addr(v));*/\
                            pending_total++;\
                            pending_in++;\
                            if (pending_in == PSLOTS)\
                             pending_in = 0;\
/*printf("DBG: FILL AFTER  pending_in = %d, pending_out = %d, pending_total = %d\n",pending_in,pending_out,pending_total);*/\
                          }

static int LLBIT = 0;
/* LLBIT = Load-Linked bit. A bit of "virtual" state used by atomic
   read-write instructions. It is set when a linked load occurs. It is
   tested and cleared by the conditional store. It is cleared (during
   other CPU operations) when a store to the location would no longer
   be atomic. In particular, it is cleared by exception return
   instructions. */

static int HIACCESS = 0;
static int LOACCESS = 0;
static int HI1ACCESS = 0;
static int LO1ACCESS = 0;

/* ??? The 4300 and a few other processors have interlocks on hi/lo register
   reads, and hence do not have this problem.  To avoid spurious warnings,
   we just disable this always.  */
#if 1
#define CHECKHILO(s)
#else
/* The HIACCESS and LOACCESS counts are used to ensure that
   corruptions caused by using the HI or LO register to close to a
   following operation are spotted. */
static ut_reg HLPC = 0;
/* If either of the preceding two instructions have accessed the HI or
   LO registers, then the values they see should be
   undefined. However, to keep the simulator world simple, we just let
   them use the value read and raise a warning to notify the user: */
#define CHECKHILO(s)    {\
                          if ((HIACCESS != 0) || (LOACCESS != 0) || (HI1ACCESS != 0) || (LO1ACCESS != 0))\
                            sim_warning("%s over-writing HI and LO registers values (PC = 0x%s HLPC = 0x%s)\n",(s),pr_addr(PC),pr_addr(HLPC));\
                        }
#endif

/* NOTE: We keep the following status flags as bit values (1 for true,
   0 for false). This allows them to be used in binary boolean
   operations without worrying about what exactly the non-zero true
   value is. */

/* UserMode */
#define UserMode        ((((SR & status_KSU_mask) >> status_KSU_shift) == ksu_user) ? 1 : 0)

/* BigEndianMem */
/* Hardware configuration. Affects endianness of LoadMemory and
   StoreMemory and the endianness of Kernel and Supervisor mode
   execution. The value is 0 for little-endian; 1 for big-endian. */
#define BigEndianMem    (CURRENT_TARGET_BYTE_ORDER == BIG_ENDIAN)
/*(state & simBE) ? 1 : 0)*/

/* ByteSwapMem */
/* This is true if the host and target have different endianness.  */
#define ByteSwapMem (CURRENT_TARGET_BYTE_ORDER != CURRENT_HOST_BYTE_ORDER)

/* ReverseEndian */
/* This mode is selected if in User mode with the RE bit being set in
   SR (Status Register). It reverses the endianness of load and store
   instructions. */
#define ReverseEndian   (((SR & status_RE) && UserMode) ? 1 : 0)

/* BigEndianCPU */
/* The endianness for load and store instructions (0=little;1=big). In
   User mode this endianness may be switched by setting the state_RE
   bit in the SR register. Thus, BigEndianCPU may be computed as
   (BigEndianMem EOR ReverseEndian). */
#define BigEndianCPU    (BigEndianMem ^ ReverseEndian) /* Already bits */

#if !defined(FASTSIM) || defined(PROFILE)
/* At the moment these values will be the same, since we do not have
   access to the pipeline cycle count information from the simulator
   engine. */
static unsigned int instruction_fetches = 0;
static unsigned int instruction_fetch_overflow = 0;
#endif

/* Flags in the "state" variable: */
#define simHALTEX       (1 << 2)  /* 0 = run; 1 = halt on exception */
#define simHALTIN       (1 << 3)  /* 0 = run; 1 = halt on interrupt */
#define simTRACE        (1 << 8)  /* 0 = do nothing; 1 = trace address activity */
#define simPROFILE      (1 << 9)  /* 0 = do nothing; 1 = gather profiling samples */
#define simPCOC0        (1 << 17) /* COC[1] from current */
#define simPCOC1        (1 << 18) /* COC[1] from previous */
#define simDELAYSLOT    (1 << 24) /* 0 = do nothing; 1 = delay slot entry exists */
#define simSKIPNEXT     (1 << 25) /* 0 = do nothing; 1 = skip instruction */
#define simSIGINT	(1 << 28)  /* 0 = do nothing; 1 = SIGINT has occured */
#define simJALDELAYSLOT	(1 << 29) /* 1 = in jal delay slot */

static unsigned int state = 0;

#define DELAYSLOT()     {\
                          if (state & simDELAYSLOT)\
                            sim_warning("Delay slot already activated (branch in delay slot?)");\
                          state |= simDELAYSLOT;\
                        }

#define JALDELAYSLOT()	{\
			  DELAYSLOT ();\
			  state |= simJALDELAYSLOT;\
			}

#define NULLIFY()       {\
                          state &= ~simDELAYSLOT;\
                          state |= simSKIPNEXT;\
                        }

#define INDELAYSLOT()	((state & simDELAYSLOT) != 0)
#define INJALDELAYSLOT() ((state & simJALDELAYSLOT) != 0)

#define K0BASE  (0x80000000)
#define K0SIZE  (0x20000000)
#define K1BASE  (0xA0000000)
#define K1SIZE  (0x20000000)

/* Simple run-time monitor support */
static unsigned char *monitor = NULL;
static ut_reg monitor_base = 0xBFC00000;
static unsigned monitor_size = (1 << 11); /* power-of-2 */

static char *logfile = NULL; /* logging disabled by default */
static FILE *logfh = NULL;

#if defined(TRACE)
static char *tracefile = "trace.din"; /* default filename for trace log */
static FILE *tracefh = NULL;
static void open_trace PARAMS((void));
#endif /* TRACE */

#if defined(PROFILE)
static unsigned profile_frequency = 256;
static unsigned profile_nsamples = (128 << 10);
static unsigned short *profile_hist = NULL;
static ut_reg profile_minpc;
static ut_reg profile_maxpc;
static int profile_shift = 0; /* address shift amount */
#endif /* PROFILE */


static SIM_RC
mips_option_handler (sd, opt, arg)
     SIM_DESC sd;
     int opt;
     char *arg;
{
  switch (opt)
    {
    case 'l':
      if (arg != NULL) {
	char *tmp;
	tmp = (char *)malloc(strlen(arg) + 1);
	if (tmp == NULL)
	  callback->printf_filtered(callback,"Failed to allocate buffer for logfile name \"%s\"\n",optarg);
	else {
	  strcpy(tmp,optarg);
	  logfile = tmp;
	}
      }
      return SIM_RC_OK;

    case 'n': /* OK */
      callback->printf_filtered(callback,"Explicit model selection not yet available (Ignoring \"%s\")\n",optarg);
      return SIM_RC_FAIL;

    case 't': /* ??? */
#if defined(TRACE)
      /* Eventually the simTRACE flag could be treated as a toggle, to
	 allow external control of the program points being traced
	 (i.e. only from main onwards, excluding the run-time setup,
	 etc.). */
      if (arg == NULL)
	state |= simTRACE;
      else if (strcmp (arg, "yes") == 0)
	state |= simTRACE;
      else if (strcmp (arg, "no") == 0)
	state &= ~simTRACE;
      else
	{
	  fprintf (stderr, "Unreconized trace option `%s'\n", arg);
	  return SIM_RC_FAIL;
	}
      return SIM_RC_OK;
#else /* !TRACE */
      fprintf(stderr,"\
Simulator constructed without tracing support (for performance).\n\
Re-compile simulator with \"-DTRACE\" to enable this option.\n");
      return SIM_RC_FAIL;
#endif /* !TRACE */

    case 'z':
#if defined(TRACE)
      if (optarg != NULL) {
	char *tmp;
	tmp = (char *)malloc(strlen(optarg) + 1);
	if (tmp == NULL)
	  {
	    callback->printf_filtered(callback,"Failed to allocate buffer for tracefile name \"%s\"\n",optarg);
	    return SIM_RC_FAIL;
	  }
	else {
	  strcpy(tmp,optarg);
	  tracefile = tmp;
	  callback->printf_filtered(callback,"Placing trace information into file \"%s\"\n",tracefile);
	}
      }
#endif /* TRACE */
      return SIM_RC_OK;

    case 'p':
#if defined(PROFILE)
      state |= simPROFILE;
      return SIM_RC_OK;
#else /* !PROFILE */
      fprintf(stderr,"\
Simulator constructed without profiling support (for performance).\n\
Re-compile simulator with \"-DPROFILE\" to enable this option.\n");
      return SIM_RC_FAIL;
#endif /* !PROFILE */

    case 'x':
#if defined(PROFILE)
      profile_nsamples = (unsigned)getnum(optarg);
#endif /* PROFILE */
      return SIM_RC_OK;

    case 'y':
#if defined(PROFILE)
      sim_set_profile((int)getnum(optarg));
#endif /* PROFILE */
      return SIM_RC_OK;

    }

  return SIM_RC_OK;
}

static const OPTION mips_options[] =
{
  { {"log",      required_argument, NULL,'l'},
      'l', "FILE", "Log file",
      mips_option_handler },
  { {"name",     required_argument, NULL,'n'},
      'n', "MODEL", "Select arch model",
      mips_option_handler },
  { {"profile",  optional_argument, NULL,'p'},
      'p', "on|off", "Enable profiling",
      mips_option_handler },
  { {"trace",    optional_argument, NULL,'t'},
      't', "on|off", "Enable tracing",
      mips_option_handler },
  { {"tracefile",required_argument, NULL,'z'},
      'z', "FILE", "Write trace to file",
      mips_option_handler },
  { {"frequency",required_argument, NULL,'y'},
      'y', "FREQ", "Profile frequency",
      mips_option_handler },
  { {"samples",  required_argument, NULL,'x'},
      'y', "SIZE", "Profile sample size",
      mips_option_handler },
  { {NULL, no_argument, NULL, 0}, '\0', NULL, NULL, NULL }
};


int interrupt_pending;

static void
interrupt_event (SIM_DESC sd, void *data)
{
  if (SR & status_IE)
    {
      interrupt_pending = 0;
      SignalException (Interrupt);
    }
  else if (!interrupt_pending)
    sim_events_schedule (sd, 1, interrupt_event, data);
}



/*---------------------------------------------------------------------------*/
/*-- GDB simulator interface ------------------------------------------------*/
/*---------------------------------------------------------------------------*/

SIM_DESC
sim_open (kind,cb,argv)
     SIM_OPEN_KIND kind;
     host_callback *cb;
     char **argv;
{
  SIM_DESC sd = &simulator;

  STATE_OPEN_KIND (sd) = kind;
  STATE_MAGIC (sd) = SIM_MAGIC_NUMBER;
  STATE_CALLBACK (sd) = cb;
  callback = cb;
  CPU_STATE (STATE_CPU (sd, 0)) = sd;

  /* FIXME: watchpoints code shouldn't need this */
  STATE_WATCHPOINTS (sd)->pc = &(PC);
  STATE_WATCHPOINTS (sd)->sizeof_pc = sizeof (PC);
  STATE_WATCHPOINTS (sd)->interrupt_handler = interrupt_event;

  /* memory defaults (unless sim_size was here first) */
  if (STATE_MEM_SIZE (sd) == 0)
    STATE_MEM_SIZE (sd) = (2 << 20);
  STATE_MEM_BASE (sd) = K1BASE;

  if (callback == NULL) {
    fprintf(stderr,"SIM Error: sim_open() called without callbacks attached\n");
    return 0;
  }

  state = 0;
  
  if (sim_pre_argv_init (sd, argv[0]) != SIM_RC_OK)
    return 0;
  sim_add_option_table (sd, mips_options);

  /* getopt will print the error message so we just have to exit if this fails.
     FIXME: Hmmm...  in the case of gdb we need getopt to call
     print_filtered.  */
  if (sim_parse_args (sd, argv) != SIM_RC_OK)
    {
      /* Uninstall the modules to avoid memory leaks,
	 file descriptor leaks, etc.  */
      sim_module_uninstall (sd);
      return 0;
    }

  if (sim_post_argv_init (sd) != SIM_RC_OK)
    {
      /* Uninstall the modules to avoid memory leaks,
	 file descriptor leaks, etc.  */
      sim_module_uninstall (sd);
      return 0;
    }

  /* verify assumptions the simulator made about the host type system.
     This macro does not return if there is a problem */
  CHECKSIM();

#if defined(HASFPU)
  /* Check that the host FPU conforms to IEEE 754-1985 for the SINGLE
     and DOUBLE binary formats. This is a bit nasty, requiring that we
     trust the explicit manifests held in the source: */
  /* TODO: We need to cope with the simulated target and the host not
     having the same endianness. This will require the high and low
     words of a (double) to be swapped when converting between the
     host and the simulated target. */
  {
    union {
      unsigned int i[2];
      double d;
      float f[2];
    } s;

    s.d = (double)523.2939453125;

    if ((s.i[0] == 0 && (s.f[1] != (float)4.01102924346923828125
			 || s.i[1] != 0x40805A5A))
	|| (s.i[1] == 0 && (s.f[0] != (float)4.01102924346923828125
			    || s.i[0] != 0x40805A5A)))
      {
	fprintf(stderr,"The host executing the simulator does not seem to have IEEE 754-1985 std FP\n");
	return 0;
      }
  }
#endif /* HASFPU */

  /* This is NASTY, in that we are assuming the size of specific
     registers: */
  {
    int rn;
    for (rn = 0; (rn < (LAST_EMBED_REGNUM + 1)); rn++) {
      if (rn < 32)
       register_widths[rn] = GPRLEN;
      else if ((rn >= FGRIDX) && (rn < (FGRIDX + 32)))
       register_widths[rn] = GPRLEN;
      else if ((rn >= 33) && (rn <= 37))
       register_widths[rn] = GPRLEN;
      else if ((rn == SRIDX) || (rn == FCR0IDX) || (rn == FCR31IDX) || ((rn >= 72) && (rn <= 89)))
       register_widths[rn] = 32;
      else
       register_widths[rn] = 0;
    }
  }


  if (logfile != NULL) {
    if (strcmp(logfile,"-") == 0)
     logfh = stdout;
    else {
      logfh = fopen(logfile,"wb+");
      if (logfh == NULL) {
        callback->printf_filtered(callback,"Failed to create file \"%s\", writing log information to stderr.\n",tracefile);
        logfh = stderr;
      }
    }
  }

  /* FIXME: In the future both of these malloc's can be replaced by
     calls to sim-core.  */

  /* If the host has "mmap" available we could use it to provide a
     very large virtual address space for the simulator, since memory
     would only be allocated within the "mmap" space as it is
     accessed. This can also be linked to the architecture specific
     support, required to simulate the MMU. */
  sim_size(STATE_MEM_SIZE (sd));
  /* NOTE: The above will also have enabled any profiling state! */

  /* Create the monitor address space as well */
  monitor = (unsigned char *)calloc(1,monitor_size);
  if (!monitor)
    fprintf(stderr,"Not enough VM for monitor simulation (%d bytes)\n",
	    monitor_size);

#if defined(TRACE)
  if (state & simTRACE)
    open_trace();
#endif /* TRACE */

  return sd;
}

#if defined(TRACE)
static void
open_trace()
{
  tracefh = fopen(tracefile,"wb+");
  if (tracefh == NULL)
    {
      sim_warning("Failed to create file \"%s\", writing trace information to stderr.",tracefile);
      tracefh = stderr;
  }
}
#endif /* TRACE */

/* For the profile writing, we write the data in the host
   endianness. This unfortunately means we are assuming that the
   profile file we create is processed on the same host executing the
   simulator. The gmon.out file format should either have an explicit
   endianness, or a method of encoding the endianness in the file
   header. */
static int
writeout32(fh,val)
     FILE *fh;
     unsigned int val;
{
  char buff[4];
  int res = 1;

  if (CURRENT_HOST_BYTE_ORDER == BIG_ENDIAN) {
    buff[3] = ((val >>  0) & 0xFF);
    buff[2] = ((val >>  8) & 0xFF);
    buff[1] = ((val >> 16) & 0xFF);
    buff[0] = ((val >> 24) & 0xFF);
  } else {
    buff[0] = ((val >>  0) & 0xFF);
    buff[1] = ((val >>  8) & 0xFF);
    buff[2] = ((val >> 16) & 0xFF);
    buff[3] = ((val >> 24) & 0xFF);
  }
  if (fwrite(buff,4,1,fh) != 1) {
    sim_warning("Failed to write 4bytes to the profile file");
    res = 0;
  }
  return(res);
}

static int
writeout16(fh,val)
     FILE *fh;
     unsigned short val;
{
  char buff[2];
  int res = 1;
  if (CURRENT_HOST_BYTE_ORDER == BIG_ENDIAN) {
    buff[1] = ((val >>  0) & 0xFF);
    buff[0] = ((val >>  8) & 0xFF);
  } else {
    buff[0] = ((val >>  0) & 0xFF);
    buff[1] = ((val >>  8) & 0xFF);
  }
  if (fwrite(buff,2,1,fh) != 1) {
    sim_warning("Failed to write 2bytes to the profile file");
    res = 0;
  }
  return(res);
}

void
sim_close (sd, quitting)
     SIM_DESC sd;
     int quitting;
{
#ifdef DEBUG
  printf("DBG: sim_close: entered (quitting = %d)\n",quitting);
#endif

  /* Cannot assume sim_kill() has been called */
  /* "quitting" is non-zero if we cannot hang on errors */

  /* Ensure that any resources allocated through the callback
     mechanism are released: */
  callback->shutdown(callback);

#if defined(PROFILE)
  if ((state & simPROFILE) && (profile_hist != NULL)) {
    FILE *pf = fopen("gmon.out","wb");
    unsigned loop;

    if (pf == NULL)
     sim_warning("Failed to open \"gmon.out\" profile file");
    else {
      int ok;
#ifdef DEBUG
      printf("DBG: minpc = 0x%s\n",pr_addr(profile_minpc));
      printf("DBG: maxpc = 0x%s\n",pr_addr(profile_maxpc));
#endif /* DEBUG */
      ok = writeout32(pf,(unsigned int)profile_minpc);
      if (ok)
       ok = writeout32(pf,(unsigned int)profile_maxpc);
      if (ok)
       ok = writeout32(pf,(profile_nsamples * 2) + 12); /* size of sample buffer (+ header) */
#ifdef DEBUG
      printf("DBG: nsamples = %d (size = 0x%08X)\n",profile_nsamples,((profile_nsamples * 2) + 12));
#endif /* DEBUG */
      for (loop = 0; (ok && (loop < profile_nsamples)); loop++) {
        ok = writeout16(pf,profile_hist[loop]);
        if (!ok)
         break;
      }

      fclose(pf);
    }

    free(profile_hist);
    profile_hist = NULL;
    state &= ~simPROFILE;
  }
#endif /* PROFILE */

#if defined(TRACE)
  if (tracefh != NULL && tracefh != stderr)
   fclose(tracefh);
  tracefh = NULL;
  state &= ~simTRACE;
#endif /* TRACE */

  if (logfh != NULL && logfh != stdout && logfh != stderr)
   fclose(logfh);
  logfh = NULL;

  if (STATE_MEMORY (sd) != NULL)
    free(STATE_MEMORY (sd)); /* cfree not available on all hosts */
  STATE_MEMORY (sd) = NULL;

  return;
}


int
sim_write (sd,addr,buffer,size)
     SIM_DESC sd;
     SIM_ADDR addr;
     unsigned char *buffer;
     int size;
{
  int index = size;
  uword64 vaddr = (uword64)addr;

  /* Return the number of bytes written, or zero if error. */
#ifdef DEBUG
  callback->printf_filtered(callback,"sim_write(0x%s,buffer,%d);\n",pr_addr(addr),size);
#endif

  /* We provide raw read and write routines, since we do not want to
     count the GDB memory accesses in our statistics gathering. */

  /* There is a lot of code duplication in the individual blocks
     below, but the variables are declared locally to a block to give
     the optimiser the best chance of improving the code. We have to
     perform slow byte reads from the host memory, to ensure that we
     get the data into the correct endianness for the (simulated)
     target memory world. */

  /* Mask count to get odd byte, odd halfword, and odd word out of the
     way. We can then perform doubleword transfers to and from the
     simulator memory for optimum performance. */
  if (index && (index & 1)) {
    uword64 paddr;
    int cca;
    if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW)) {
      uword64 value = ((uword64)(*buffer++));
      StoreMemory(cca,AccessLength_BYTE,value,0,paddr,vaddr,isRAW);
    }
    vaddr++;
    index &= ~1; /* logical operations usually quicker than arithmetic on RISC systems */
  }
  if (index && (index & 2)) {
    uword64 paddr;
    int cca;
    if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW)) {
      uword64 value;
      /* We need to perform the following magic to ensure that that
         bytes are written into same byte positions in the target memory
         world, regardless of the endianness of the host. */
      if (BigEndianMem) {
        value =  ((uword64)(*buffer++) << 8);
        value |= ((uword64)(*buffer++) << 0);
      } else {
        value =  ((uword64)(*buffer++) << 0);
        value |= ((uword64)(*buffer++) << 8);
      }
      StoreMemory(cca,AccessLength_HALFWORD,value,0,paddr,vaddr,isRAW);
    }
    vaddr += 2;
    index &= ~2;
  }
  if (index && (index & 4)) {
    uword64 paddr;
    int cca;
    if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW)) {
      uword64 value;
      if (BigEndianMem) {
        value =  ((uword64)(*buffer++) << 24);
        value |= ((uword64)(*buffer++) << 16);
        value |= ((uword64)(*buffer++) << 8);
        value |= ((uword64)(*buffer++) << 0);
      } else {
        value =  ((uword64)(*buffer++) << 0);
        value |= ((uword64)(*buffer++) << 8);
        value |= ((uword64)(*buffer++) << 16);
        value |= ((uword64)(*buffer++) << 24);
      }
      StoreMemory(cca,AccessLength_WORD,value,0,paddr,vaddr,isRAW);
    }
    vaddr += 4;
    index &= ~4;
  }
  for (;index; index -= 8) {
    uword64 paddr;
    int cca;
    if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW)) {
      uword64 value;
      if (BigEndianMem) {
        value =  ((uword64)(*buffer++) << 56);
        value |= ((uword64)(*buffer++) << 48);
        value |= ((uword64)(*buffer++) << 40);
        value |= ((uword64)(*buffer++) << 32);
        value |= ((uword64)(*buffer++) << 24);
        value |= ((uword64)(*buffer++) << 16);
        value |= ((uword64)(*buffer++) << 8);
        value |= ((uword64)(*buffer++) << 0);
      } else {
        value =  ((uword64)(*buffer++) << 0);
        value |= ((uword64)(*buffer++) << 8);
        value |= ((uword64)(*buffer++) << 16);
        value |= ((uword64)(*buffer++) << 24);
        value |= ((uword64)(*buffer++) << 32);
        value |= ((uword64)(*buffer++) << 40);
        value |= ((uword64)(*buffer++) << 48);
        value |= ((uword64)(*buffer++) << 56);
      }
      StoreMemory(cca,AccessLength_DOUBLEWORD,value,0,paddr,vaddr,isRAW);
    }
    vaddr += 8;
  }

  return(size);
}

int
sim_read (sd,addr,buffer,size)
     SIM_DESC sd;
     SIM_ADDR addr;
     unsigned char *buffer;
     int size;
{
  int index;

  /* Return the number of bytes read, or zero if error. */
#ifdef DEBUG
  callback->printf_filtered(callback,"sim_read(0x%s,buffer,%d);\n",pr_addr(addr),size);
#endif /* DEBUG */

  /* TODO: Perform same optimisation as the sim_write() code
     above. NOTE: This will require a bit more work since we will need
     to ensure that the source physical address is doubleword aligned
     before, and then deal with trailing bytes. */
  for (index = 0; (index < size); index++) {
    uword64 vaddr,paddr,value;
    int cca;
    vaddr = (uword64)addr + index;
    if (AddressTranslation(vaddr,isDATA,isLOAD,&paddr,&cca,isTARGET,isRAW)) {
      LoadMemory(&value,NULL,cca,AccessLength_BYTE,paddr,vaddr,isDATA,isRAW);
      buffer[index] = (unsigned char)(value&0xFF);
    } else
     break;
  }

  return(index);
}

void
sim_store_register (sd,rn,memory)
     SIM_DESC sd;
     int rn;
     unsigned char *memory;
{
  /* NOTE: gdb (the client) stores registers in target byte order
     while the simulator uses host byte order */
#ifdef DEBUG
  callback->printf_filtered(callback,"sim_store_register(%d,*memory=0x%s);\n",rn,pr_addr(*((SIM_ADDR *)memory)));
#endif /* DEBUG */

  /* Unfortunately this suffers from the same problem as the register
     numbering one. We need to know what the width of each logical
     register number is for the architecture being simulated. */

  if (register_widths[rn] == 0)
    sim_warning("Invalid register width for %d (register store ignored)",rn);
  else
    {
      if (register_widths[rn] == 32)
	registers[rn] = T2H_4 (*(unsigned int*)memory);
      else
	registers[rn] = T2H_8 (*(uword64*)memory);
    }

  return;
}

void
sim_fetch_register (sd,rn,memory)
     SIM_DESC sd;
     int rn;
     unsigned char *memory;
{
  /* NOTE: gdb (the client) stores registers in target byte order
     while the simulator uses host byte order */
#ifdef DEBUG
  callback->printf_filtered(callback,"sim_fetch_register(%d=0x%s,mem) : place simulator registers into memory\n",rn,pr_addr(registers[rn]));
#endif /* DEBUG */

  if (register_widths[rn] == 0)
    sim_warning("Invalid register width for %d (register fetch ignored)",rn);
  else
    {
      if (register_widths[rn] == 32)
	*((unsigned int *)memory) = H2T_4 ((unsigned int)(registers[rn] & 0xFFFFFFFF));
      else /* 64bit register */
	*((uword64 *)memory) = H2T_8 (registers[rn]);
    }

  return;
}


void
sim_info (sd,verbose)
     SIM_DESC sd;
     int verbose;
{
  
  return;
  /* Accessed from the GDB "info files" command: */
  if (STATE_VERBOSE_P (sd) || verbose)
    {
      
      sim_io_printf (sd, "MIPS %d-bit %s endian simulator\n",
		     (PROCESSOR_64BIT ? 64 : 32),
		     (CURRENT_TARGET_BYTE_ORDER == BIG_ENDIAN ? "Big" : "Little"));
      
      sim_io_printf (sd, "0x%08X bytes of memory at 0x%s\n",
		     STATE_MEM_SIZE (sd),
		     pr_addr (STATE_MEM_BASE (sd)));
      
#if !defined(FASTSIM)
#if 0
      /* at present this simulator executes one instruction per
         simulator cycle.  Consequently this data never changes */
      if (instruction_fetch_overflow != 0)
	sim_io_printf (sd, "Instruction fetches = 0x%08X%08X\n",
		       instruction_fetch_overflow, instruction_fetches);
      else
	sim_io_printf (sd, "Instruction fetches = %d\n", instruction_fetches);
#endif
      /* It would be a useful feature, if when performing multi-cycle
	 simulations (rather than single-stepping) we keep the start and
	 end times of the execution, so that we can give a performance
	 figure for the simulator. */
#endif /* !FASTSIM */
      sim_io_printf (sd, "Number of execution cycles = %ld\n",
		     (long) sim_events_time (sd));
      
      /* print information pertaining to MIPS ISA and architecture being simulated */
      /* things that may be interesting */
      /* instructions executed - if available */
      /* cycles executed - if available */
      /* pipeline stalls - if available */
      /* virtual time taken */
      /* profiling size */
      /* profiling frequency */
      /* profile minpc */
      /* profile maxpc */
    }
}

SIM_RC
sim_load (sd,prog,abfd,from_tty)
     SIM_DESC sd;
     char *prog;
     bfd *abfd;
     int from_tty;
{
  bfd *prog_bfd;

  prog_bfd = sim_load_file (sd,
			    STATE_MY_NAME (sd),
			    callback,
			    prog,
			    /* pass NULL for abfd, we always open our own */
			    NULL,
			    STATE_OPEN_KIND (sd) == SIM_OPEN_DEBUG);
  if (prog_bfd == NULL)
    return SIM_RC_FAIL;
  sim_analyze_program (sd, prog_bfd);

  /* Configure/verify the target byte order and other runtime
     configuration options */
  sim_config (sd, PREFERED_TARGET_BYTE_ORDER(prog_bfd));

  /* (re) Write the monitor trap address handlers into the monitor
     (eeprom) address space.  This can only be done once the target
     endianness has been determined. */
  {
    unsigned loop;
    /* Entry into the IDT monitor is via fixed address vectors, and
       not using machine instructions. To avoid clashing with use of
       the MIPS TRAP system, we place our own (simulator specific)
       "undefined" instructions into the relevant vector slots. */
    for (loop = 0; (loop < monitor_size); loop += 4) {
      uword64 vaddr = (monitor_base + loop);
      uword64 paddr;
      int cca;
      if (AddressTranslation(vaddr, isDATA, isSTORE, &paddr, &cca, isTARGET, isRAW))
	StoreMemory(cca, AccessLength_WORD,
		    (RSVD_INSTRUCTION | (((loop >> 2) & RSVD_INSTRUCTION_ARG_MASK) << RSVD_INSTRUCTION_ARG_SHIFT)),
		    0, paddr, vaddr, isRAW);
    }
    /* The PMON monitor uses the same address space, but rather than
       branching into it the address of a routine is loaded. We can
       cheat for the moment, and direct the PMON routine to IDT style
       instructions within the monitor space. This relies on the IDT
       monitor not using the locations from 0xBFC00500 onwards as its
       entry points.*/
    for (loop = 0; (loop < 24); loop++)
      {
        uword64 vaddr = (monitor_base + 0x500 + (loop * 4));
        uword64 paddr;
        int cca;
        unsigned int value = ((0x500 - 8) / 8); /* default UNDEFINED reason code */
        switch (loop)
          {
            case 0: /* read */
              value = 7;
              break;

            case 1: /* write */
              value = 8;
              break;

            case 2: /* open */
              value = 6;
              break;

            case 3: /* close */
              value = 10;
              break;

            case 5: /* printf */
              value = ((0x500 - 16) / 8); /* not an IDT reason code */
              break;

            case 8: /* cliexit */
              value = 17;
              break;

            case 11: /* flush_cache */
              value = 28;
              break;
          }
	    /* FIXME - should monitor_base be SIM_ADDR?? */
        value = ((unsigned int)monitor_base + (value * 8));
        if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW))
          StoreMemory(cca,AccessLength_WORD,value,0,paddr,vaddr,isRAW);
        else
          sim_error("Failed to write to monitor space 0x%s",pr_addr(vaddr));

	/* The LSI MiniRISC PMON has its vectors at 0x200, not 0x500.  */
	vaddr -= 0x300;
        if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isRAW))
          StoreMemory(cca,AccessLength_WORD,value,0,paddr,vaddr,isRAW);
        else
          sim_error("Failed to write to monitor space 0x%s",pr_addr(vaddr));
      }
  }

  return SIM_RC_OK;
}

SIM_RC
sim_create_inferior (sd, argv,env)
     SIM_DESC sd;
     char **argv;
     char **env;
{

#ifdef DEBUG
  printf("DBG: sim_create_inferior entered: start_address = 0x%s\n",
	 pr_addr(PC));
#endif /* DEBUG */

  ColdReset();
  /* If we were providing a more complete I/O, co-processor or memory
     simulation, we should perform any "device" initialisation at this
     point. This can include pre-loading memory areas with particular
     patterns (e.g. simulating ROM monitors). */

#if 1
  PC = (uword64) STATE_START_ADDR(sd);
#else
  /* TODO: Sort this properly. SIM_ADDR may already be a 64bit value: */
  PC = SIGNEXTEND(bfd_get_start_address(prog_bfd),32);
#endif

  /* Prepare to execute the program to be simulated */
  /* argv and env are NULL terminated lists of pointers */

  if (argv || env) {
#if 0 /* def DEBUG */
    callback->printf_filtered(callback,"sim_create_inferior() : passed arguments ignored\n");
    {
     char **cptr;
     for (cptr = argv; (cptr && *cptr); cptr++)
      printf("DBG: arg \"%s\"\n",*cptr);
    }
#endif /* DEBUG */
    /* We should really place the argv slot values into the argument
       registers, and onto the stack as required. However, this
       assumes that we have a stack defined, which is not necessarily
       true at the moment. */
  }

  return SIM_RC_OK;
}

void
sim_kill (sd)
     SIM_DESC sd;
{
#if 1
  /* This routine should be for terminating any existing simulation
     thread. Since we are single-threaded only at the moment, this is
     not an issue. It should *NOT* be used to terminate the
     simulator. */
#else /* do *NOT* call sim_close */
  sim_close(sd, 1); /* Do not hang on errors */
  /* This would also be the point where any memory mapped areas used
     by the simulator should be released. */
#endif
  return;
}

typedef enum {e_terminate,e_help,e_setmemsize,e_reset} e_cmds;

static struct t_sim_command {
 e_cmds id;
 const char *name;
 const char *help;
} sim_commands[] = {
  {e_help,      "help",           ": Show MIPS simulator private commands"},
  {e_setmemsize,"set-memory-size","<n> : Specify amount of memory simulated"},
  {e_reset,     "reset-system",   ": Reset the simulated processor"},
  {e_terminate, NULL}
};

void
sim_do_command (sd,cmd)
     SIM_DESC sd;
     char *cmd;
{
  struct t_sim_command *cptr;

  if (callback == NULL) {
    fprintf(stderr,"Simulator not enabled: \"target sim\" should be used to activate\n");
    return;
  }

  if (!(cmd && *cmd != '\0'))
   cmd = "help";

  /* NOTE: Accessed from the GDB "sim" commmand: */
  for (cptr = sim_commands; cptr && cptr->name; cptr++)
    if (strncmp (cmd, cptr->name, strlen(cptr->name)) == 0)
      {
	cmd += strlen(cptr->name);
	switch (cptr->id) {
	case e_help: /* no arguments */
	  { /* no arguments */
	    struct t_sim_command *lptr;
	    callback->printf_filtered(callback,"List of MIPS simulator commands:\n");
	    for (lptr = sim_commands; lptr->name; lptr++)
	      callback->printf_filtered(callback,"%s %s\n",lptr->name,lptr->help);
	    sim_args_command (sd, "help");
	  }
        break;

	case e_setmemsize: /* memory size argument */
	  {
	    unsigned int newsize = (unsigned int)getnum(cmd);
	    sim_size(newsize);
	  }
        break;

	case e_reset: /* no arguments */
	  ColdReset();
	  /* NOTE: See the comments in sim_open() relating to device
	     initialisation. */
	  break;

	default:
	  callback->printf_filtered(callback,"FATAL: Matched \"%s\", but failed to match command id %d.\n",cmd,cptr->id);
	  break;
	}
	break;
      }

  if (!(cptr->name))
    {
      /* try for a common command when the sim specific lookup fails */
      if (sim_args_command (sd, cmd) != SIM_RC_OK)
	callback->printf_filtered(callback,"Error: \"%s\" is not a valid MIPS simulator command.\n",cmd);
    }

  return;
}

/*---------------------------------------------------------------------------*/
/* NOTE: The following routines do not seem to be used by GDB at the
   moment. However, they may be useful to the standalone simulator
   world. */


/* The profiling format is described in the "gmon_out.h" header file */
void
sim_set_profile (n)
     int n;
{
#if defined(PROFILE)
  profile_frequency = n;
  state |= simPROFILE;
#endif /* PROFILE */
  return;
}

void
sim_set_profile_size (n)
     int n;
{
  SIM_DESC sd = &simulator;
#if defined(PROFILE)
  if (state & simPROFILE) {
    int bsize;

    /* Since we KNOW that the memory banks are a power-of-2 in size: */
    profile_nsamples = power2(n);
    profile_minpc = STATE_MEM_BASE (sd);
    profile_maxpc = (STATE_MEM_BASE (sd) + STATE_MEM_SIZE (sd));

    /* Just in-case we are sampling every address: NOTE: The shift
       right of 2 is because we only have word-aligned PC addresses. */
    if (profile_nsamples > (STATE_MEM_SIZE (sd) >> 2))
     profile_nsamples = (STATE_MEM_SIZE (sd) >> 2);

    /* Since we are dealing with power-of-2 values: */
    profile_shift = (((STATE_MEM_SIZE (sd) >> 2) / profile_nsamples) - 1);

    bsize = (profile_nsamples * sizeof(unsigned short));
    if (profile_hist == NULL)
     profile_hist = (unsigned short *)calloc(64,(bsize / 64));
    else
     profile_hist = (unsigned short *)realloc(profile_hist,bsize);
    if (profile_hist == NULL) {
      sim_warning("Failed to allocate VM for profiling buffer (0x%08X bytes)",bsize);
      state &= ~simPROFILE;
    }
  }
#endif /* PROFILE */

  return;
}

void
sim_size(newsize)
     int newsize;
{
  SIM_DESC sd = &simulator;
  char *new;
  /* Used by "run", and internally, to set the simulated memory size */
  if (newsize == 0) {
    callback->printf_filtered(callback,"Zero not valid: Memory size still 0x%08X bytes\n",STATE_MEM_SIZE (sd));
    return;
  }
  newsize = power2(newsize);
  if (STATE_MEMORY (sd) == NULL)
   new = (char *)calloc(64,(STATE_MEM_SIZE (sd) / 64));
  else
   new = (char *)realloc(STATE_MEMORY (sd),newsize);
  if (new == NULL) {
    if (STATE_MEMORY (sd) == NULL)
     sim_error("Not enough VM for simulation memory of 0x%08X bytes",STATE_MEM_SIZE (sd));
    else
     sim_warning("Failed to resize memory (still 0x%08X bytes)",STATE_MEM_SIZE (sd));
  } else {
    STATE_MEM_SIZE (sd) = (unsigned)newsize;
    STATE_MEMORY (sd) = new;
#if defined(PROFILE)
    /* Ensure that we sample across the new memory range */
    sim_set_profile_size(profile_nsamples);
#endif /* PROFILE */
  }

  return;
}

int
sim_trace(sd)
     SIM_DESC sd;
{
  sim_io_eprintf (sd, "Sim trace not supported");
#if 0
  /* This routine is called by the "run" program, when detailed
     execution information is required. Rather than executing a single
     instruction, and looping around externally... we just start
     simulating, returning TRUE when the simulator stops (for whatever
     reason). */

#if defined(TRACE)
  /* Ensure tracing is enabled, if available */
  if (tracefh == NULL)
    {
      open_trace();
      state |= simTRACE;
    }
#endif /* TRACE */

#if 0
  state &= ~(simSTOP | simSTEP); /* execute until event */
#endif
  state |= (simHALTEX | simHALTIN); /* treat interrupt event as exception */
  /* Start executing instructions from the current state (set
     explicitly by register updates, or by sim_create_inferior): */
  simulate();

#endif
  return(1);
}

/*---------------------------------------------------------------------------*/
/*-- Private simulator support interface ------------------------------------*/
/*---------------------------------------------------------------------------*/

/* Simple monitor interface (currently setup for the IDT and PMON monitors) */
static void
sim_monitor(reason)
     unsigned int reason;
{
  SIM_DESC sd = &simulator;
#ifdef DEBUG
  printf("DBG: sim_monitor: entered (reason = %d)\n",reason);
#endif /* DEBUG */

  /* The IDT monitor actually allows two instructions per vector
     slot. However, the simulator currently causes a trap on each
     individual instruction. We cheat, and lose the bottom bit. */
  reason >>= 1;

  /* The following callback functions are available, however the
     monitor we are simulating does not make use of them: get_errno,
     isatty, lseek, rename, system, time and unlink */
  switch (reason) {
    case 6: /* int open(char *path,int flags) */
      {
        uword64 paddr;
        int cca;
        if (AddressTranslation(A0,isDATA,isLOAD,&paddr,&cca,isHOST,isREAL))
         V0 = callback->open(callback,(char *)((int)paddr),(int)A1);
        else
         sim_error("Attempt to pass pointer that does not reference simulated memory");
      }
      break;

    case 7: /* int read(int file,char *ptr,int len) */
      {
        uword64 paddr;
        int cca;
        if (AddressTranslation(A1,isDATA,isLOAD,&paddr,&cca,isHOST,isREAL))
         V0 = callback->read(callback,(int)A0,(char *)((int)paddr),(int)A2);
        else
         sim_error("Attempt to pass pointer that does not reference simulated memory");
      }
      break;

    case 8: /* int write(int file,char *ptr,int len) */
      {
        uword64 paddr;
        int cca;
        if (AddressTranslation(A1,isDATA,isLOAD,&paddr,&cca,isHOST,isREAL))
         V0 = callback->write(callback,(int)A0,(const char *)((int)paddr),(int)A2);
        else
         sim_error("Attempt to pass pointer that does not reference simulated memory");
      }
      break;

    case 10: /* int close(int file) */
      V0 = callback->close(callback,(int)A0);
      break;

    case 11: /* char inbyte(void) */
      {
        char tmp;
        if (callback->read_stdin(callback,&tmp,sizeof(char)) != sizeof(char)) {
          sim_error("Invalid return from character read");
          V0 = (ut_reg)-1;
        }
        else
         V0 = (ut_reg)tmp;
      }
      break;

    case 12: /* void outbyte(char chr) : write a byte to "stdout" */
      {
        char tmp = (char)(A0 & 0xFF);
        callback->write_stdout(callback,&tmp,sizeof(char));
      }
      break;

    case 17: /* void _exit() */
      sim_warning("sim_monitor(17): _exit(int reason) to be coded");
      sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA, sim_exited,
		       (unsigned int)(A0 & 0xFFFFFFFF));
      break;

    case 28 : /* PMON flush_cache */
      break;

    case 55: /* void get_mem_info(unsigned int *ptr) */
      /* in:  A0 = pointer to three word memory location */
      /* out: [A0 + 0] = size */
      /*      [A0 + 4] = instruction cache size */
      /*      [A0 + 8] = data cache size */
      {
        uword64 vaddr = A0;
        uword64 paddr, value;
        int cca;
        int failed = 0;

        /* NOTE: We use RAW memory writes here, but since we are not
           gathering statistics for the monitor calls we are simulating,
           it is not an issue. */

        /* Memory size */
        if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isREAL)) {
          value = (uword64)STATE_MEM_SIZE (sd);
          StoreMemory(cca,AccessLength_WORD,value,0,paddr,vaddr,isRAW);
          /* We re-do the address translations, in-case the block
             overlaps a memory boundary: */
          value = 0;
          vaddr += (AccessLength_WORD + 1);
          if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isREAL)) {
            StoreMemory(cca,AccessLength_WORD,0,value,paddr,vaddr,isRAW);
            vaddr += (AccessLength_WORD + 1);
            if (AddressTranslation(vaddr,isDATA,isSTORE,&paddr,&cca,isTARGET,isREAL))
             StoreMemory(cca,AccessLength_WORD,value,0,paddr,vaddr,isRAW);
            else
             failed = -1;
          } else
           failed = -1;
        } else
         failed = -1;

        if (failed)
         sim_error("Invalid pointer passed into monitor call");
      }
      break;

    case 158 : /* PMON printf */
      /* in:  A0 = pointer to format string */
      /*      A1 = optional argument 1 */
      /*      A2 = optional argument 2 */
      /*      A3 = optional argument 3 */
      /* out: void */
      /* The following is based on the PMON printf source */
      {
        uword64 paddr;
        int cca;
        /* This isn't the quickest way, since we call the host print
           routine for every character almost. But it does avoid
           having to allocate and manage a temporary string buffer. */
        if (AddressTranslation(A0,isDATA,isLOAD,&paddr,&cca,isHOST,isREAL)) {
          char *s = (char *)((int)paddr);
          ut_reg *ap = &A1; /* 1st argument */
          /* TODO: Include check that we only use three arguments (A1, A2 and A3) */
          for (; *s;) {
            if (*s == '%') {
              char tmp[40];
              enum {FMT_RJUST, FMT_LJUST, FMT_RJUST0, FMT_CENTER} fmt = FMT_RJUST;
              int width = 0, trunc = 0, haddot = 0, longlong = 0;
              s++;
              for (; *s; s++) {
                if (strchr ("dobxXulscefg%", *s))
                  break;
		else if (*s == '-')
                  fmt = FMT_LJUST;
		else if (*s == '0')
                  fmt = FMT_RJUST0;
		else if (*s == '~')
                  fmt = FMT_CENTER;
		else if (*s == '*') {
                  if (haddot)
                    trunc = (int)*ap++;
                  else
                    width = (int)*ap++;
		} else if (*s >= '1' && *s <= '9') {
                  char *t;
                  unsigned int n;
                  for (t = s; isdigit (*s); s++);
                  strncpy (tmp, t, s - t);
                  tmp[s - t] = '\0';
                  n = (unsigned int)strtol(tmp,NULL,10);
                  if (haddot)
                   trunc = n;
                  else
                   width = n;
                  s--;
		} else if (*s == '.')
                  haddot = 1;
              }
              if (*s == '%') {
                callback->printf_filtered(callback,"%%");
              } else if (*s == 's') {
                if ((int)*ap != 0) {
                  if (AddressTranslation(*ap++,isDATA,isLOAD,&paddr,&cca,isHOST,isREAL)) {
                    char *p = (char *)((int)paddr);;
                    callback->printf_filtered(callback,p);
                  } else {
                    ap++;
                    sim_error("Attempt to pass pointer that does not reference simulated memory");
                  }
                }
		else
                  callback->printf_filtered(callback,"(null)");
              } else if (*s == 'c') {
                int n = (int)*ap++;
		callback->printf_filtered(callback,"%c",n);
              } else {
		if (*s == 'l') {
                  if (*++s == 'l') {
                    longlong = 1;
                    ++s;
                  }
		}
		if (strchr ("dobxXu", *s)) {
                  word64 lv = (word64) *ap++;
                  if (*s == 'b')
                    callback->printf_filtered(callback,"<binary not supported>");
                  else {
                    sprintf(tmp,"%%%s%c",longlong ? "ll" : "",*s);
                    if (longlong)
                      callback->printf_filtered(callback,tmp,lv);
                    else
                      callback->printf_filtered(callback,tmp,(int)lv);
                  }
		} else if (strchr ("eEfgG", *s)) {
#ifdef _MSC_VER /* MSVC version 2.x can't convert from uword64 directly */
                  double dbl = (double)((word64)*ap++);
#else
                  double dbl = (double)*ap++;
#endif
                  sprintf(tmp,"%%%d.%d%c",width,trunc,*s);
                  callback->printf_filtered(callback,tmp,dbl);
                  trunc = 0;
		}
              }
              s++;
            } else
             callback->printf_filtered(callback,"%c",*s++);
          }
        } else
         sim_error("Attempt to pass pointer that does not reference simulated memory");
      }
      break;

    default:
      sim_warning("TODO: sim_monitor(%d) : PC = 0x%s",reason,pr_addr(IPC));
      sim_warning("(Arguments : A0 = 0x%s : A1 = 0x%s : A2 = 0x%s : A3 = 0x%s)",pr_addr(A0),pr_addr(A1),pr_addr(A2),pr_addr(A3));
      break;
  }
  return;
}

/* Store a word into memory.  */

static void
store_word (vaddr, val)
     uword64 vaddr;
     t_reg val;
{
  uword64 paddr;
  int uncached;

  if ((vaddr & 3) != 0)
    SignalException (AddressStore);
  else
    {
      if (AddressTranslation (vaddr, isDATA, isSTORE, &paddr, &uncached,
			      isTARGET, isREAL))
	{
	  const uword64 mask = 7;
	  uword64 memval;
	  unsigned int byte;

	  paddr = (paddr & ~mask) | ((paddr & mask) ^ (ReverseEndian << 2));
	  byte = (vaddr & mask) ^ (BigEndianCPU << 2);
	  memval = ((uword64) val) << (8 * byte);
	  StoreMemory (uncached, AccessLength_WORD, memval, 0, paddr, vaddr,
		       isREAL);
	}
    }
}

/* Load a word from memory.  */

static t_reg
load_word (vaddr)
     uword64 vaddr;
{
  if ((vaddr & 3) != 0)
    SignalException (AddressLoad);
  else
    {
      uword64 paddr;
      int uncached;

      if (AddressTranslation (vaddr, isDATA, isLOAD, &paddr, &uncached,
			      isTARGET, isREAL))
	{
	  const uword64 mask = 0x7;
	  const unsigned int reverse = ReverseEndian ? 1 : 0;
	  const unsigned int bigend = BigEndianCPU ? 1 : 0;
	  uword64 memval;
	  unsigned int byte;

	  paddr = (paddr & ~mask) | ((paddr & mask) ^ (reverse << 2));
	  LoadMemory (&memval,NULL,uncached, AccessLength_WORD, paddr, vaddr,
			       isDATA, isREAL);
	  byte = (vaddr & mask) ^ (bigend << 2);
	  return SIGNEXTEND (((memval >> (8 * byte)) & 0xffffffff), 32);
	}
    }

  return 0;
}

/* Simulate the mips16 entry and exit pseudo-instructions.  These
   would normally be handled by the reserved instruction exception
   code, but for ease of simulation we just handle them directly.  */

static void
mips16_entry (insn)
     unsigned int insn;
{
  int aregs, sregs, rreg;

#ifdef DEBUG
  printf("DBG: mips16_entry: entered (insn = 0x%08X)\n",insn);
#endif /* DEBUG */

  aregs = (insn & 0x700) >> 8;
  sregs = (insn & 0x0c0) >> 6;
  rreg =  (insn & 0x020) >> 5;

  /* This should be checked by the caller.  */
  if (sregs == 3)
    abort ();

  if (aregs < 5)
    {
      int i;
      t_reg tsp;

      /* This is the entry pseudo-instruction.  */

      for (i = 0; i < aregs; i++)
	store_word ((uword64) (SP + 4 * i), registers[i + 4]);

      tsp = SP;
      SP -= 32;

      if (rreg)
	{
	  tsp -= 4;
	  store_word ((uword64) tsp, RA);
	}

      for (i = 0; i < sregs; i++)
	{
	  tsp -= 4;
	  store_word ((uword64) tsp, registers[16 + i]);
	}
    }
  else
    {
      int i;
      t_reg tsp;

      /* This is the exit pseudo-instruction.  */

      tsp = SP + 32;

      if (rreg)
	{
	  tsp -= 4;
	  RA = load_word ((uword64) tsp);
	}

      for (i = 0; i < sregs; i++)
	{
	  tsp -= 4;
	  registers[i + 16] = load_word ((uword64) tsp);
	}

      SP += 32;

      if (aregs == 5)
	{
	  FGR[0] = WORD64LO (GPR[4]);
	  fpr_state[0] = fmt_uninterpreted;
	}
      else if (aregs == 6)
	{
	  FGR[0] = WORD64LO (GPR[5]);
	  FGR[1] = WORD64LO (GPR[4]);
	  fpr_state[0] = fmt_uninterpreted;
	  fpr_state[1] = fmt_uninterpreted;
	}

      PC = RA;
    }
}

void
sim_warning(char *fmt,...)
{
  char buf[256];
  va_list ap;

  va_start (ap,fmt);
  vsprintf (buf, fmt, ap);
  va_end (ap);
  
  if (logfh != NULL) {
    fprintf(logfh,"SIM Warning: %s\n", buf);
  } else {
    callback->printf_filtered(callback,"SIM Warning: %s\n", buf);
  }
  /* This used to call SignalException with a SimulatorFault, but that causes
     the simulator to exit, and that is inappropriate for a warning.  */
  return;
}

void
sim_error(char *fmt,...)
{
  char buf[256];
  va_list ap;

  va_start (ap,fmt);
  vsprintf (buf, fmt, ap);
  va_end (ap);

  callback->printf_filtered(callback,"SIM Error: %s", buf);
  SignalException (SimulatorFault, buf);
  return;
}

static unsigned int
power2(value)
     unsigned int value;
{
  int loop,tmp;

  /* Round *UP* to the nearest power-of-2 if not already one */
  if (value != (value & ~(value - 1))) {
    for (tmp = value, loop = 0; (tmp != 0); loop++)
     tmp >>= 1;
    value = (1 << loop);
  }

  return(value);
}

static long
getnum(value)
     char *value;
{
  long num;
  char *end;

  num = strtol(value,&end,10);
  if (end == value)
   callback->printf_filtered(callback,"Warning: Invalid number \"%s\" ignored, using zero\n",value);
  else {
    if (*end && ((tolower(*end) == 'k') || (tolower(*end) == 'm'))) {
      if (tolower(*end) == 'k')
       num *= (1 << 10);
      else
       num *= (1 << 20);
      end++;
    }
    if (*end)
     callback->printf_filtered(callback,"Warning: Spurious characters \"%s\" at end of number ignored\n",end);
  }

  return(num);
}

/*-- trace support ----------------------------------------------------------*/

/* The TRACE support is provided (if required) in the memory accessing
   routines. Since we are also providing the architecture specific
   features, the architecture simulation code can also deal with
   notifying the TRACE world of cache flushes, etc. Similarly we do
   not need to provide profiling support in the simulator engine,
   since we can sample in the instruction fetch control loop. By
   defining the TRACE manifest, we add tracing as a run-time
   option. */

#if defined(TRACE)
/* Tracing by default produces "din" format (as required by
   dineroIII). Each line of such a trace file *MUST* have a din label
   and address field. The rest of the line is ignored, so comments can
   be included if desired. The first field is the label which must be
   one of the following values:

	0       read data
        1       write data
        2       instruction fetch
        3       escape record (treated as unknown access type)
        4       escape record (causes cache flush)

   The address field is a 32bit (lower-case) hexadecimal address
   value. The address should *NOT* be preceded by "0x".

   The size of the memory transfer is not important when dealing with
   cache lines (as long as no more than a cache line can be
   transferred in a single operation :-), however more information
   could be given following the dineroIII requirement to allow more
   complete memory and cache simulators to provide better
   results. i.e. the University of Pisa has a cache simulator that can
   also take bus size and speed as (variable) inputs to calculate
   complete system performance (a much more useful ability when trying
   to construct an end product, rather than a processor). They
   currently have an ARM version of their tool called ChARM. */


static
void dotrace(FILE *tracefh,int type,SIM_ADDR address,int width,char *comment,...)
{
  if (state & simTRACE) {
    va_list ap;
    fprintf(tracefh,"%d %s ; width %d ; ", 
		type,
		pr_addr(address),
		width);
    va_start(ap,comment);
    vfprintf(tracefh,comment,ap);
    va_end(ap);
    fprintf(tracefh,"\n");
  }
  /* NOTE: Since the "din" format will only accept 32bit addresses, and
     we may be generating 64bit ones, we should put the hi-32bits of the
     address into the comment field. */

  /* TODO: Provide a buffer for the trace lines. We can then avoid
     performing writes until the buffer is filled, or the file is
     being closed. */

  /* NOTE: We could consider adding a comment field to the "din" file
     produced using type 3 markers (unknown access). This would then
     allow information about the program that the "din" is for, and
     the MIPs world that was being simulated, to be placed into the
     trace file. */

  return;
}
#endif /* TRACE */

/*---------------------------------------------------------------------------*/
/*-- simulator engine -------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

static void
ColdReset()
{
  /* RESET: Fixed PC address: */
  PC = (((uword64)0xFFFFFFFF<<32) | 0xBFC00000);
  /* The reset vector address is in the unmapped, uncached memory space. */

  SR &= ~(status_SR | status_TS | status_RP);
  SR |= (status_ERL | status_BEV);

#if defined(HASFPU) && (GPRLEN == (64))
  /* Cheat and allow access to the complete register set immediately: */
  SR |= status_FR; /* 64bit registers */
#endif /* HASFPU and 64bit FP registers */

  /* Ensure that any instructions with pending register updates are
     cleared: */
  {
    int loop;
    for (loop = 0; (loop < PSLOTS); loop++)
     pending_slot_reg[loop] = (LAST_EMBED_REGNUM + 1);
    pending_in = pending_out = pending_total = 0;
  }

#if defined(HASFPU)
  /* Initialise the FPU registers to the unknown state */
  {
    int rn;
    for (rn = 0; (rn < 32); rn++)
     fpr_state[rn] = fmt_uninterpreted;
  }
#endif /* HASFPU */

  return;
}

/* Description from page A-22 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* Translate a virtual address to a physical address and cache
   coherence algorithm describing the mechanism used to resolve the
   memory reference. Given the virtual address vAddr, and whether the
   reference is to Instructions ot Data (IorD), find the corresponding
   physical address (pAddr) and the cache coherence algorithm (CCA)
   used to resolve the reference. If the virtual address is in one of
   the unmapped address spaces the physical address and the CCA are
   determined directly by the virtual address. If the virtual address
   is in one of the mapped address spaces then the TLB is used to
   determine the physical address and access type; if the required
   translation is not present in the TLB or the desired access is not
   permitted the function fails and an exception is taken.

   NOTE: This function is extended to return an exception state. This,
   along with the exception generation is used to notify whether a
   valid address translation occured */

static int
AddressTranslation(vAddr,IorD,LorS,pAddr,CCA,host,raw)
     uword64 vAddr;
     int IorD;
     int LorS;
     uword64 *pAddr;
     int *CCA;
     int host;
     int raw;
{
  SIM_DESC sd = &simulator;
  int res = -1; /* TRUE : Assume good return */

#ifdef DEBUG
  callback->printf_filtered(callback,"AddressTranslation(0x%s,%s,%s,...);\n",pr_addr(vAddr),(IorD ? "isDATA" : "isINSTRUCTION"),(LorS ? "iSTORE" : "isLOAD"));
#endif

  /* Check that the address is valid for this memory model */

  /* For a simple (flat) memory model, we simply pass virtual
     addressess through (mostly) unchanged. */
  vAddr &= 0xFFFFFFFF;

  /* Treat the kernel memory spaces identically for the moment: */
  if ((STATE_MEM_BASE (sd) == K1BASE) && (vAddr >= K0BASE) && (vAddr < (K0BASE + K0SIZE)))
    vAddr += (K1BASE - K0BASE);

  /* Also assume that the K1BASE memory wraps. This is required to
     allow the PMON run-time __sizemem() routine to function (without
     having to provide exception simulation). NOTE: A kludge to work
     around the fact that the monitor memory is currently held in the
     K1BASE space. */
  if (((vAddr < monitor_base) || (vAddr >= (monitor_base + monitor_size))) && (vAddr >= K1BASE && vAddr < (K1BASE + K1SIZE)))
    vAddr = (K1BASE | (vAddr & (STATE_MEM_SIZE (sd) - 1)));

  *pAddr = vAddr; /* default for isTARGET */
  *CCA = Uncached; /* not used for isHOST */

  /* NOTE: This is a duplicate of the code that appears in the
     LoadMemory and StoreMemory functions. They should be merged into
     a single function (that can be in-lined if required). */
  if ((vAddr >= STATE_MEM_BASE (sd)) && (vAddr < (STATE_MEM_BASE (sd) + STATE_MEM_SIZE (sd)))) {
    if (host)
     *pAddr = (int)&STATE_MEMORY (sd)[((unsigned int)(vAddr - STATE_MEM_BASE (sd)) & (STATE_MEM_SIZE (sd) - 1))];
  } else if ((vAddr >= monitor_base) && (vAddr < (monitor_base + monitor_size))) {
    if (host)
     *pAddr = (int)&monitor[((unsigned int)(vAddr - monitor_base) & (monitor_size - 1))];
  } else {
#ifdef DEBUG
    sim_warning("Failed: AddressTranslation(0x%s,%s,%s,...) IPC = 0x%s",pr_addr(vAddr),(IorD ? "isDATA" : "isINSTRUCTION"),(LorS ? "isSTORE" : "isLOAD"),pr_addr(IPC));
#endif /* DEBUG */
    res = 0; /* AddressTranslation has failed */
    *pAddr = (SIM_ADDR)-1;
    if (!raw) /* only generate exceptions on real memory transfers */
     SignalException((LorS == isSTORE) ? AddressStore : AddressLoad);
#ifdef DEBUG
    else
     /* This is a normal occurance during gdb operation, for instance trying
	to print parameters at function start before they have been setup,
	and hence we should not print a warning except when debugging the
	simulator.  */
     sim_warning("AddressTranslation for %s %s from 0x%s failed",(IorD ? "data" : "instruction"),(LorS ? "store" : "load"),pr_addr(vAddr));
#endif
  }

  return(res);
}

/* Description from page A-23 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* Prefetch data from memory. Prefetch is an advisory instruction for
   which an implementation specific action is taken. The action taken
   may increase performance, but must not change the meaning of the
   program, or alter architecturally-visible state. */

static void UNUSED
Prefetch(CCA,pAddr,vAddr,DATA,hint)
     int CCA;
     uword64 pAddr;
     uword64 vAddr;
     int DATA;
     int hint;
{
#ifdef DEBUG
  callback->printf_filtered(callback,"Prefetch(%d,0x%s,0x%s,%d,%d);\n",CCA,pr_addr(pAddr),pr_addr(vAddr),DATA,hint);
#endif /* DEBUG */

  /* For our simple memory model we do nothing */
  return;
}

/* Description from page A-22 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* Load a value from memory. Use the cache and main memory as
   specified in the Cache Coherence Algorithm (CCA) and the sort of
   access (IorD) to find the contents of AccessLength memory bytes
   starting at physical location pAddr. The data is returned in the
   fixed width naturally-aligned memory element (MemElem). The
   low-order two (or three) bits of the address and the AccessLength
   indicate which of the bytes within MemElem needs to be given to the
   processor. If the memory access type of the reference is uncached
   then only the referenced bytes are read from memory and valid
   within the memory element. If the access type is cached, and the
   data is not present in cache, an implementation specific size and
   alignment block of memory is read and loaded into the cache to
   satisfy a load reference. At a minimum, the block is the entire
   memory element. */
static void
LoadMemory(memvalp,memval1p,CCA,AccessLength,pAddr,vAddr,IorD,raw)
     uword64* memvalp;
     uword64* memval1p;
     int CCA;
     int AccessLength;
     uword64 pAddr;
     uword64 vAddr;
     int IorD;
     int raw;
{
  SIM_DESC sd = &simulator;
  uword64 value = 0;
  uword64 value1 = 0;

#ifdef DEBUG
  if (STATE_MEMORY (sd) == NULL)
   callback->printf_filtered(callback,"DBG: LoadMemory(%p,%p,%d,%d,0x%s,0x%s,%s,%s)\n",memvalp,memval1p,CCA,AccessLength,pr_addr(pAddr),pr_addr(vAddr),(IorD ? "isDATA" : "isINSTRUCTION"),(raw ? "isRAW" : "isREAL"));
#endif /* DEBUG */

#if defined(WARN_MEM)
  if (CCA != uncached)
   sim_warning("LoadMemory CCA (%d) is not uncached (currently all accesses treated as cached)",CCA);

  if (((pAddr & LOADDRMASK) + AccessLength) > LOADDRMASK) {
    /* In reality this should be a Bus Error */
    sim_error("AccessLength of %d would extend over %dbit aligned boundary for physical address 0x%s\n",AccessLength,(LOADDRMASK + 1)<<2,pr_addr(pAddr));
  }
#endif /* WARN_MEM */

  /* Decide which physical memory locations are being dealt with. At
     this point we should be able to split the pAddr bits into the
     relevant address map being simulated. If the "raw" variable is
     set, the memory read being performed should *NOT* update any I/O
     state or affect the CPU state. This also includes avoiding
     affecting statistics gathering. */

  /* If instruction fetch then we need to check that the two lo-order
     bits are zero, otherwise raise a InstructionFetch exception: */
  if ((IorD == isINSTRUCTION)
      && ((pAddr & 0x3) != 0)
      && (((pAddr & 0x1) != 0) || ((vAddr & 0x1) == 0)))
   SignalException(InstructionFetch);
  else {
    unsigned int index = 0;
    unsigned char *mem = NULL;

#if defined(TRACE)
    if (!raw)
     dotrace(tracefh,((IorD == isDATA) ? 0 : 2),(unsigned int)(pAddr&0xFFFFFFFF),(AccessLength + 1),"load%s",((IorD == isDATA) ? "" : " instruction"));
#endif /* TRACE */

    /* NOTE: Quicker methods of decoding the address space can be used
       when a real memory map is being simulated (i.e. using hi-order
       address bits to select device). */
    if ((pAddr >= STATE_MEM_BASE (sd)) && (pAddr < (STATE_MEM_BASE (sd) + STATE_MEM_SIZE (sd)))) {
      index = ((unsigned int)(pAddr - STATE_MEM_BASE (sd)) & (STATE_MEM_SIZE (sd) - 1));
      mem = STATE_MEMORY (sd);
    } else if ((pAddr >= monitor_base) && (pAddr < (monitor_base + monitor_size))) {
      index = ((unsigned int)(pAddr - monitor_base) & (monitor_size - 1));
      mem = monitor;
    }
    if (mem == NULL)
     sim_error("Simulator memory not found for physical address 0x%s\n",pr_addr(pAddr));
    else {
      /* If we obtained the endianness of the host, and it is the same
         as the target memory system we can optimise the memory
         accesses. However, without that information we must perform
         slow transfer, and hope that the compiler optimisation will
         merge successive loads. */

      /* In reality we should always be loading a doubleword value (or
         word value in 32bit memory worlds). The external code then
         extracts the required bytes. However, to keep performance
         high we only load the required bytes into the relevant
         slots. */
      if (BigEndianMem)
       switch (AccessLength) { /* big-endian memory */
         case AccessLength_QUADWORD :
          value1 |= ((uword64)mem[index++] << 56);
         case 14:   /* AccessLength is one less than datalen */
          value1 |= ((uword64)mem[index++] << 48);
         case 13:
          value1 |= ((uword64)mem[index++] << 40);
         case 12:
          value1 |= ((uword64)mem[index++] << 32);
         case 11:
          value1 |= ((unsigned int)mem[index++] << 24);
         case 10:
          value1 |= ((unsigned int)mem[index++] << 16);
         case 9:
          value1 |= ((unsigned int)mem[index++] << 8);
         case 8:
          value1 |= mem[index];

         case AccessLength_DOUBLEWORD :
          value |= ((uword64)mem[index++] << 56);
         case AccessLength_SEPTIBYTE :
          value |= ((uword64)mem[index++] << 48);
         case AccessLength_SEXTIBYTE :
          value |= ((uword64)mem[index++] << 40);
         case AccessLength_QUINTIBYTE :
          value |= ((uword64)mem[index++] << 32);
         case AccessLength_WORD :
          value |= ((unsigned int)mem[index++] << 24);
         case AccessLength_TRIPLEBYTE :
          value |= ((unsigned int)mem[index++] << 16);
         case AccessLength_HALFWORD :
          value |= ((unsigned int)mem[index++] << 8);
         case AccessLength_BYTE :
          value |= mem[index];
          break;
       }
      else {
        index += (AccessLength + 1);
        switch (AccessLength) { /* little-endian memory */
          case AccessLength_QUADWORD :
           value1 |= ((uword64)mem[--index] << 56);
         case 14:   /* AccessLength is one less than datalen */
           value1 |= ((uword64)mem[--index] << 48);
         case 13:
           value1 |= ((uword64)mem[--index] << 40);
         case 12:
           value1 |= ((uword64)mem[--index] << 32);
         case 11:
           value1 |= ((uword64)mem[--index] << 24);
         case 10:
           value1 |= ((uword64)mem[--index] << 16);
         case 9:
           value1 |= ((uword64)mem[--index] << 8);
         case 8:
           value1 |= ((uword64)mem[--index] << 0);

          case AccessLength_DOUBLEWORD :
           value |= ((uword64)mem[--index] << 56);
          case AccessLength_SEPTIBYTE :
           value |= ((uword64)mem[--index] << 48);
          case AccessLength_SEXTIBYTE :
           value |= ((uword64)mem[--index] << 40);
          case AccessLength_QUINTIBYTE :
           value |= ((uword64)mem[--index] << 32);
          case AccessLength_WORD :
           value |= ((uword64)mem[--index] << 24);
          case AccessLength_TRIPLEBYTE :
           value |= ((uword64)mem[--index] << 16);
          case AccessLength_HALFWORD :
           value |= ((uword64)mem[--index] << 8);
          case AccessLength_BYTE :
           value |= ((uword64)mem[--index] << 0);
           break;
        }
      }

#ifdef DEBUG
      printf("DBG: LoadMemory() : (offset %d) : value = 0x%s%s\n",
             (int)(pAddr & LOADDRMASK),pr_uword64(value1),pr_uword64(value));
#endif /* DEBUG */

      /* TODO: We could try and avoid the shifts when dealing with raw
         memory accesses. This would mean updating the LoadMemory and
         StoreMemory routines to avoid shifting the data before
         returning or using it. */
      if (AccessLength <= AccessLength_DOUBLEWORD) {
        if (!raw) { /* do nothing for raw accessess */
          if (BigEndianMem)
            value <<= (((7 - (pAddr & LOADDRMASK)) - AccessLength) * 8);
          else /* little-endian only needs to be shifted up to the correct byte offset */
            value <<= ((pAddr & LOADDRMASK) * 8);
        }
      }

#ifdef DEBUG
      printf("DBG: LoadMemory() : shifted value = 0x%s%s\n",
             pr_uword64(value1),pr_uword64(value));
#endif /* DEBUG */
    }
  }

*memvalp = value;
if (memval1p) *memval1p = value1;
}


/* Description from page A-23 of the "MIPS IV Instruction Set" manual
   (revision 3.1) */
/* Store a value to memory. The specified data is stored into the
   physical location pAddr using the memory hierarchy (data caches and
   main memory) as specified by the Cache Coherence Algorithm
   (CCA). The MemElem contains the data for an aligned, fixed-width
   memory element (word for 32-bit processors, doubleword for 64-bit
   processors), though only the bytes that will actually be stored to
   memory need to be valid. The low-order two (or three) bits of pAddr
   and the AccessLength field indicates which of the bytes within the
   MemElem data should actually be stored; only these bytes in memory
   will be changed. */

static void
StoreMemory(CCA,AccessLength,MemElem,MemElem1,pAddr,vAddr,raw)
     int CCA;
     int AccessLength;
     uword64 MemElem;
     uword64 MemElem1;   /* High order 64 bits */
     uword64 pAddr;
     uword64 vAddr;
     int raw;
{
  SIM_DESC sd = &simulator;
#ifdef DEBUG
  callback->printf_filtered(callback,"DBG: StoreMemory(%d,%d,0x%s,0x%s,0x%s,0x%s,%s)\n",CCA,AccessLength,pr_uword64(MemElem),pr_uword64(MemElem1),pr_addr(pAddr),pr_addr(vAddr),(raw ? "isRAW" : "isREAL"));
#endif /* DEBUG */

#if defined(WARN_MEM)
  if (CCA != uncached)
   sim_warning("StoreMemory CCA (%d) is not uncached (currently all accesses treated as cached)",CCA);
 
  if (((pAddr & LOADDRMASK) + AccessLength) > LOADDRMASK)
   sim_error("AccessLength of %d would extend over %dbit aligned boundary for physical address 0x%s\n",AccessLength,(LOADDRMASK + 1)<<2,pr_addr(pAddr));
#endif /* WARN_MEM */

#if defined(TRACE)
  if (!raw)
   dotrace(tracefh,1,(unsigned int)(pAddr&0xFFFFFFFF),(AccessLength + 1),"store");
#endif /* TRACE */

  /* See the comments in the LoadMemory routine about optimising
     memory accesses. Also if we wanted to make the simulator smaller,
     we could merge a lot of this code with the LoadMemory
     routine. However, this would slow the simulator down with
     run-time conditionals. */
  {
    unsigned int index = 0;
    unsigned char *mem = NULL;

    if ((pAddr >= STATE_MEM_BASE (sd)) && (pAddr < (STATE_MEM_BASE (sd) + STATE_MEM_SIZE (sd)))) {
      index = ((unsigned int)(pAddr - STATE_MEM_BASE (sd)) & (STATE_MEM_SIZE (sd) - 1));
      mem = STATE_MEMORY (sd);
    } else if ((pAddr >= monitor_base) && (pAddr < (monitor_base + monitor_size))) {
      index = ((unsigned int)(pAddr - monitor_base) & (monitor_size - 1));
      mem = monitor;
    }

    if (mem == NULL)
     sim_error("Simulator memory not found for physical address 0x%s\n",pr_addr(pAddr));
    else {
      int shift = 0;

#ifdef DEBUG
      printf("DBG: StoreMemory: offset = %d MemElem = 0x%s%s\n",(unsigned int)(pAddr & LOADDRMASK),pr_uword64(MemElem1),pr_uword64(MemElem));
#endif /* DEBUG */

      if (AccessLength <= AccessLength_DOUBLEWORD) {
        if (BigEndianMem) {
          if (raw)
            shift = ((7 - AccessLength) * 8);
          else /* real memory access */
            shift = ((pAddr & LOADDRMASK) * 8);
          MemElem <<= shift;
        } else {
          /* no need to shift raw little-endian data */
          if (!raw)
            MemElem >>= ((pAddr & LOADDRMASK) * 8);
        }
      }

#ifdef DEBUG
      printf("DBG: StoreMemory: shift = %d MemElem = 0x%s%s\n",shift,pr_uword64(MemElem1),pr_uword64(MemElem));
#endif /* DEBUG */

      if (BigEndianMem) {
        switch (AccessLength) { /* big-endian memory */
          case AccessLength_QUADWORD :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 14 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 13 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 12 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 11 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 10 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 9 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);
           MemElem1 <<= 8;
          case 8 :
           mem[index++] = (unsigned char)(MemElem1 >> 56);

          case AccessLength_DOUBLEWORD :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_SEPTIBYTE :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_SEXTIBYTE :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_QUINTIBYTE :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_WORD :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_TRIPLEBYTE :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_HALFWORD :
           mem[index++] = (unsigned char)(MemElem >> 56);
           MemElem <<= 8;
          case AccessLength_BYTE :
           mem[index++] = (unsigned char)(MemElem >> 56);
           break;
        }
      } else {
        index += (AccessLength + 1);
        switch (AccessLength) { /* little-endian memory */
          case AccessLength_QUADWORD :
           mem[--index] = (unsigned char)(MemElem1 >> 56);
          case 14 :
           mem[--index] = (unsigned char)(MemElem1 >> 48);
          case 13 :
           mem[--index] = (unsigned char)(MemElem1 >> 40);
          case 12 :
           mem[--index] = (unsigned char)(MemElem1 >> 32);
          case 11 :
           mem[--index] = (unsigned char)(MemElem1 >> 24);
          case 10 :
           mem[--index] = (unsigned char)(MemElem1 >> 16);
          case 9 :
           mem[--index] = (unsigned char)(MemElem1 >> 8);
          case 8 :
           mem[--index] = (unsigned char)(MemElem1 >> 0);

          case AccessLength_DOUBLEWORD :
           mem[--index] = (unsigned char)(MemElem >> 56);
          case AccessLength_SEPTIBYTE :
           mem[--index] = (unsigned char)(MemElem >> 48);
          case AccessLength_SEXTIBYTE :
           mem[--index] = (unsigned char)(MemElem >> 40);
          case AccessLength_QUINTIBYTE :
           mem[--index] = (unsigned char)(MemElem >> 32);
          case AccessLength_WORD :
           mem[--index] = (unsigned char)(MemElem >> 24);
          case AccessLength_TRIPLEBYTE :
           mem[--index] = (unsigned char)(MemElem >> 16);
          case AccessLength_HALFWORD :
           mem[--index] = (unsigned char)(MemElem >> 8);
          case AccessLength_BYTE :
           mem[--index] = (unsigned char)(MemElem >> 0);
           break;
        }
      }
    }
  }

  return;
}


/* Description from page A-26 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* Order loads and stores to synchronise shared memory. Perform the
   action necessary to make the effects of groups of synchronizable
   loads and stores indicated by stype occur in the same order for all
   processors. */
static void
SyncOperation(stype)
     int stype;
{
#ifdef DEBUG
  callback->printf_filtered(callback,"SyncOperation(%d) : TODO\n",stype);
#endif /* DEBUG */
  return;
}

/* Description from page A-26 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* Signal an exception condition. This will result in an exception
   that aborts the instruction. The instruction operation pseudocode
   will never see a return from this function call. */

static void
SignalException (int exception,...)
{
  int vector;
  SIM_DESC sd = &simulator;
  /* Ensure that any active atomic read/modify/write operation will fail: */
  LLBIT = 0;

  switch (exception) {
    /* TODO: For testing purposes I have been ignoring TRAPs. In
       reality we should either simulate them, or allow the user to
       ignore them at run-time. */
    case Trap :
     sim_warning("Ignoring instruction TRAP (PC 0x%s)",pr_addr(IPC));
     break;

    case ReservedInstruction :
     {
       va_list ap;
       unsigned int instruction;
       va_start(ap,exception);
       instruction = va_arg(ap,unsigned int);
       va_end(ap);
       /* Provide simple monitor support using ReservedInstruction
          exceptions. The following code simulates the fixed vector
          entry points into the IDT monitor by causing a simulator
          trap, performing the monitor operation, and returning to
          the address held in the $ra register (standard PCS return
          address). This means we only need to pre-load the vector
          space with suitable instruction values. For systems were
          actual trap instructions are used, we would not need to
          perform this magic. */
       if ((instruction & RSVD_INSTRUCTION_MASK) == RSVD_INSTRUCTION) {
         sim_monitor( ((instruction >> RSVD_INSTRUCTION_ARG_SHIFT) & RSVD_INSTRUCTION_ARG_MASK) );
         PC = RA; /* simulate the return from the vector entry */
         /* NOTE: This assumes that a branch-and-link style
            instruction was used to enter the vector (which is the
            case with the current IDT monitor). */
	 sim_engine_restart (sd, STATE_CPU (sd, 0), NULL, NULL_CIA);
       }
       /* Look for the mips16 entry and exit instructions, and
          simulate a handler for them.  */
       else if ((IPC & 1) != 0
		&& (instruction & 0xf81f) == 0xe809
		&& (instruction & 0x0c0) != 0x0c0) {
	 mips16_entry (instruction);
	 sim_engine_restart (sd, STATE_CPU (sd, 0), NULL, NULL_CIA);
       } /* else fall through to normal exception processing */
       sim_warning("ReservedInstruction 0x%08X at IPC = 0x%s",instruction,pr_addr(IPC));
     }

    case BreakPoint:
#ifdef DEBUG
	callback->printf_filtered(callback,"DBG: SignalException(%d) IPC = 0x%s\n",exception,pr_addr(IPC));
#endif /* DEBUG */
      /* Keep a copy of the current A0 in-case this is the program exit
	 breakpoint:  */
      {
	va_list ap;
	unsigned int instruction;
	va_start(ap,exception);
	instruction = va_arg(ap,unsigned int);
	va_end(ap);
	/* Check for our special terminating BREAK: */
	if ((instruction & 0x03FFFFC0) == 0x03ff0000) {
	  sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			   sim_exited, (unsigned int)(A0 & 0xFFFFFFFF));
	}
      }
      if (state & simDELAYSLOT)
	PC = IPC - 4; /* reference the branch instruction */
      else
	PC = IPC;
      sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
		       sim_stopped, SIGTRAP);

    default:
     /* Store exception code into current exception id variable (used
        by exit code): */

     /* TODO: If not simulating exceptions then stop the simulator
        execution. At the moment we always stop the simulation. */

     /* See figure 5-17 for an outline of the code below */
     if (! (SR & status_EXL))
       {
	 CAUSE = (exception << 2);
	 if (state & simDELAYSLOT)
	   {
	     state &= ~simDELAYSLOT;
	     CAUSE |= cause_BD;
	     EPC = (IPC - 4); /* reference the branch instruction */
	   }
	 else
	   EPC = IPC;
	 /* FIXME: TLB et.al. */
	 vector = 0x180;
       }
     else
       {
	 CAUSE = (exception << 2);
	 vector = 0x180;
       }
     SR |= status_EXL;
     /* Store exception code into current exception id variable (used
        by exit code): */
     if (SR & status_BEV)
       PC = (signed)0xBFC00200 + 0x180;
     else
       PC = (signed)0x80000000 + 0x180;

     switch ((CAUSE >> 2) & 0x1F)
       {
       case Interrupt:
	 /* Interrupts arrive during event processing, no need to
            restart */
	 return;
	 
       case TLBModification:
       case TLBLoad:
       case TLBStore:
       case AddressLoad:
       case AddressStore:
       case InstructionFetch:
       case DataReference:
	 /* The following is so that the simulator will continue from the
	    exception address on breakpoint operations. */
	 PC = EPC;
	 sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			  sim_stopped, SIGBUS);

       case ReservedInstruction:
       case CoProcessorUnusable:
	 PC = EPC;
	 sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			  sim_stopped, SIGILL);

       case IntegerOverflow:
       case FPE:
	 sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			  sim_stopped, SIGFPE);

       case Trap:
       case Watch:
       case SystemCall:
	 PC = EPC;
	 sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			  sim_stopped, SIGTRAP);

       case BreakPoint:
	 PC = EPC;
	 sim_engine_abort (sd, STATE_CPU (sd, 0), NULL_CIA,
			   "FATAL: Should not encounter a breakpoint\n");

       default : /* Unknown internal exception */
	 PC = EPC;
	 sim_engine_halt (sd, STATE_CPU (sd, 0), NULL, NULL_CIA,
			  sim_stopped, SIGQUIT);

       }

    case SimulatorFault:
     {
       va_list ap;
       char *msg;
       va_start(ap,exception);
       msg = va_arg(ap,char *);
       va_end(ap);
       sim_engine_abort (sd, STATE_CPU (sd, 0), NULL_CIA,
			 "FATAL: Simulator error \"%s\"\n",msg);
     }
   }

  return;
}

#if defined(WARN_RESULT)
/* Description from page A-26 of the "MIPS IV Instruction Set" manual (revision 3.1) */
/* This function indicates that the result of the operation is
   undefined. However, this should not affect the instruction
   stream. All that is meant to happen is that the destination
   register is set to an undefined result. To keep the simulator
   simple, we just don't bother updating the destination register, so
   the overall result will be undefined. If desired we can stop the
   simulator by raising a pseudo-exception. */
static void
UndefinedResult()
{
  sim_warning("UndefinedResult: IPC = 0x%s",pr_addr(IPC));
#if 0 /* Disabled for the moment, since it actually happens a lot at the moment. */
  state |= simSTOP;
#endif
  return;
}
#endif /* WARN_RESULT */

static void UNUSED
CacheOp(op,pAddr,vAddr,instruction)
     int op;
     uword64 pAddr;
     uword64 vAddr;
     unsigned int instruction;
{
#if 1 /* stop warning message being displayed (we should really just remove the code) */
  static int icache_warning = 1;
  static int dcache_warning = 1;
#else
  static int icache_warning = 0;
  static int dcache_warning = 0;
#endif

  /* If CP0 is not useable (User or Supervisor mode) and the CP0
     enable bit in the Status Register is clear - a coprocessor
     unusable exception is taken. */
#if 0
  callback->printf_filtered(callback,"TODO: Cache availability checking (PC = 0x%s)\n",pr_addr(IPC));
#endif

  switch (op & 0x3) {
    case 0: /* instruction cache */
      switch (op >> 2) {
        case 0: /* Index Invalidate */
        case 1: /* Index Load Tag */
        case 2: /* Index Store Tag */
        case 4: /* Hit Invalidate */
        case 5: /* Fill */
        case 6: /* Hit Writeback */
          if (!icache_warning)
            {
              sim_warning("Instruction CACHE operation %d to be coded",(op >> 2));
              icache_warning = 1;
            }
          break;

        default:
          SignalException(ReservedInstruction,instruction);
          break;
      }
      break;

    case 1: /* data cache */
      switch (op >> 2) {
        case 0: /* Index Writeback Invalidate */
        case 1: /* Index Load Tag */
        case 2: /* Index Store Tag */
        case 3: /* Create Dirty */
        case 4: /* Hit Invalidate */
        case 5: /* Hit Writeback Invalidate */
        case 6: /* Hit Writeback */ 
          if (!dcache_warning)
            {
              sim_warning("Data CACHE operation %d to be coded",(op >> 2));
              dcache_warning = 1;
            }
          break;

        default:
          SignalException(ReservedInstruction,instruction);
          break;
      }
      break;

    default: /* unrecognised cache ID */
      SignalException(ReservedInstruction,instruction);
      break;
  }

  return;
}

/*-- FPU support routines ---------------------------------------------------*/

#if defined(HASFPU) /* Only needed when building FPU aware simulators */

#if 1
#define SizeFGR() (GPRLEN)
#else
/* They depend on the CPU being simulated */
#define SizeFGR() ((PROCESSOR_64BIT && ((SR & status_FR) == 1)) ? 64 : 32)
#endif

/* Numbers are held in normalized form. The SINGLE and DOUBLE binary
   formats conform to ANSI/IEEE Std 754-1985. */
/* SINGLE precision floating:
 *    seeeeeeeefffffffffffffffffffffff
 *      s =  1bit  = sign
 *      e =  8bits = exponent
 *      f = 23bits = fraction
 */
/* SINGLE precision fixed:
 *    siiiiiiiiiiiiiiiiiiiiiiiiiiiiiii
 *      s =  1bit  = sign
 *      i = 31bits = integer
 */
/* DOUBLE precision floating:
 *    seeeeeeeeeeeffffffffffffffffffffffffffffffffffffffffffffffffffff
 *      s =  1bit  = sign
 *      e = 11bits = exponent
 *      f = 52bits = fraction
 */
/* DOUBLE precision fixed:
 *    siiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii
 *      s =  1bit  = sign
 *      i = 63bits = integer
 */

/* Extract sign-bit: */
#define FP_S_s(v)    (((v) & ((unsigned)1 << 31)) ? 1 : 0)
#define FP_D_s(v)    (((v) & ((uword64)1 << 63)) ? 1 : 0)
/* Extract biased exponent: */
#define FP_S_be(v)   (((v) >> 23) & 0xFF)
#define FP_D_be(v)   (((v) >> 52) & 0x7FF)
/* Extract unbiased Exponent: */
#define FP_S_e(v)    (FP_S_be(v) - 0x7F)
#define FP_D_e(v)    (FP_D_be(v) - 0x3FF)
/* Extract complete fraction field: */
#define FP_S_f(v)    ((v) & ~((unsigned)0x1FF << 23))
#define FP_D_f(v)    ((v) & ~((uword64)0xFFF << 52))
/* Extract numbered fraction bit: */
#define FP_S_fb(b,v) (((v) & (1 << (23 - (b)))) ? 1 : 0)
#define FP_D_fb(b,v) (((v) & (1 << (52 - (b)))) ? 1 : 0)

/* Explicit QNaN values used when value required: */
#define FPQNaN_SINGLE   (0x7FBFFFFF)
#define FPQNaN_WORD     (0x7FFFFFFF)
#define FPQNaN_DOUBLE   (((uword64)0x7FF7FFFF << 32) | 0xFFFFFFFF)
#define FPQNaN_LONG     (((uword64)0x7FFFFFFF << 32) | 0xFFFFFFFF)

/* Explicit Infinity values used when required: */
#define FPINF_SINGLE    (0x7F800000)
#define FPINF_DOUBLE    (((uword64)0x7FF00000 << 32) | 0x00000000)

#if 1 /* def DEBUG */
#define RMMODE(v) (((v) == FP_RM_NEAREST) ? "Round" : (((v) == FP_RM_TOZERO) ? "Trunc" : (((v) == FP_RM_TOPINF) ? "Ceil" : "Floor")))
#define DOFMT(v)  (((v) == fmt_single) ? "single" : (((v) == fmt_double) ? "double" : (((v) == fmt_word) ? "word" : (((v) == fmt_long) ? "long" : (((v) == fmt_unknown) ? "<unknown>" : (((v) == fmt_uninterpreted) ? "<uninterpreted>" : "<format error>"))))))
#endif /* DEBUG */

static uword64
ValueFPR(fpr,fmt)
         int fpr;
         FP_formats fmt;
{
  uword64 value = 0;
  int err = 0;

  /* Treat unused register values, as fixed-point 64bit values: */
  if ((fmt == fmt_uninterpreted) || (fmt == fmt_unknown))
#if 1
   /* If request to read data as "uninterpreted", then use the current
      encoding: */
   fmt = fpr_state[fpr];
#else
   fmt = fmt_long;
#endif

  /* For values not yet accessed, set to the desired format: */
  if (fpr_state[fpr] == fmt_uninterpreted) {
    fpr_state[fpr] = fmt;
#ifdef DEBUG
    printf("DBG: Register %d was fmt_uninterpreted. Now %s\n",fpr,DOFMT(fmt));
#endif /* DEBUG */
  }
  if (fmt != fpr_state[fpr]) {
    sim_warning("FPR %d (format %s) being accessed with format %s - setting to unknown (PC = 0x%s)",fpr,DOFMT(fpr_state[fpr]),DOFMT(fmt),pr_addr(IPC));
    fpr_state[fpr] = fmt_unknown;
  }

  if (fpr_state[fpr] == fmt_unknown) {
   /* Set QNaN value: */
   switch (fmt) {
    case fmt_single:
     value = FPQNaN_SINGLE;
     break;

    case fmt_double:
     value = FPQNaN_DOUBLE;
     break;

    case fmt_word:
     value = FPQNaN_WORD;
     break;

    case fmt_long:
     value = FPQNaN_LONG;
     break;

    default:
     err = -1;
     break;
   }
  } else if (SizeFGR() == 64) {
    switch (fmt) {
     case fmt_single:
     case fmt_word:
      value = (FGR[fpr] & 0xFFFFFFFF);
      break;

     case fmt_uninterpreted:
     case fmt_double:
     case fmt_long:
      value = FGR[fpr];
      break;

     default :
      err = -1;
      break;
    }
  } else {
    switch (fmt) {
     case fmt_single:
     case fmt_word:
      value = (FGR[fpr] & 0xFFFFFFFF);
      break;

     case fmt_uninterpreted:
     case fmt_double:
     case fmt_long:
      if ((fpr & 1) == 0) { /* even registers only */
	value = ((((uword64)FGR[fpr+1]) << 32) | (FGR[fpr] & 0xFFFFFFFF));
      } else {
	SignalException (ReservedInstruction, 0);
      }
      break;

     default :
      err = -1;
      break;
    }
  }

  if (err)
   SignalException(SimulatorFault,"Unrecognised FP format in ValueFPR()");

#ifdef DEBUG
  printf("DBG: ValueFPR: fpr = %d, fmt = %s, value = 0x%s : PC = 0x%s : SizeFGR() = %d\n",fpr,DOFMT(fmt),pr_addr(value),pr_addr(IPC),SizeFGR());
#endif /* DEBUG */

  return(value);
}

static void
StoreFPR(fpr,fmt,value)
     int fpr;
     FP_formats fmt;
     uword64 value;
{
  int err = 0;

#ifdef DEBUG
  printf("DBG: StoreFPR: fpr = %d, fmt = %s, value = 0x%s : PC = 0x%s : SizeFGR() = %d\n",fpr,DOFMT(fmt),pr_addr(value),pr_addr(IPC),SizeFGR());
#endif /* DEBUG */

  if (SizeFGR() == 64) {
    switch (fmt) {
      case fmt_single :
      case fmt_word :
       FGR[fpr] = (((uword64)0xDEADC0DE << 32) | (value & 0xFFFFFFFF));
       fpr_state[fpr] = fmt;
       break;

      case fmt_uninterpreted:
      case fmt_double :
      case fmt_long :
       FGR[fpr] = value;
       fpr_state[fpr] = fmt;
       break;

      default :
       fpr_state[fpr] = fmt_unknown;
       err = -1;
       break;
    }
  } else {
    switch (fmt) {
      case fmt_single :
      case fmt_word :
       FGR[fpr] = (value & 0xFFFFFFFF);
       fpr_state[fpr] = fmt;
       break;

      case fmt_uninterpreted:
      case fmt_double :
      case fmt_long :
	if ((fpr & 1) == 0) { /* even register number only */
	  FGR[fpr+1] = (value >> 32);
	  FGR[fpr] = (value & 0xFFFFFFFF);
	  fpr_state[fpr + 1] = fmt;
	  fpr_state[fpr] = fmt;
	} else {
	  fpr_state[fpr] = fmt_unknown;
	  fpr_state[fpr + 1] = fmt_unknown;
	  SignalException (ReservedInstruction, 0);
	}
       break;

      default :
       fpr_state[fpr] = fmt_unknown;
       err = -1;
       break;
    }
  }
#if defined(WARN_RESULT)
  else
    UndefinedResult();
#endif /* WARN_RESULT */

  if (err)
   SignalException(SimulatorFault,"Unrecognised FP format in StoreFPR()");

#ifdef DEBUG
  printf("DBG: StoreFPR: fpr[%d] = 0x%s (format %s)\n",fpr,pr_addr(FGR[fpr]),DOFMT(fmt));
#endif /* DEBUG */

  return;
}

static int
NaN(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  int boolean = 0;

  /* Check if (((E - bias) == (E_max + 1)) && (fraction != 0)). We
     know that the exponent field is biased... we we cheat and avoid
     removing the bias value. */
  switch (fmt) {
   case fmt_single:
    boolean = ((FP_S_be(op) == 0xFF) && (FP_S_f(op) != 0));
    /* We could use "FP_S_fb(1,op)" to ascertain whether we are
       dealing with a SNaN or QNaN */
    break;
   case fmt_double:
    boolean = ((FP_D_be(op) == 0x7FF) && (FP_D_f(op) != 0));
    /* We could use "FP_S_fb(1,op)" to ascertain whether we are
       dealing with a SNaN or QNaN */
    break;
   case fmt_word:
    boolean = (op == FPQNaN_WORD);
    break;
   case fmt_long:
    boolean = (op == FPQNaN_LONG);
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
printf("DBG: NaN: returning %d for 0x%s (format = %s)\n",boolean,pr_addr(op),DOFMT(fmt));
#endif /* DEBUG */

  return(boolean);
}

static int
Infinity(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  int boolean = 0;

#ifdef DEBUG
  printf("DBG: Infinity: format %s 0x%s (PC = 0x%s)\n",DOFMT(fmt),pr_addr(op),pr_addr(IPC));
#endif /* DEBUG */

  /* Check if (((E - bias) == (E_max + 1)) && (fraction == 0)). We
     know that the exponent field is biased... we we cheat and avoid
     removing the bias value. */
  switch (fmt) {
   case fmt_single:
    boolean = ((FP_S_be(op) == 0xFF) && (FP_S_f(op) == 0));
    break;
   case fmt_double:
    boolean = ((FP_D_be(op) == 0x7FF) && (FP_D_f(op) == 0));
    break;
   default:
    printf("DBG: TODO: unrecognised format (%s) for Infinity check\n",DOFMT(fmt));
    break;
  }

#ifdef DEBUG
  printf("DBG: Infinity: returning %d for 0x%s (format = %s)\n",boolean,pr_addr(op),DOFMT(fmt));
#endif /* DEBUG */

  return(boolean);
}

static int
Less(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  int boolean = 0;

  /* Argument checking already performed by the FPCOMPARE code */

#ifdef DEBUG
  printf("DBG: Less: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop1 = (unsigned int)op1;
      unsigned int wop2 = (unsigned int)op2;
      boolean = (*(float *)&wop1 < *(float *)&wop2);
    }
    break;
   case fmt_double:
    boolean = (*(double *)&op1 < *(double *)&op2);
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Less: returning %d (format = %s)\n",boolean,DOFMT(fmt));
#endif /* DEBUG */

  return(boolean);
}

static int
Equal(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  int boolean = 0;

  /* Argument checking already performed by the FPCOMPARE code */

#ifdef DEBUG
  printf("DBG: Equal: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    boolean = ((op1 & 0xFFFFFFFF) == (op2 & 0xFFFFFFFF));
    break;
   case fmt_double:
    boolean = (op1 == op2);
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Equal: returning %d (format = %s)\n",boolean,DOFMT(fmt));
#endif /* DEBUG */

  return(boolean);
}

static uword64
AbsoluteValue(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: AbsoluteValue: %s: op = 0x%s\n",DOFMT(fmt),pr_addr(op));
#endif /* DEBUG */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop = (unsigned int)op;
      float tmp = ((float)fabs((double)*(float *)&wop));
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = (fabs(*(double *)&op));
      result = *(uword64 *)&tmp;
    }
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

  return(result);
}

static uword64
Negate(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Negate: %s: op = 0x%s\n",DOFMT(fmt),pr_addr(op));
#endif /* DEBUG */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop = (unsigned int)op;
      float tmp = ((float)0.0 - *(float *)&wop);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = ((double)0.0 - *(double *)&op);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

  return(result);
}

static uword64
Add(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Add: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop1 = (unsigned int)op1;
      unsigned int wop2 = (unsigned int)op2;
      float tmp = (*(float *)&wop1 + *(float *)&wop2);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = (*(double *)&op1 + *(double *)&op2);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Add: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64
Sub(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Sub: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop1 = (unsigned int)op1;
      unsigned int wop2 = (unsigned int)op2;
      float tmp = (*(float *)&wop1 - *(float *)&wop2);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = (*(double *)&op1 - *(double *)&op2);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Sub: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64
Multiply(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Multiply: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop1 = (unsigned int)op1;
      unsigned int wop2 = (unsigned int)op2;
      float tmp = (*(float *)&wop1 * *(float *)&wop2);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = (*(double *)&op1 * *(double *)&op2);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Multiply: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64
Divide(op1,op2,fmt)
     uword64 op1;
     uword64 op2;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Divide: %s: op1 = 0x%s : op2 = 0x%s\n",DOFMT(fmt),pr_addr(op1),pr_addr(op2));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop1 = (unsigned int)op1;
      unsigned int wop2 = (unsigned int)op2;
      float tmp = (*(float *)&wop1 / *(float *)&wop2);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = (*(double *)&op1 / *(double *)&op2);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Divide: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64 UNUSED
Recip(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Recip: %s: op = 0x%s\n",DOFMT(fmt),pr_addr(op));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop = (unsigned int)op;
      float tmp = ((float)1.0 / *(float *)&wop);
      result = (uword64)*(unsigned int *)&tmp;
    }
    break;
   case fmt_double:
    {
      double tmp = ((double)1.0 / *(double *)&op);
      result = *(uword64 *)&tmp;
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Recip: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64
SquareRoot(op,fmt)
     uword64 op;
     FP_formats fmt; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: SquareRoot: %s: op = 0x%s\n",DOFMT(fmt),pr_addr(op));
#endif /* DEBUG */

  /* The registers must specify FPRs valid for operands of type
     "fmt". If they are not valid, the result is undefined. */

  /* The format type should already have been checked: */
  switch (fmt) {
   case fmt_single:
    {
      unsigned int wop = (unsigned int)op;
#ifdef HAVE_SQRT
      float tmp = ((float)sqrt((double)*(float *)&wop));
      result = (uword64)*(unsigned int *)&tmp;
#else
      /* TODO: Provide square-root */
      result = (uword64)0;
#endif
    }
    break;
   case fmt_double:
    {
#ifdef HAVE_SQRT
      double tmp = (sqrt(*(double *)&op));
      result = *(uword64 *)&tmp;
#else
      /* TODO: Provide square-root */
      result = (uword64)0;
#endif
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: SquareRoot: returning 0x%s (format = %s)\n",pr_addr(result),DOFMT(fmt));
#endif /* DEBUG */

  return(result);
}

static uword64
Convert(rm,op,from,to)
     int rm;
     uword64 op;
     FP_formats from; 
     FP_formats to; 
{
  uword64 result = 0;

#ifdef DEBUG
  printf("DBG: Convert: mode %s : op 0x%s : from %s : to %s : (PC = 0x%s)\n",RMMODE(rm),pr_addr(op),DOFMT(from),DOFMT(to),pr_addr(IPC));
#endif /* DEBUG */

  /* The value "op" is converted to the destination format, rounding
     using mode "rm". When the destination is a fixed-point format,
     then a source value of Infinity, NaN or one which would round to
     an integer outside the fixed point range then an IEEE Invalid
     Operation condition is raised. */
  switch (to) {
   case fmt_single:
    {
      float tmp;
      switch (from) {
       case fmt_double:
        tmp = (float)(*(double *)&op);
        break;

       case fmt_word:
        tmp = (float)((int)(op & 0xFFFFFFFF));
        break;

       case fmt_long:
        tmp = (float)((word64)op);
        break;
       default:
	fprintf (stderr, "Bad switch\n");
	abort ();
      }

#if 0
      /* FIXME: This code is incorrect.  The rounding mode does not
         round to integral values; it rounds to the nearest
         representable value in the format.  */

      switch (rm) {
       case FP_RM_NEAREST:
        /* Round result to nearest representable value. When two
           representable values are equally near, round to the value
           that has a least significant bit of zero (i.e. is even). */
#ifdef HAVE_ANINT
        tmp = (float)anint((double)tmp);
#else
        /* TODO: Provide round-to-nearest */
#endif
        break;

       case FP_RM_TOZERO:
        /* Round result to the value closest to, and not greater in
           magnitude than, the result. */
#ifdef HAVE_AINT
        tmp = (float)aint((double)tmp);
#else
        /* TODO: Provide round-to-zero */
#endif
        break;

       case FP_RM_TOPINF:
        /* Round result to the value closest to, and not less than,
           the result. */
        tmp = (float)ceil((double)tmp);
        break;

       case FP_RM_TOMINF:
        /* Round result to the value closest to, and not greater than,
           the result. */
        tmp = (float)floor((double)tmp);
        break;
      }
#endif /* 0 */

      result = (uword64)*(unsigned int *)&tmp;
    }
    break;

   case fmt_double:
    {
      double tmp;
      word64 xxx;

      switch (from) {
       case fmt_single:
        {
          unsigned int wop = (unsigned int)op;
          tmp = (double)(*(float *)&wop);
        }
        break;

       case fmt_word:
        xxx = SIGNEXTEND((op & 0xFFFFFFFF),32);
        tmp = (double)xxx;
        break;

       case fmt_long:
        tmp = (double)((word64)op);
        break;

       default:
        fprintf (stderr, "Bad switch\n");
        abort ();
      }

#if 0
      /* FIXME: This code is incorrect.  The rounding mode does not
         round to integral values; it rounds to the nearest
         representable value in the format.  */

      switch (rm) {
       case FP_RM_NEAREST:
#ifdef HAVE_ANINT
        tmp = anint(*(double *)&tmp);
#else
        /* TODO: Provide round-to-nearest */
#endif
        break;

       case FP_RM_TOZERO:
#ifdef HAVE_AINT
        tmp = aint(*(double *)&tmp);
#else
        /* TODO: Provide round-to-zero */
#endif
        break;

       case FP_RM_TOPINF:
        tmp = ceil(*(double *)&tmp);
        break;

       case FP_RM_TOMINF:
        tmp = floor(*(double *)&tmp);
        break;
      }
#endif /* 0 */

      result = *(uword64 *)&tmp;
    }
    break;

   case fmt_word:
   case fmt_long:
    if (Infinity(op,from) || NaN(op,from) || (1 == 0/*TODO: check range */)) {
      printf("DBG: TODO: update FCSR\n");
      SignalException(FPE);
    } else {
      if (to == fmt_word) {
        int tmp = 0;
        switch (from) {
         case fmt_single:
          {
            unsigned int wop = (unsigned int)op;
            tmp = (int)*((float *)&wop);
          }
          break;
         case fmt_double:
          tmp = (int)*((double *)&op);
#ifdef DEBUG
          printf("DBG: from double %.30f (0x%s) to word: 0x%08X\n",*((double *)&op),pr_addr(op),tmp);
#endif /* DEBUG */
          break;
	 default:
	  fprintf (stderr, "Bad switch\n");
	  abort ();
        }
        result = (uword64)tmp;
      } else { /* fmt_long */
	word64 tmp = 0;
        switch (from) {
         case fmt_single:
          {
            unsigned int wop = (unsigned int)op;
            tmp = (word64)*((float *)&wop);
          }
          break;
         case fmt_double:
          tmp = (word64)*((double *)&op);
          break;
	 default:
	  fprintf (stderr, "Bad switch\n");
	  abort ();
        }
	result = (uword64)tmp;
      }
    }
    break;
   default:
    fprintf (stderr, "Bad switch\n");
    abort ();
  }

#ifdef DEBUG
  printf("DBG: Convert: returning 0x%s (to format = %s)\n",pr_addr(result),DOFMT(to));
#endif /* DEBUG */

  return(result);
}
#endif /* HASFPU */

/*-- co-processor support routines ------------------------------------------*/

static int UNUSED
CoProcPresent(coproc_number)
     unsigned int coproc_number;
{
  /* Return TRUE if simulator provides a model for the given co-processor number */
  return(0);
}

static void
COP_LW(coproc_num,coproc_reg,memword)
     int coproc_num, coproc_reg;
     unsigned int memword;
{
  switch (coproc_num) {
#if defined(HASFPU)
    case 1:
#ifdef DEBUG
    printf("DBG: COP_LW: memword = 0x%08X (uword64)memword = 0x%s\n",memword,pr_addr(memword));
#endif
     StoreFPR(coproc_reg,fmt_word,(uword64)memword);
     fpr_state[coproc_reg] = fmt_uninterpreted;
     break;
#endif /* HASFPU */

    default:
#if 0 /* this should be controlled by a configuration option */
     callback->printf_filtered(callback,"COP_LW(%d,%d,0x%08X) at IPC = 0x%s : TODO (architecture specific)\n",coproc_num,coproc_reg,memword,pr_addr(IPC));
#endif
     break;
  }

  return;
}

static void
COP_LD(coproc_num,coproc_reg,memword)
     int coproc_num, coproc_reg;
     uword64 memword;
{
  switch (coproc_num) {
#if defined(HASFPU)
    case 1:
     StoreFPR(coproc_reg,fmt_uninterpreted,memword);
     break;
#endif /* HASFPU */

    default:
#if 0 /* this message should be controlled by a configuration option */
     callback->printf_filtered(callback,"COP_LD(%d,%d,0x%s) at IPC = 0x%s : TODO (architecture specific)\n",coproc_num,coproc_reg,pr_addr(memword),pr_addr(IPC));
#endif
     break;
  }

  return;
}

static unsigned int
COP_SW(coproc_num,coproc_reg)
     int coproc_num, coproc_reg;
{
  unsigned int value = 0;
  FP_formats hold;

  switch (coproc_num) {
#if defined(HASFPU)
    case 1:
#if 1
     hold = fpr_state[coproc_reg];
     fpr_state[coproc_reg] = fmt_word;
     value = (unsigned int)ValueFPR(coproc_reg,fmt_uninterpreted);
     fpr_state[coproc_reg] = hold;
#else
#if 1
     value = (unsigned int)ValueFPR(coproc_reg,fpr_state[coproc_reg]);
#else
#ifdef DEBUG
     printf("DBG: COP_SW: reg in format %s (will be accessing as single)\n",DOFMT(fpr_state[coproc_reg])); 
#endif /* DEBUG */
     value = (unsigned int)ValueFPR(coproc_reg,fmt_single);
#endif
#endif
     break;
#endif /* HASFPU */

    default:
#if 0 /* should be controlled by configuration option */
     callback->printf_filtered(callback,"COP_SW(%d,%d) at IPC = 0x%s : TODO (architecture specific)\n",coproc_num,coproc_reg,pr_addr(IPC));
#endif
     break;
  }

  return(value);
}

static uword64
COP_SD(coproc_num,coproc_reg)
     int coproc_num, coproc_reg;
{
  uword64 value = 0;
  switch (coproc_num) {
#if defined(HASFPU)
    case 1:
#if 1
     value = ValueFPR(coproc_reg,fmt_uninterpreted);
#else
#if 1
     value = ValueFPR(coproc_reg,fpr_state[coproc_reg]);
#else
#ifdef DEBUG
     printf("DBG: COP_SD: reg in format %s (will be accessing as double)\n",DOFMT(fpr_state[coproc_reg]));
#endif /* DEBUG */
     value = ValueFPR(coproc_reg,fmt_double);
#endif
#endif
     break;
#endif /* HASFPU */

    default:
#if 0 /* should be controlled by configuration option */
     callback->printf_filtered(callback,"COP_SD(%d,%d) at IPC = 0x%s : TODO (architecture specific)\n",coproc_num,coproc_reg,pr_addr(IPC));
#endif
     break;
  }

  return(value);
}

static void
decode_coproc(instruction)
     unsigned int instruction;
{
  int coprocnum = ((instruction >> 26) & 3);

  switch (coprocnum)
    {
    case 0: /* standard CPU control and cache registers */
      {
        int code = ((instruction >> 21) & 0x1F);
        /* R4000 Users Manual (second edition) lists the following CP0
           instructions:
	   DMFC0   Doubleword Move From CP0        (VR4100 = 01000000001tttttddddd00000000000)
	   DMTC0   Doubleword Move To CP0          (VR4100 = 01000000101tttttddddd00000000000)
	   MFC0    word Move From CP0              (VR4100 = 01000000000tttttddddd00000000000)
	   MTC0    word Move To CP0                (VR4100 = 01000000100tttttddddd00000000000)
	   TLBR    Read Indexed TLB Entry          (VR4100 = 01000010000000000000000000000001)
	   TLBWI   Write Indexed TLB Entry         (VR4100 = 01000010000000000000000000000010)
	   TLBWR   Write Random TLB Entry          (VR4100 = 01000010000000000000000000000110)
	   TLBP    Probe TLB for Matching Entry    (VR4100 = 01000010000000000000000000001000)
	   CACHE   Cache operation                 (VR4100 = 101111bbbbbpppppiiiiiiiiiiiiiiii)
	   ERET    Exception return                (VR4100 = 01000010000000000000000000011000)
	   */
        if (((code == 0x00) || (code == 0x04)) && ((instruction & 0x7FF) == 0))
	  {
	    int rt = ((instruction >> 16) & 0x1F);
	    int rd = ((instruction >> 11) & 0x1F);
	    
	    switch (rd)  /* NOTEs: Standard CP0 registers */
	      {
		/* 0 = Index               R4000   VR4100  VR4300 */
		/* 1 = Random              R4000   VR4100  VR4300 */
		/* 2 = EntryLo0            R4000   VR4100  VR4300 */
		/* 3 = EntryLo1            R4000   VR4100  VR4300 */
		/* 4 = Context             R4000   VR4100  VR4300 */
		/* 5 = PageMask            R4000   VR4100  VR4300 */
		/* 6 = Wired               R4000   VR4100  VR4300 */
		/* 8 = BadVAddr            R4000   VR4100  VR4300 */
		/* 9 = Count               R4000   VR4100  VR4300 */
		/* 10 = EntryHi            R4000   VR4100  VR4300 */
		/* 11 = Compare            R4000   VR4100  VR4300 */
		/* 12 = SR                 R4000   VR4100  VR4300 */
	      case 12:
		if (code == 0x00)
		  GPR[rt] = SR;
		else
		  SR = GPR[rt];
		break;
		/* 13 = Cause              R4000   VR4100  VR4300 */
	      case 13:
		if (code == 0x00)
		  GPR[rt] = CAUSE;
		else
		  CAUSE = GPR[rt];
		break;
		/* 14 = EPC                R4000   VR4100  VR4300 */
		/* 15 = PRId               R4000   VR4100  VR4300 */
		/* 16 = Config             R4000   VR4100  VR4300 */
		/* 17 = LLAddr             R4000   VR4100  VR4300 */
		/* 18 = WatchLo            R4000   VR4100  VR4300 */
		/* 19 = WatchHi            R4000   VR4100  VR4300 */
		/* 20 = XContext           R4000   VR4100  VR4300 */
		/* 26 = PErr or ECC        R4000   VR4100  VR4300 */
		/* 27 = CacheErr           R4000   VR4100 */
		/* 28 = TagLo              R4000   VR4100  VR4300 */
		/* 29 = TagHi              R4000   VR4100  VR4300 */
		/* 30 = ErrorEPC           R4000   VR4100  VR4300 */
		GPR[rt] = 0xDEADC0DE; /* CPR[0,rd] */
		/* CPR[0,rd] = GPR[rt]; */
	      default:
		if (code == 0x00)
		  callback->printf_filtered(callback,"Warning: MFC0 %d,%d not handled yet (architecture specific)\n",rt,rd);
		else
		  callback->printf_filtered(callback,"Warning: MTC0 %d,%d not handled yet (architecture specific)\n",rt,rd);
	      }
	  }
	else if (code == 0x10 && (instruction & 0x3f) == 0x18)
	  {
	    /* ERET */
	    if (SR & status_ERL)
	      {
		/* Oops, not yet available */
		callback->printf_filtered(callback,"Warning: ERET when SR[ERL] set not handled yet");
		PC = EPC;
		SR &= ~status_ERL;
	      }
	    else
	      {
		PC = EPC;
		SR &= ~status_EXL;
	      }
	  }
	else
	  sim_warning("Unrecognised COP0 instruction 0x%08X at IPC = 0x%s : No handler present",instruction,pr_addr(IPC));
        /* TODO: When executing an ERET or RFE instruction we should
           clear LLBIT, to ensure that any out-standing atomic
           read/modify/write sequence fails. */
      }
    break;
    
    case 2: /* undefined co-processor */
      sim_warning("COP2 instruction 0x%08X at IPC = 0x%s : No handler present",instruction,pr_addr(IPC));
      break;
      
    case 1: /* should not occur (FPU co-processor) */
    case 3: /* should not occur (FPU co-processor) */
      SignalException(ReservedInstruction,instruction);
      break;
    }
  
  return;
}

/*-- instruction simulation -------------------------------------------------*/

void
sim_engine_run (sd, next_cpu_nr, siggnal)
     SIM_DESC sd;
     int next_cpu_nr; /* ignore */
     int siggnal; /* ignore */
{
#if !defined(FASTSIM)
  unsigned int pipeline_count = 1;
#endif

#ifdef DEBUG
  if (STATE_MEMORY (sd) == NULL) {
    printf("DBG: simulate() entered with no memory\n");
    exit(1);
  }
#endif /* DEBUG */

#if 0 /* Disabled to check that everything works OK */
  /* The VR4300 seems to sign-extend the PC on its first
     access. However, this may just be because it is currently
     configured in 32bit mode. However... */
  PC = SIGNEXTEND(PC,32);
#endif

  /* main controlling loop */
  while (1) {
    /* Fetch the next instruction from the simulator memory: */
    uword64 vaddr = (uword64)PC;
    uword64 paddr;
    int cca;
    unsigned int instruction;	/* uword64? what's this used for?  FIXME! */
    int dsstate = (state & simDELAYSLOT);

#ifdef DEBUG
    {
      printf("DBG: state = 0x%08X :",state);
#if 0
      if (state & simSTOP) printf(" simSTOP");
      if (state & simSTEP) printf(" simSTEP");
#endif
      if (state & simHALTEX) printf(" simHALTEX");
      if (state & simHALTIN) printf(" simHALTIN");
#if 0
      if (state & simBE) printf(" simBE");
#endif
      printf("\n");
    }
#endif /* DEBUG */

#ifdef DEBUG
    if (dsstate)
     callback->printf_filtered(callback,"DBG: DSPC = 0x%s\n",pr_addr(DSPC));
#endif /* DEBUG */

    if (AddressTranslation(PC,isINSTRUCTION,isLOAD,&paddr,&cca,isTARGET,isREAL)) {
      if ((vaddr & 1) == 0) {
	/* Copy the action of the LW instruction */
	unsigned int reverse = (ReverseEndian ? (LOADDRMASK >> 2) : 0);
	unsigned int bigend = (BigEndianCPU ? (LOADDRMASK >> 2) : 0);
	uword64 value;
	unsigned int byte;
	paddr = ((paddr & ~LOADDRMASK) | ((paddr & LOADDRMASK) ^ (reverse << 2)));
	LoadMemory(&value,NULL,cca,AccessLength_WORD,paddr,vaddr,isINSTRUCTION,isREAL);
	byte = ((vaddr & LOADDRMASK) ^ (bigend << 2));
	instruction = ((value >> (8 * byte)) & 0xFFFFFFFF);
      } else {
	/* Copy the action of the LH instruction */
	unsigned int reverse = (ReverseEndian ? (LOADDRMASK >> 1) : 0);
	unsigned int bigend = (BigEndianCPU ? (LOADDRMASK >> 1) : 0);
	uword64 value;
	unsigned int byte;
	paddr = (((paddr & ~ (uword64) 1) & ~LOADDRMASK)
		 | (((paddr & ~ (uword64) 1) & LOADDRMASK) ^ (reverse << 1)));
	LoadMemory(&value,NULL,cca, AccessLength_HALFWORD,
			   paddr & ~ (uword64) 1,
			   vaddr, isINSTRUCTION, isREAL);
	byte = (((vaddr &~ (uword64) 1) & LOADDRMASK) ^ (bigend << 1));
	instruction = ((value >> (8 * byte)) & 0xFFFF);
      }
    } else {
      fprintf(stderr,"Cannot translate address for PC = 0x%s failed\n",pr_addr(PC));
      exit(1);
    }

#ifdef DEBUG
    callback->printf_filtered(callback,"DBG: fetched 0x%08X from PC = 0x%s\n",instruction,pr_addr(PC));
#endif /* DEBUG */

#if !defined(FASTSIM) || defined(PROFILE)
    instruction_fetches++;
    /* Since we increment above, the value should only ever be zero if
       we have just overflowed: */
    if (instruction_fetches == 0)
      instruction_fetch_overflow++;
#if defined(PROFILE)
    if ((state & simPROFILE) && ((instruction_fetches % profile_frequency) == 0) && profile_hist) {
      unsigned n = ((unsigned int)(PC - profile_minpc) >> (profile_shift + 2));
      if (n < profile_nsamples) {
        /* NOTE: The counts for the profiling bins are only 16bits wide */
        if (profile_hist[n] != USHRT_MAX)
         (profile_hist[n])++;
      }
    }
#endif /* PROFILE */
#endif /* !FASTSIM && PROFILE */

    IPC = PC; /* copy PC for this instruction */
    /* This is required by exception processing, to ensure that we can
       cope with exceptions in the delay slots of branches that may
       already have changed the PC. */
    if ((vaddr & 1) == 0)
      PC += 4; /* increment ready for the next fetch */
    else
      PC += 2;
    /* NOTE: If we perform a delay slot change to the PC, this
       increment is not requuired. However, it would make the
       simulator more complicated to try and avoid this small hit. */

    /* Currently this code provides a simple model. For more
       complicated models we could perform exception status checks at
       this point, and set the simSTOP state as required. This could
       also include processing any hardware interrupts raised by any
       I/O model attached to the simulator context.

       Support for "asynchronous" I/O events within the simulated world
       could be providing by managing a counter, and calling a I/O
       specific handler when a particular threshold is reached. On most
       architectures a decrement and check for zero operation is
       usually quicker than an increment and compare. However, the
       process of managing a known value decrement to zero, is higher
       than the cost of using an explicit value UINT_MAX into the
       future. Which system is used will depend on how complicated the
       I/O model is, and how much it is likely to affect the simulator
       bandwidth.

       If events need to be scheduled further in the future than
       UINT_MAX event ticks, then the I/O model should just provide its
       own counter, triggered from the event system. */

    /* MIPS pipeline ticks. To allow for future support where the
       pipeline hit of individual instructions is known, this control
       loop manages a "pipeline_count" variable. It is initialised to
       1 (one), and will only be changed by the simulator engine when
       executing an instruction. If the engine does not have access to
       pipeline cycle count information then all instructions will be
       treated as using a single cycle. NOTE: A standard system is not
       provided by the default simulator because different MIPS
       architectures have different cycle counts for the same
       instructions.

       [NOTE: pipeline_count has been replaced the event queue] */

#if defined(HASFPU)
    /* Set previous flag, depending on current: */
    if (state & simPCOC0)
     state |= simPCOC1;
    else
     state &= ~simPCOC1;
    /* and update the current value: */
    if (GETFCC(0))
     state |= simPCOC0;
    else
     state &= ~simPCOC0;
#endif /* HASFPU */

/* NOTE: For multi-context simulation environments the "instruction"
   variable should be local to this routine. */

/* Shorthand accesses for engine. Note: If we wanted to use global
   variables (and a single-threaded simulator engine), then we can
   create the actual variables with these names. */

    if (!(state & simSKIPNEXT)) {
      /* Include the simulator engine */
#include "engine.c"
#if ((GPRLEN == 64) && !PROCESSOR_64BIT) || ((GPRLEN == 32) && PROCESSOR_64BIT)
#error "Mismatch between run-time simulator code and simulation engine"
#endif

#if defined(WARN_LOHI)
      /* Decrement the HI/LO validity ticks */
      if (HIACCESS > 0)
       HIACCESS--;
      if (LOACCESS > 0)
       LOACCESS--;
      if (HI1ACCESS > 0)
       HI1ACCESS--;
      if (LO1ACCESS > 0)
       LO1ACCESS--;
#endif /* WARN_LOHI */

      /* For certain MIPS architectures, GPR[0] is hardwired to zero. We
         should check for it being changed. It is better doing it here,
         than within the simulator, since it will help keep the simulator
         small. */
      if (ZERO != 0) {
#if defined(WARN_ZERO)
        sim_warning("The ZERO register has been updated with 0x%s (PC = 0x%s) (reset back to zero)",pr_addr(ZERO),pr_addr(IPC));
#endif /* WARN_ZERO */
        ZERO = 0; /* reset back to zero before next instruction */
      }
    } else /* simSKIPNEXT check */
     state &= ~simSKIPNEXT;

    /* If the delay slot was active before the instruction is
       executed, then update the PC to its new value: */
    if (dsstate) {
#ifdef DEBUG
      printf("DBG: dsstate set before instruction execution - updating PC to 0x%s\n",pr_addr(DSPC));
#endif /* DEBUG */
      PC = DSPC;
      state &= ~(simDELAYSLOT | simJALDELAYSLOT);
    }

    if (MIPSISA < 4) { /* The following is only required on pre MIPS IV processors: */
      /* Deal with pending register updates: */
#ifdef DEBUG
      printf("DBG: EMPTY BEFORE pending_in = %d, pending_out = %d, pending_total = %d\n",pending_in,pending_out,pending_total);
#endif /* DEBUG */
      if (pending_out != pending_in) {
        int loop;
        int index = pending_out;
        int total = pending_total;
        if (pending_total == 0) {
          fprintf(stderr,"FATAL: Mis-match on pending update pointers\n");
          exit(1);
        }
        for (loop = 0; (loop < total); loop++) {
#ifdef DEBUG
          printf("DBG: BEFORE index = %d, loop = %d\n",index,loop);
#endif /* DEBUG */
          if (pending_slot_reg[index] != (LAST_EMBED_REGNUM + 1)) {
#ifdef DEBUG
            printf("pending_slot_count[%d] = %d\n",index,pending_slot_count[index]);
#endif /* DEBUG */
            if (--(pending_slot_count[index]) == 0) {
#ifdef DEBUG
              printf("pending_slot_reg[%d] = %d\n",index,pending_slot_reg[index]);
              printf("pending_slot_value[%d] = 0x%s\n",index,pr_addr(pending_slot_value[index]));
#endif /* DEBUG */
              if (pending_slot_reg[index] == COCIDX) {
                SETFCC(0,((FCR31 & (1 << 23)) ? 1 : 0));
              } else {
                registers[pending_slot_reg[index]] = pending_slot_value[index];
#if defined(HASFPU)
                /* The only time we have PENDING updates to FPU
                   registers, is when performing binary transfers. This
                   means we should update the register type field.  */
                if ((pending_slot_reg[index] >= FGRIDX) && (pending_slot_reg[index] < (FGRIDX + 32)))
                 fpr_state[pending_slot_reg[index] - FGRIDX] = fmt_uninterpreted;
#endif /* HASFPU */
              }
#ifdef DEBUG
              printf("registers[%d] = 0x%s\n",pending_slot_reg[index],pr_addr(registers[pending_slot_reg[index]]));
#endif /* DEBUG */
              pending_slot_reg[index] = (LAST_EMBED_REGNUM + 1);
              pending_out++;
              if (pending_out == PSLOTS)
               pending_out = 0;
              pending_total--;
            }
          }
#ifdef DEBUG
          printf("DBG: AFTER  index = %d, loop = %d\n",index,loop);
#endif /* DEBUG */
          index++;
          if (index == PSLOTS)
           index = 0;
        }
      }
#ifdef DEBUG
      printf("DBG: EMPTY AFTER  pending_in = %d, pending_out = %d, pending_total = %d\n",pending_in,pending_out,pending_total);
#endif /* DEBUG */
    }

#if !defined(FASTSIM)
    if (sim_events_tickn (sd, pipeline_count))
      {
	/* cpu->cia = cia; */
	sim_events_process (sd);
      }
#else
    if (sim_events_tick (sd))
      {
	/* cpu->cia = cia; */
	sim_events_process (sd);
      }
#endif /* FASTSIM */
  }
}

/* This code copied from gdb's utils.c.  Would like to share this code,
   but don't know of a common place where both could get to it. */

/* Temporary storage using circular buffer */
#define NUMCELLS 16
#define CELLSIZE 32
static char*
get_cell()
{
  static char buf[NUMCELLS][CELLSIZE];
  static int cell=0;
  if (++cell>=NUMCELLS) cell=0;
  return buf[cell];
}     

/* Print routines to handle variable size regs, etc */

/* Eliminate warning from compiler on 32-bit systems */
static int thirty_two = 32;	

char* 
pr_addr(addr)
  SIM_ADDR addr;
{
  char *paddr_str=get_cell();
  switch (sizeof(addr))
    {
      case 8:
        sprintf(paddr_str,"%08lx%08lx",
		(unsigned long)(addr>>thirty_two),(unsigned long)(addr&0xffffffff));
	break;
      case 4:
        sprintf(paddr_str,"%08lx",(unsigned long)addr);
	break;
      case 2:
        sprintf(paddr_str,"%04x",(unsigned short)(addr&0xffff));
	break;
      default:
        sprintf(paddr_str,"%x",addr);
    }
  return paddr_str;
}

char* 
pr_uword64(addr)
  uword64 addr;
{
  char *paddr_str=get_cell();
  sprintf(paddr_str,"%08lx%08lx",
          (unsigned long)(addr>>thirty_two),(unsigned long)(addr&0xffffffff));
  return paddr_str;
}


/*---------------------------------------------------------------------------*/
/*> EOF interp.c <*/
