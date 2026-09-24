/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/*
 * ac_titan_regmap.h -- TITAN's context/SH register map.
 *
 * TITAN (Xclipse 530, MGFX1) lays out its registers differently from GFX10_3, with per-block
 * shifts no gfx_level can express. Measured by running the same probe through the vendor driver
 * on a 920 (VOYAGER) and a 530 (TITAN) and comparing where identical values land:
 *
 *   VOYAGER  [02d5] VGT_SHADER_STAGES_EN = 0x06412010
 *   TITAN    [02d5] never written
 *            [02a6] = 0x06412010
 *
 * Entries note how many independent values agreed. INFERRED entries extend a measured shift and
 * are the first to doubt.
 *
 * VGT_GS_OUT_PRIM_TYPE moves from CONTEXT 0x29b to UCONFIG 0x241 (a space change, not a dword
 * remap); it is handled at its emission site.
 */
#ifndef AC_TITAN_REGMAP_H
#define AC_TITAN_REGMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "sid.h"
#include "ac_titan_kmap.h"

#ifdef __cplusplus
extern "C" {
#endif

int ac_xclipse_runcheck(void);

/* Set once at device init when the chip is CHIP_TITAN. A global because
 * __ac_cmdbuf_set_reg_seq() has no device pointer; assumes one GPU per process. */
extern bool ac_titan_regmap_active;
extern uint32_t ac_titan_regmap_hits;   /* remapped writes (proves the arm engaged) */

/* RADV_XCLIPSE_TITAN: how much of the map to apply. Levels are cumulative, so a regression can be
 * bisected to a block:
 *
 *   1  VGT stage enable, DB stencil ref, the viewport region
 *   2  + stop writing the two SH CU masks (one lands on TITAN's shader address)
 *   3  + the merged geometry stage's RSRC1/RSRC2
 *   4  + the SPI context block and the PA_CL guard band
 *   5  + VGT_GS_OUT_PRIM_TYPE in UCONFIG (emission site)
 *   6  + the two VGT registers that size the NGG subgroup
 *   7  + the merged stage's address high half (emission site)
 *   8  + the colour block's write enables
 *   9  + the per-MRT colour block (emission split)
 *  10  + the DB block
 *
 * 0 = off. ac_titan_regmap_active gates all levels. */
extern uint32_t ac_titan_regmap_level;

/* SH CU-mask write suppression, independent of the level. RADV_XCLIPSE_TITAN_SUP=0 disables.
 * Default on at level >= 2. */
extern bool ac_titan_regmap_suppress;

/* Slot of TITAN's 9-dword per-MRT block for the colour target address high half, or -1 for
 * GFX10_3's CB_COLOR0_BASE_EXT. Unmeasured (the vendor's surfaces are below 40 bits);
 * RADV_XCLIPSE_TITAN_CBEXT sweeps it. */
extern int ac_titan_cb_ext_slot;

/* Non-zero: write CB_COLORi_VIEW (SLICE_START, SLICE_MAX, MIP_LEVEL; slot 1 of TITAN's 9-dword
 * per-MRT block, from the kernel map and the vendor's stream). Without it every colour write
 * lands on layer 0 of level 0. RADV_XCLIPSE_TITAN_CBVIEW=0 disables; default on. */
extern int ac_titan_cb_view_slot;

void ac_titan_regmap_set_level(uint32_t level);

/* RADV_XCLIPSE_KMAP: use the register map from the vendor kernel (gc_10_4_0_offset_m1.h,
 * M1 = MGFX1 = TITAN) instead of the hand-recovered table, which had many wrong or missing
 * entries (e.g. VGT_GS_INSTANCE_CNT written over VGT_ESGS_RING_ITEMSIZE, breaking geometry
 * shaders). Graded by space:
 *   0  the hand-recovered table below
 *   1  + context space from the kernel
 *   2  + SH space
 *   3  + uconfig space (default)
 * The generated map is injective; its one space change is VGT_GS_OUT_PRIM_TYPE. */
static inline uint32_t
ac_titan_kmap_level(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_KMAP");
      cached = (e && e[0]) ? atoi(e) : 3;
   }
   return (uint32_t)cached;
}
/* The hand-recovered map left GFX10_3's SH 0x087 (SPI_SHADER_PGM_RSRC3_GS) in place, and TITAN
 * reads the geometry stage's address high half there, so RADV wrote va >> 40 to it. The kernel
 * map moves 0x087 to TITAN's real RSRC3_GS (0x08a) and PGM_HI_ES to 0x087, so with SH space
 * mapped that write sets the NGG stage's CU_EN to 0x80 and its WAVE_LIMIT to 0 on every draw,
 * a register the vendor never writes. Only emitted without the kernel's SH map;
 * RADV_XCLIPSE_TITAN_PGMHI=1 forces it back for A/B. */
static inline bool
ac_titan_legacy_pgm_hi(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_TITAN_PGMHI");
      cached = (e && e[0]) ? (atoi(e) != 0) : (ac_titan_kmap_level() < 2);
   }
   return cached;
}

/* The DB block is mapped (level 10): three GFX10_3 writes that land on live TITAN registers are
 * gated at their emission sites. */
static inline bool
ac_titan_db_active(void)
{
   return ac_titan_regmap_active && ac_titan_regmap_level >= 10;
}

/* RADV_XCLIPSE_TEXIT256=1: set ITERATE_256 on every sampled image descriptor, as the vendor does
 * (word6 = 0x400). RADV sets it only for TC-compatible MSAA HTILE. Default off: matching the
 * vendor here did not fix the 530 mip bug. */
static inline bool
ac_titan_tex_iterate256(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_TEXIT256");
      cached = (e && e[0]) ? atoi(e) : 0;
   }
   return ac_titan_regmap_active && cached != 0;
}

/* Check the remap is injective over both spaces: a collision makes one write silently clobber
 * another. A block moves as a block. */
bool ac_titan_regmap_validate(void);

/* One overridable map slot (debug), to re-measure an entry against its competing reading. */
static inline uint32_t
ac_titan_spi_slot(const char *env, uint32_t dflt)
{
   const char *e = getenv(env);
   return (e && e[0]) ? (uint32_t)strtoul(e, NULL, 0) : dflt;
}

/* RADV_XCLIPSE_MAPOVR="src:dst,src:dst,...": relocate map entries (context dwords, hex) without a
 * rebuild. Applied before the table, so it can also move identity entries. Parsed once at device
 * init from ac_titan_regmap_set_level(). */
#define AC_TITAN_MAPOVR_MAX 16
extern int ac_titan_mapovr_n;                  /* 0 = nothing overridden, the usual case */
extern uint32_t ac_titan_mapovr_src[AC_TITAN_MAPOVR_MAX];
extern uint32_t ac_titan_mapovr_dst[AC_TITAN_MAPOVR_MAX];
void ac_titan_mapovr_parse(void);

static inline uint32_t
ac_titan_ctx_dw(uint32_t dw)
{
   /* Environment override first. */
   for (int __i = 0; __i < ac_titan_mapovr_n; __i++)
      if (ac_titan_mapovr_src[__i] == dw)
         return ac_titan_mapovr_dst[__i];

   if (ac_titan_kmap_level() >= 1)
      return ac_titan_kmap_ctx(dw);

   /* ---- Level 1 ---- */
   switch (dw) {
   case 0x2d5: return 0x2a6;   /* VGT_SHADER_STAGES_EN  -- direct, the whole reason for this */
   case 0x2a6: return 0x2a8;   /* VGT_DRAW_PAYLOAD_CNTL -- direct; must move or it clobbers ^ */
   case 0x10c: return 0x0fa;   /* DB_STENCILREFMASK     -- n=6                                */
   case 0x10d: return 0x0fb;   /* DB_STENCILREFMASK_BF  -- n=6                                */
   /* Withdrawn: PA_SU_SC_MODE_CNTL 0x205 -> 0x204 collides with PA_CL_CLIP_CNTL at 0x204. */
   default: break;
   }

   /* Viewport region: GFX12's interleaved layout (8 registers per viewport, ZMIN/ZMAX at slots 6/7)
    * shifted by two, because TITAN's PA_CL_GB block sits at 0x10d..0x110. Measured 8/8:
    *
    *   920  XSCALE 0x10f XOFFSET 0x110 YSCALE 0x111 YOFFSET 0x112 ZSCALE 0x113 ZOFFSET 0x114
    *        ZMIN 0x0b4  ZMAX 0x0b5                      (two arrays, six-stride + two-stride)
    *   530  XSCALE 0x111 XOFFSET 0x112 YSCALE 0x113 YOFFSET 0x114 ZSCALE 0x115 ZOFFSET 0x116
    *        ZMIN 0x117  ZMAX 0x118                      (one array, eight-stride)
    *
    * A flat +2 is only right because radv_emit_viewport_state emits the GFX12 shape on TITAN;
    * a remap cannot change a run's stride. */
   if (dw >= 0x10f && dw < 0x10f + 8 * 16)
      return dw + 2;

   if (ac_titan_regmap_level < 4)
      return dw;

   /* ---- Level 4: the SPI block, GFX12's layout ----
    * The 530 writes GFX12-only registers (SPI_GFX_SCRATCH_BASE_LO/HI 0x1bb/0x1bc,
    * SPI_BARYC_SSAA_CNTL 0x1b9) at GFX12 offsets. Anchored by value (920 -> 530):
    *
    *   SPI_BARYC_CNTL       0x1b8 -> 0x196   value 0x01000000, unique
    *   SPI_SHADER_COL_FORMAT 0x1c5 -> 0x195  value 0x4444, unique
    *   SPI_SHADER_POS_FORMAT 0x1c3 -> 0x193  value 0x0044, unique
    *   SPI_PS_INPUT_ENA/ADDR 0x1b3/4 -> 0x197/8
    *   SPI_SHADER_IDX_FORMAT 0x1c2 -> 0x192, SPI_SHADER_Z_FORMAT 0x1c4 -> 0x194
    *   SPI_INTERP_CONTROL_0  0x1b5 -> 0x191
    *
    * Matched by value sequence across pipelines (GFX12 has no slot for these):
    *
    *   SPI_VS_OUT_CONFIG  0x1b1 -> 0x1c3   920 emits 0, 0x80, 0 ; the 530 emits 0, 0, 0x80, 0
    *   SPI_PS_IN_CONTROL  0x1b6 -> 0x1c4   920 emits 0, 1       ; the 530 emits 0, 0, 1
    *
    * SPI_PS_IN_CONTROL is the weakest entry (value 1; GFX12's 0x190 is the competing reading).
    */
   if (dw >= 0x191 && dw <= 0x1b0)
      return dw + 8;           /* SPI_PS_INPUT_CNTL_0..31 -> 0x199..0x1b8 */
   switch (dw) {
   case 0x1b5: return 0x191;   /* SPI_INTERP_CONTROL_0                     */
   case 0x1c2: return 0x192;   /* SPI_SHADER_IDX_FORMAT                    */
   case 0x1c3: return 0x193;   /* SPI_SHADER_POS_FORMAT                    */
   case 0x1c4: return 0x194;   /* SPI_SHADER_Z_FORMAT                      */
   case 0x1c5: return 0x195;   /* SPI_SHADER_COL_FORMAT                    */
   case 0x1b8: return 0x196;   /* SPI_BARYC_CNTL                           */
   case 0x1b3: return 0x197;   /* SPI_PS_INPUT_ENA                         */
   case 0x1b4: return 0x198;   /* SPI_PS_INPUT_ADDR                        */
   /* The two sequence-matched entries. SPI_PS_IN_CONTROL.NUM_INTERP could explain interpolation
    * jitter. RADV_XCLIPSE_SPIVOC / RADV_XCLIPSE_SPIPIC override the destinations. */
   case 0x1b1: return ac_titan_spi_slot("RADV_XCLIPSE_SPIVOC", 0x1c3);
   case 0x1b6: return ac_titan_spi_slot("RADV_XCLIPSE_SPIPIC", 0x1c4);

   /* PA_CL guard band: GFX12's block (0x10b..0x10e) at +2, like the viewport array after it. */
   case 0x2fa: return 0x10d;   /* PA_CL_GB_VERT_CLIP_ADJ */
   case 0x2fb: return 0x10e;   /* PA_CL_GB_VERT_DISC_ADJ */
   case 0x2fc: return 0x10f;   /* PA_CL_GB_HORZ_CLIP_ADJ */
   case 0x2fd: return 0x110;   /* PA_CL_GB_HORZ_DISC_ADJ */
   default: break;
   }

   if (ac_titan_regmap_level < 6)
      return dw;

   /* ---- Level 6: the VGT registers that configure the NGG subgroup ----
    * Same chip, RADV vs PAL on the 530:
    *
    *   VGT_GS_ONCHIP_CNTL   0x291 -> 0x2a9   value 0x20040080, one PAL offset
    *   VGT_GS_INSTANCE_CNT  0x2e4 -> 0x2d0   value 4 (weak: supported by 0x2e4 being RADV-only
    *                                         and 0x2d0 PAL-only)
    *
    * With zero in VGT_GS_ONCHIP_CNTL (verts/prims per subgroup) no wave ever launches. */
   switch (dw) {
   case 0x291: return 0x2a9;   /* VGT_GS_ONCHIP_CNTL  */
   case 0x2e4: return 0x2d0;   /* VGT_GS_INSTANCE_CNT */
   default: break;
   }

   if (ac_titan_regmap_level < 8)
      return dw;

   /* ---- Level 8: the colour block's write enables ----
    * Same chip, RADV vs PAL on the 530:
    *
    *   CB_COLOR_CONTROL  0x202 -> 0x378   value 0x00cc0010, one PAL offset
    *   CB_TARGET_MASK    0x08e -> 0x37a   value 0xf
    *   CB_SHADER_MASK    0x08f -> 0x37b   value 0xf
    *
    * The two masks share a value, so their order is a guess (GFX10_3's). Swap them first if colour
    * comes out wrong rather than absent. CB_COLOR0_INFO and ATTRIB2/3 need an emission split
    * (level 9). */
   switch (dw) {
   case 0x202: return 0x378;   /* CB_COLOR_CONTROL */
   case 0x08e: return 0x37a;   /* CB_TARGET_MASK   */
   case 0x08f: return 0x37b;   /* CB_SHADER_MASK   */
   default: break;
   }

   if (ac_titan_regmap_level < 9)
      return dw;

   /* ---- Level 9: the per-MRT colour block ----
    * TITAN gives each MRT 9 dwords, folds ATTRIB2/ATTRIB3 in as slots 7 and 8, and moves
    * CB_COLORi_INFO to a compact array:
    *
    *            MRT      0      1      2      3
    *     920    BASE   0x318  0x327  0x336  0x345      stride 15
    *            INFO   0x31c  0x32b  0x33a  0x349
    *            ATTRIB2 0x3b0 0x3b1  0x3b2  0x3b3      separate array
    *     530    BASE   0x318  0x321  0x32a  0x333      stride 9
    *            ATTRIB2 0x31f 0x328  0x331  0x33a      slot 7 of the block
    *            ATTRIB3 0x320 0x329  0x332  0x33b      slot 8
    *            INFO   0x360  0x361  0x362  0x363      compact, stride 1
    *
    * These are the only per-MRT registers the 530's PAL writes for an uncompressed target, so the
    * TITAN emission path writes exactly these four. */
   if (dw >= 0x318 && dw < 0x318 + 0x0f * 8) {
      const uint32_t mrt = (dw - 0x318) / 0x0f, slot = (dw - 0x318) % 0x0f;
      if (slot == 0)
         return 0x318 + 9 * mrt;      /* CB_COLORi_BASE */
      if (slot == 4)
         return 0x360 + mrt;          /* CB_COLORi_INFO -> the compact array */
   }
   if (dw >= 0x3b0 && dw < 0x3b8)
      return 0x31f + 9 * (dw - 0x3b0); /* CB_COLORi_ATTRIB2 */
   if (dw >= 0x3b8 && dw < 0x3c0)
      return 0x320 + 9 * (dw - 0x3b8); /* CB_COLORi_ATTRIB3 */

   if (ac_titan_regmap_level < 10)
      return dw;

   /* ---- Level 10: the DB block ----
    * Without it depth-tested scenes render nothing. TITAN's DB layout is its own. Anchored by value
    * (the 530's PAL emits the surface half as one 13-dword run at 0x00e):
    *
    *     920                            value         530
    *     0x000 DB_RENDER_CONTROL        0x00000003 -> 0x019
    *     0x004 DB_RENDER_OVERRIDE2      0x10000000 -> 0x017
    *     0x005 DB_HTILE_DATA_BASE       htile VA   -> 0x00e   (z_base + 0x200 on both chips)
    *     0x007 DB_DEPTH_SIZE_XY         0x003f003f -> 0x00f   (the probe's 64x64)
    *     0x01f DB_RMI_L2_CACHE_CONTROL  0x000a0045 -> 0x01a
    *     0x201 DB_EQAA                  0x00130000 -> 0x01b
    *     0x2dc DB_ALPHA_TO_MASK         0x00018701 -> 0x01c
    *     0x2af DB_HTILE_SURFACE         0x00040000 -> 0x018
    *     0x10b DB_STENCIL_CONTROL       0x00333333 -> 0x201   (n=2)
    *
    * Confirmed unmoved: DB_COUNT_CONTROL 0x001, DB_RENDER_OVERRIDE 0x003, DB_DEPTH_BOUNDS_MIN/MAX
    * 0x008/0x009, DB_Z_INFO 0x010, DB_STENCIL_INFO 0x011, surface bases 0x012..0x015,
    * DB_DEPTH_CONTROL 0x200, DB_SHADER_CONTROL 0x203.
    * No evidence (always zero): DB_DEPTH_VIEW 0x002, DB_STENCIL_CLEAR 0x00a, DB_DEPTH_CLEAR 0x00b.
    *
    * Suppressed at their emission sites (see ac_titan_db_active), as they hit live registers:
    *   R_028064_DB_VRS_OVERRIDE_CNTL -> 0x019 (TITAN's DB_RENDER_CONTROL)
    *   R_028038_DB_DFSM_CONTROL      -> 0x00e (TITAN's DB_HTILE_DATA_BASE)
    *   the 5-register R_028068_DB_Z_READ_BASE_HI run -> 0x01a..0x01e */
   switch (dw) {
   case 0x000: return 0x019;   /* DB_RENDER_CONTROL      */
   case 0x004: return 0x017;   /* DB_RENDER_OVERRIDE2    */
   case 0x005: return 0x00e;   /* DB_HTILE_DATA_BASE     */
   case 0x007: return 0x00f;   /* DB_DEPTH_SIZE_XY       */
   case 0x01f: return 0x01a;   /* DB_RMI_L2_CACHE_CONTROL */
   case 0x10b: return 0x201;   /* DB_STENCIL_CONTROL     */
   case 0x201: return 0x01b;   /* DB_EQAA                */
   case 0x2af: return 0x018;   /* DB_HTILE_SURFACE       */
   case 0x2dc: return 0x01c;   /* DB_ALPHA_TO_MASK       */
   default: break;
   }
   return dw;
}

static inline uint32_t
ac_titan_sh_dw(uint32_t dw)
{
   if (ac_titan_kmap_level() >= 2)
      return ac_titan_kmap_sh(dw);

   /* ---- Level 1 ---- */
   if (dw == 0x0c8)
      return 0x086;            /* SPI_SHADER_PGM_LO_ES -- n=2 on value, and see below */

   if (ac_titan_regmap_level < 3)
      return dw;

   /* ---- Level 3: the merged geometry stage's RSRC pair ----
    * TITAN packs the NGG stage's registers into one run at 0x084, in a different order:
    *
    *     TITAN    0x084 RSRC1   0x085 RSRC2   0x086 PGM_LO   0x087 PGM_HI
    *     GFX10_3  0x08a RSRC1   0x08b RSRC2   0x0c8 PGM_LO   0x0c9 PGM_HI
    *
    * Measured by value and by run alignment (n=10). With RSRC1/RSRC2 at GFX10_3's offsets the
    * chip hangs on the first draw (3/3); moved, the draw retires.
    * PGM_HI (0x087) is written at level 7 (RADV_TITAN_EMIT_PGM_HI in radv_cmd_buffer.c).
    */
   switch (dw) {
   case 0x08a: return 0x084;   /* SPI_SHADER_PGM_RSRC1_GS */
   case 0x08b: return 0x085;   /* SPI_SHADER_PGM_RSRC2_GS */
   default: return dw;
   }
}

/* SH writes TITAN must not receive (levels 2..6): RADV's CU_EN at 0x087, which is TITAN's
 * PGM_HI. Suppression did not fix the hang; from level 7 the address high half is written there.
 * Takes the byte address. */
static inline bool
ac_titan_sh_suppressed(uint32_t reg)
{
   const uint32_t dw = (reg - SI_SH_REG_OFFSET) >> 2;

   if (!ac_titan_regmap_active || ac_titan_regmap_level < 2 || !ac_titan_regmap_suppress)
      return false;
   return dw == 0x087 || dw == 0x081;
}

/* RADV_XCLIPSE_PREMAP: remap the GFX preamble's writes too (ac_pm4_set_reg() converts
 * byte->dword itself and bypassed the map). Default on, 0 disables; empty counts as unset. */
static inline bool
ac_titan_preamble_remap(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("RADV_XCLIPSE_PREMAP");
      cached = (e && e[0]) ? (atoi(e) != 0) : 1;
   }
   return cached != 0;
}

/* Same mapping without touching ac_titan_regmap_hits, so the run checker does not perturb it. */
static inline uint32_t
ac_titan_remap_dw_quiet(uint32_t base, uint32_t dw)
{
   if (!ac_titan_regmap_active)
      return dw;
   if (base == SI_CONTEXT_REG_OFFSET)
      return ac_titan_ctx_dw(dw);
   if (base == SI_SH_REG_OFFSET)
      return ac_titan_sh_dw(dw);
   if (base == CIK_UCONFIG_REG_OFFSET && ac_titan_kmap_level() >= 3)
      return ac_titan_kmap_ucfg(dw);
   return dw;
}
/* True when a run of `num` registers from `dw` does not stay contiguous under TITAN's map, so it
 * has to go out one register at a time (radeonsi's tracked-register runs). */
static inline bool
ac_titan_run_split(uint32_t base, uint32_t dw, uint32_t num)
{
   if (!ac_titan_regmap_active)
      return false;
   const uint32_t d0 = ac_titan_remap_dw_quiet(base, dw);
   for (uint32_t k = 1; k < num; k++)
      if (ac_titan_remap_dw_quiet(base, dw + k) != d0 + k)
         return true;
   return false;
}

/* Called where a register write becomes a dword offset. `base` identifies the space; only
 * CONTEXT and SH are touched. */
static inline uint32_t
ac_titan_remap_dw(uint32_t base, uint32_t dw)
{
   uint32_t out = dw;

   if (!ac_titan_regmap_active)
      return dw;

   if (base == SI_CONTEXT_REG_OFFSET)
      out = ac_titan_ctx_dw(dw);
   else if (base == SI_SH_REG_OFFSET)
      out = ac_titan_sh_dw(dw);
   else if (base == CIK_UCONFIG_REG_OFFSET && ac_titan_kmap_level() >= 3)
      out = ac_titan_kmap_ucfg(dw);

   if (out != dw)
      ac_titan_regmap_hits++;
   return out;
}

/* A run can only be remapped if it stays a run: __ac_cmdbuf_set_reg_seq() maps the start and
 * emits consecutive dwords. Report each (reg, num) whose destinations scatter, once, so the
 * emission site can be split.
 */
void ac_titan_report_split_run(uint32_t base, uint32_t dw, uint32_t num, uint32_t d0, uint32_t dk,
                               uint32_t k);

static inline void
ac_titan_check_run(uint32_t base, uint32_t dw, uint32_t num)
{
   /* RADV_XCLIPSE_RUNCHECK=1 enables (a map lookup per register of every run). */
   static int enabled = -1;
   if (enabled < 0)
      enabled = ac_xclipse_runcheck() != 0;
   if (!enabled || !ac_titan_regmap_active || num < 2)
      return;

   /* Deliberate reshapes: the viewport array at PA_CL_VPORT_XSCALE is emitted GFX12-style on
    * TITAN (see radv_emit_viewport). Keep this list tiny. */
   if (base == SI_CONTEXT_REG_OFFSET && dw == 0x10f)
      return;

   const uint32_t d0 = ac_titan_remap_dw_quiet(base, dw);
   for (uint32_t k = 1; k < num; k++) {
      const uint32_t dk = ac_titan_remap_dw_quiet(base, dw + k);
      if (dk != d0 + k) {
         ac_titan_report_split_run(base, dw, num, d0, dk, k);
         return;
      }
   }
}
#ifdef __cplusplus
}
#endif

#endif /* AC_TITAN_REGMAP_H */
