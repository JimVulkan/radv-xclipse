/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* Generated -- do not edit.
 *
 * TITAN's register map, from the Samsung kernel's asic_reg/gc/gc_10_4_0_offset_m1.h
 * (M1 == MGFX1 == Xclipse 530).
 */
#ifndef AC_TITAN_KMAP_H
#define AC_TITAN_KMAP_H

#include <stdint.h>

/* 337 CTX registers move on TITAN. */
static inline uint32_t
ac_titan_kmap_ctx(uint32_t dw)
{
   switch (dw) {
   case 0x000: return 0x019;   /* DB_RENDER_CONTROL */
   case 0x002: return 0x016;   /* DB_DEPTH_VIEW */
   case 0x004: return 0x017;   /* DB_RENDER_OVERRIDE2 */
   case 0x005: return 0x00e;   /* DB_HTILE_DATA_BASE */
   case 0x007: return 0x00f;   /* DB_DEPTH_SIZE_XY */
   case 0x00f: return 0x007;   /* DB_RESERVED_REG_2 */
   case 0x016: return 0x002;   /* DB_RESERVED_REG_1 */
   case 0x017: return 0x004;   /* DB_RESERVED_REG_3 */
   case 0x018: return 0x2af;   /* DB_SPI_VRS_CENTER_LOCATION */
   case 0x019: return 0x000;   /* DB_VRS_OVERRIDE_CNTL */
   case 0x01a: return 0x01f;   /* DB_Z_READ_BASE_HI */
   case 0x01b: return 0x023;   /* DB_STENCIL_READ_BASE_HI */
   case 0x01c: return 0x022;   /* DB_Z_WRITE_BASE_HI */
   case 0x01f: return 0x01a;   /* DB_RMI_L2_CACHE_CONTROL */
   case 0x08e: return 0x37a;   /* CB_TARGET_MASK */
   case 0x08f: return 0x37b;   /* CB_SHADER_MASK */
   case 0x0b4: return 0x117;   /* PA_SC_VPORT_ZMIN_0 */
   case 0x0b5: return 0x118;   /* PA_SC_VPORT_ZMAX_0 */
   case 0x0b6: return 0x11f;   /* PA_SC_VPORT_ZMIN_1 */
   case 0x0b7: return 0x120;   /* PA_SC_VPORT_ZMAX_1 */
   case 0x0b8: return 0x127;   /* PA_SC_VPORT_ZMIN_2 */
   case 0x0b9: return 0x128;   /* PA_SC_VPORT_ZMAX_2 */
   case 0x0ba: return 0x12f;   /* PA_SC_VPORT_ZMIN_3 */
   case 0x0bb: return 0x130;   /* PA_SC_VPORT_ZMAX_3 */
   case 0x0bc: return 0x137;   /* PA_SC_VPORT_ZMIN_4 */
   case 0x0bd: return 0x138;   /* PA_SC_VPORT_ZMAX_4 */
   case 0x0be: return 0x13f;   /* PA_SC_VPORT_ZMIN_5 */
   case 0x0bf: return 0x140;   /* PA_SC_VPORT_ZMAX_5 */
   case 0x0c0: return 0x147;   /* PA_SC_VPORT_ZMIN_6 */
   case 0x0c1: return 0x148;   /* PA_SC_VPORT_ZMAX_6 */
   case 0x0c2: return 0x14f;   /* PA_SC_VPORT_ZMIN_7 */
   case 0x0c3: return 0x150;   /* PA_SC_VPORT_ZMAX_7 */
   case 0x0c4: return 0x157;   /* PA_SC_VPORT_ZMIN_8 */
   case 0x0c5: return 0x158;   /* PA_SC_VPORT_ZMAX_8 */
   case 0x0c6: return 0x15f;   /* PA_SC_VPORT_ZMIN_9 */
   case 0x0c7: return 0x160;   /* PA_SC_VPORT_ZMAX_9 */
   case 0x0c8: return 0x167;   /* PA_SC_VPORT_ZMIN_10 */
   case 0x0c9: return 0x168;   /* PA_SC_VPORT_ZMAX_10 */
   case 0x0ca: return 0x16f;   /* PA_SC_VPORT_ZMIN_11 */
   case 0x0cb: return 0x170;   /* PA_SC_VPORT_ZMAX_11 */
   case 0x0cc: return 0x177;   /* PA_SC_VPORT_ZMIN_12 */
   case 0x0cd: return 0x178;   /* PA_SC_VPORT_ZMAX_12 */
   case 0x0ce: return 0x17f;   /* PA_SC_VPORT_ZMIN_13 */
   case 0x0cf: return 0x180;   /* PA_SC_VPORT_ZMAX_13 */
   case 0x0d0: return 0x187;   /* PA_SC_VPORT_ZMIN_14 */
   case 0x0d1: return 0x188;   /* PA_SC_VPORT_ZMAX_14 */
   case 0x0d2: return 0x18f;   /* PA_SC_VPORT_ZMIN_15 */
   case 0x0d3: return 0x190;   /* PA_SC_VPORT_ZMAX_15 */
   case 0x10a: return 0x379;   /* CB_COVERAGE_OUT_CONTROL */
   case 0x10b: return 0x201;   /* DB_STENCIL_CONTROL */
   case 0x10c: return 0x0fa;   /* DB_STENCILREFMASK */
   case 0x10d: return 0x0fb;   /* DB_STENCILREFMASK_BF */
   case 0x10f: return 0x111;   /* PA_CL_VPORT_XSCALE */
   case 0x110: return 0x112;   /* PA_CL_VPORT_XOFFSET */
   case 0x111: return 0x113;   /* PA_CL_VPORT_YSCALE */
   case 0x112: return 0x114;   /* PA_CL_VPORT_YOFFSET */
   case 0x113: return 0x115;   /* PA_CL_VPORT_ZSCALE */
   case 0x114: return 0x116;   /* PA_CL_VPORT_ZOFFSET */
   case 0x115: return 0x119;   /* PA_CL_VPORT_XSCALE_1 */
   case 0x116: return 0x11a;   /* PA_CL_VPORT_XOFFSET_1 */
   case 0x117: return 0x11b;   /* PA_CL_VPORT_YSCALE_1 */
   case 0x118: return 0x11c;   /* PA_CL_VPORT_YOFFSET_1 */
   case 0x119: return 0x11d;   /* PA_CL_VPORT_ZSCALE_1 */
   case 0x11a: return 0x11e;   /* PA_CL_VPORT_ZOFFSET_1 */
   case 0x11b: return 0x121;   /* PA_CL_VPORT_XSCALE_2 */
   case 0x11c: return 0x122;   /* PA_CL_VPORT_XOFFSET_2 */
   case 0x11d: return 0x123;   /* PA_CL_VPORT_YSCALE_2 */
   case 0x11e: return 0x124;   /* PA_CL_VPORT_YOFFSET_2 */
   case 0x11f: return 0x125;   /* PA_CL_VPORT_ZSCALE_2 */
   case 0x120: return 0x126;   /* PA_CL_VPORT_ZOFFSET_2 */
   case 0x121: return 0x129;   /* PA_CL_VPORT_XSCALE_3 */
   case 0x122: return 0x12a;   /* PA_CL_VPORT_XOFFSET_3 */
   case 0x123: return 0x12b;   /* PA_CL_VPORT_YSCALE_3 */
   case 0x124: return 0x12c;   /* PA_CL_VPORT_YOFFSET_3 */
   case 0x125: return 0x12d;   /* PA_CL_VPORT_ZSCALE_3 */
   case 0x126: return 0x12e;   /* PA_CL_VPORT_ZOFFSET_3 */
   case 0x127: return 0x131;   /* PA_CL_VPORT_XSCALE_4 */
   case 0x128: return 0x132;   /* PA_CL_VPORT_XOFFSET_4 */
   case 0x129: return 0x133;   /* PA_CL_VPORT_YSCALE_4 */
   case 0x12a: return 0x134;   /* PA_CL_VPORT_YOFFSET_4 */
   case 0x12b: return 0x135;   /* PA_CL_VPORT_ZSCALE_4 */
   case 0x12c: return 0x136;   /* PA_CL_VPORT_ZOFFSET_4 */
   case 0x12d: return 0x139;   /* PA_CL_VPORT_XSCALE_5 */
   case 0x12e: return 0x13a;   /* PA_CL_VPORT_XOFFSET_5 */
   case 0x12f: return 0x13b;   /* PA_CL_VPORT_YSCALE_5 */
   case 0x130: return 0x13c;   /* PA_CL_VPORT_YOFFSET_5 */
   case 0x131: return 0x13d;   /* PA_CL_VPORT_ZSCALE_5 */
   case 0x132: return 0x13e;   /* PA_CL_VPORT_ZOFFSET_5 */
   case 0x133: return 0x141;   /* PA_CL_VPORT_XSCALE_6 */
   case 0x134: return 0x142;   /* PA_CL_VPORT_XOFFSET_6 */
   case 0x135: return 0x143;   /* PA_CL_VPORT_YSCALE_6 */
   case 0x136: return 0x144;   /* PA_CL_VPORT_YOFFSET_6 */
   case 0x137: return 0x145;   /* PA_CL_VPORT_ZSCALE_6 */
   case 0x138: return 0x146;   /* PA_CL_VPORT_ZOFFSET_6 */
   case 0x139: return 0x149;   /* PA_CL_VPORT_XSCALE_7 */
   case 0x13a: return 0x14a;   /* PA_CL_VPORT_XOFFSET_7 */
   case 0x13b: return 0x14b;   /* PA_CL_VPORT_YSCALE_7 */
   case 0x13c: return 0x14c;   /* PA_CL_VPORT_YOFFSET_7 */
   case 0x13d: return 0x14d;   /* PA_CL_VPORT_ZSCALE_7 */
   case 0x13e: return 0x14e;   /* PA_CL_VPORT_ZOFFSET_7 */
   case 0x13f: return 0x151;   /* PA_CL_VPORT_XSCALE_8 */
   case 0x140: return 0x152;   /* PA_CL_VPORT_XOFFSET_8 */
   case 0x141: return 0x153;   /* PA_CL_VPORT_YSCALE_8 */
   case 0x142: return 0x154;   /* PA_CL_VPORT_YOFFSET_8 */
   case 0x143: return 0x155;   /* PA_CL_VPORT_ZSCALE_8 */
   case 0x144: return 0x156;   /* PA_CL_VPORT_ZOFFSET_8 */
   case 0x145: return 0x159;   /* PA_CL_VPORT_XSCALE_9 */
   case 0x146: return 0x15a;   /* PA_CL_VPORT_XOFFSET_9 */
   case 0x147: return 0x15b;   /* PA_CL_VPORT_YSCALE_9 */
   case 0x148: return 0x15c;   /* PA_CL_VPORT_YOFFSET_9 */
   case 0x149: return 0x15d;   /* PA_CL_VPORT_ZSCALE_9 */
   case 0x14a: return 0x15e;   /* PA_CL_VPORT_ZOFFSET_9 */
   case 0x14b: return 0x161;   /* PA_CL_VPORT_XSCALE_10 */
   case 0x14c: return 0x162;   /* PA_CL_VPORT_XOFFSET_10 */
   case 0x14d: return 0x163;   /* PA_CL_VPORT_YSCALE_10 */
   case 0x14e: return 0x164;   /* PA_CL_VPORT_YOFFSET_10 */
   case 0x14f: return 0x165;   /* PA_CL_VPORT_ZSCALE_10 */
   case 0x150: return 0x166;   /* PA_CL_VPORT_ZOFFSET_10 */
   case 0x151: return 0x169;   /* PA_CL_VPORT_XSCALE_11 */
   case 0x152: return 0x16a;   /* PA_CL_VPORT_XOFFSET_11 */
   case 0x153: return 0x16b;   /* PA_CL_VPORT_YSCALE_11 */
   case 0x154: return 0x16c;   /* PA_CL_VPORT_YOFFSET_11 */
   case 0x155: return 0x16d;   /* PA_CL_VPORT_ZSCALE_11 */
   case 0x156: return 0x16e;   /* PA_CL_VPORT_ZOFFSET_11 */
   case 0x157: return 0x171;   /* PA_CL_VPORT_XSCALE_12 */
   case 0x158: return 0x172;   /* PA_CL_VPORT_XOFFSET_12 */
   case 0x159: return 0x173;   /* PA_CL_VPORT_YSCALE_12 */
   case 0x15a: return 0x174;   /* PA_CL_VPORT_YOFFSET_12 */
   case 0x15b: return 0x175;   /* PA_CL_VPORT_ZSCALE_12 */
   case 0x15c: return 0x176;   /* PA_CL_VPORT_ZOFFSET_12 */
   case 0x15d: return 0x179;   /* PA_CL_VPORT_XSCALE_13 */
   case 0x15e: return 0x17a;   /* PA_CL_VPORT_XOFFSET_13 */
   case 0x15f: return 0x17b;   /* PA_CL_VPORT_YSCALE_13 */
   case 0x160: return 0x17c;   /* PA_CL_VPORT_YOFFSET_13 */
   case 0x161: return 0x17d;   /* PA_CL_VPORT_ZSCALE_13 */
   case 0x162: return 0x17e;   /* PA_CL_VPORT_ZOFFSET_13 */
   case 0x163: return 0x181;   /* PA_CL_VPORT_XSCALE_14 */
   case 0x164: return 0x182;   /* PA_CL_VPORT_XOFFSET_14 */
   case 0x165: return 0x183;   /* PA_CL_VPORT_YSCALE_14 */
   case 0x166: return 0x184;   /* PA_CL_VPORT_YOFFSET_14 */
   case 0x167: return 0x185;   /* PA_CL_VPORT_ZSCALE_14 */
   case 0x168: return 0x186;   /* PA_CL_VPORT_ZOFFSET_14 */
   case 0x169: return 0x189;   /* PA_CL_VPORT_XSCALE_15 */
   case 0x16a: return 0x18a;   /* PA_CL_VPORT_XOFFSET_15 */
   case 0x16b: return 0x18b;   /* PA_CL_VPORT_YSCALE_15 */
   case 0x16c: return 0x18c;   /* PA_CL_VPORT_YOFFSET_15 */
   case 0x16d: return 0x18d;   /* PA_CL_VPORT_ZSCALE_15 */
   case 0x16e: return 0x18e;   /* PA_CL_VPORT_ZOFFSET_15 */
   case 0x16f: return 0x0b4;   /* PA_CL_UCP_0_X */
   case 0x170: return 0x0b5;   /* PA_CL_UCP_0_Y */
   case 0x171: return 0x0b6;   /* PA_CL_UCP_0_Z */
   case 0x172: return 0x0b7;   /* PA_CL_UCP_0_W */
   case 0x173: return 0x0b8;   /* PA_CL_UCP_1_X */
   case 0x174: return 0x0b9;   /* PA_CL_UCP_1_Y */
   case 0x175: return 0x0ba;   /* PA_CL_UCP_1_Z */
   case 0x176: return 0x0bb;   /* PA_CL_UCP_1_W */
   case 0x177: return 0x0bc;   /* PA_CL_UCP_2_X */
   case 0x178: return 0x0bd;   /* PA_CL_UCP_2_Y */
   case 0x179: return 0x0be;   /* PA_CL_UCP_2_Z */
   case 0x17a: return 0x0bf;   /* PA_CL_UCP_2_W */
   case 0x17b: return 0x0c0;   /* PA_CL_UCP_3_X */
   case 0x17c: return 0x0c1;   /* PA_CL_UCP_3_Y */
   case 0x17d: return 0x0c2;   /* PA_CL_UCP_3_Z */
   case 0x17e: return 0x0c3;   /* PA_CL_UCP_3_W */
   case 0x17f: return 0x0c4;   /* PA_CL_UCP_4_X */
   case 0x180: return 0x0c5;   /* PA_CL_UCP_4_Y */
   case 0x181: return 0x0c6;   /* PA_CL_UCP_4_Z */
   case 0x182: return 0x0c7;   /* PA_CL_UCP_4_W */
   case 0x183: return 0x0c8;   /* PA_CL_UCP_5_X */
   case 0x184: return 0x0c9;   /* PA_CL_UCP_5_Y */
   case 0x185: return 0x0ca;   /* PA_CL_UCP_5_Z */
   case 0x186: return 0x0cb;   /* PA_CL_UCP_5_W */
   case 0x187: return 0x0cc;   /* PA_CL_PROG_NEAR_CLIP_Z */
   case 0x188: return 0x0cd;   /* PA_RATE_CNTL */
   case 0x191: return 0x199;   /* SPI_PS_INPUT_CNTL_0 */
   case 0x192: return 0x19a;   /* SPI_PS_INPUT_CNTL_1 */
   case 0x193: return 0x19b;   /* SPI_PS_INPUT_CNTL_2 */
   case 0x194: return 0x19c;   /* SPI_PS_INPUT_CNTL_3 */
   case 0x195: return 0x19d;   /* SPI_PS_INPUT_CNTL_4 */
   case 0x196: return 0x19e;   /* SPI_PS_INPUT_CNTL_5 */
   case 0x197: return 0x19f;   /* SPI_PS_INPUT_CNTL_6 */
   case 0x198: return 0x1a0;   /* SPI_PS_INPUT_CNTL_7 */
   case 0x199: return 0x1a1;   /* SPI_PS_INPUT_CNTL_8 */
   case 0x19a: return 0x1a2;   /* SPI_PS_INPUT_CNTL_9 */
   case 0x19b: return 0x1a3;   /* SPI_PS_INPUT_CNTL_10 */
   case 0x19c: return 0x1a4;   /* SPI_PS_INPUT_CNTL_11 */
   case 0x19d: return 0x1a5;   /* SPI_PS_INPUT_CNTL_12 */
   case 0x19e: return 0x1a6;   /* SPI_PS_INPUT_CNTL_13 */
   case 0x19f: return 0x1a7;   /* SPI_PS_INPUT_CNTL_14 */
   case 0x1a0: return 0x1a8;   /* SPI_PS_INPUT_CNTL_15 */
   case 0x1a1: return 0x1a9;   /* SPI_PS_INPUT_CNTL_16 */
   case 0x1a2: return 0x1aa;   /* SPI_PS_INPUT_CNTL_17 */
   case 0x1a3: return 0x1ab;   /* SPI_PS_INPUT_CNTL_18 */
   case 0x1a4: return 0x1ac;   /* SPI_PS_INPUT_CNTL_19 */
   case 0x1a5: return 0x1ad;   /* SPI_PS_INPUT_CNTL_20 */
   case 0x1a6: return 0x1ae;   /* SPI_PS_INPUT_CNTL_21 */
   case 0x1a7: return 0x1af;   /* SPI_PS_INPUT_CNTL_22 */
   case 0x1a8: return 0x1b0;   /* SPI_PS_INPUT_CNTL_23 */
   case 0x1a9: return 0x1b1;   /* SPI_PS_INPUT_CNTL_24 */
   case 0x1aa: return 0x1b2;   /* SPI_PS_INPUT_CNTL_25 */
   case 0x1ab: return 0x1b3;   /* SPI_PS_INPUT_CNTL_26 */
   case 0x1ac: return 0x1b4;   /* SPI_PS_INPUT_CNTL_27 */
   case 0x1ad: return 0x1b5;   /* SPI_PS_INPUT_CNTL_28 */
   case 0x1ae: return 0x1b6;   /* SPI_PS_INPUT_CNTL_29 */
   case 0x1af: return 0x1b7;   /* SPI_PS_INPUT_CNTL_30 */
   case 0x1b0: return 0x1b8;   /* SPI_PS_INPUT_CNTL_31 */
   case 0x1b1: return 0x1c3;   /* SPI_VS_OUT_CONFIG */
   case 0x1b3: return 0x197;   /* SPI_PS_INPUT_ENA */
   case 0x1b4: return 0x198;   /* SPI_PS_INPUT_ADDR */
   case 0x1b5: return 0x191;   /* SPI_INTERP_CONTROL_0 */
   case 0x1b6: return 0x1c4;   /* SPI_PS_IN_CONTROL */
   case 0x1b7: return 0x1c2;   /* SPI_BARYC_SSAA_CNTL */
   case 0x1b8: return 0x196;   /* SPI_BARYC_CNTL */
   case 0x1c2: return 0x192;   /* SPI_SHADER_IDX_FORMAT */
   case 0x1c3: return 0x193;   /* SPI_SHADER_POS_FORMAT */
   case 0x1c4: return 0x194;   /* SPI_SHADER_Z_FORMAT */
   case 0x1c5: return 0x195;   /* SPI_SHADER_COL_FORMAT */
   case 0x201: return 0x01b;   /* DB_EQAA */
   case 0x202: return 0x378;   /* CB_COLOR_CONTROL */
   case 0x204: return 0x205;   /* PA_CL_CLIP_CNTL */
   case 0x205: return 0x204;   /* PA_SU_SC_MODE_CNTL */
   case 0x291: return 0x2a9;   /* VGT_GS_ONCHIP_CNTL */
   case 0x292: return 0x310;   /* PA_SC_MODE_CNTL_0 */
   case 0x2a1: return 0x2a5;   /* VGT_PRIMITIVEID_EN */
   case 0x2a6: return 0x2a8;   /* VGT_DRAW_PAYLOAD_CNTL */
   case 0x2aa: return 0x2db;   /* IA_MULTI_VGT_PARAM */
   case 0x2ab: return 0x2d0;   /* VGT_ESGS_RING_ITEMSIZE */
   case 0x2ad: return 0x2a7;   /* VGT_REUSE_OFF */
   case 0x2af: return 0x018;   /* DB_HTILE_SURFACE */
   case 0x2d5: return 0x2a6;   /* VGT_SHADER_STAGES_EN */
   case 0x2db: return 0x2aa;   /* VGT_TF_PARAM */
   case 0x2dc: return 0x01c;   /* DB_ALPHA_TO_MASK */
   case 0x2e4: return 0x2cf;   /* VGT_GS_INSTANCE_CNT */
   case 0x2f5: return 0x2fc;   /* PA_SC_CENTROID_PRIORITY_0 */
   case 0x2f6: return 0x2fd;   /* PA_SC_CENTROID_PRIORITY_1 */
   case 0x2fa: return 0x10d;   /* PA_CL_GB_VERT_CLIP_ADJ */
   case 0x2fb: return 0x10e;   /* PA_CL_GB_VERT_DISC_ADJ */
   case 0x2fc: return 0x10f;   /* PA_CL_GB_HORZ_CLIP_ADJ */
   case 0x2fd: return 0x110;   /* PA_CL_GB_HORZ_DISC_ADJ */
   case 0x310: return 0x316;   /* PA_SC_SHADER_CONTROL */
   case 0x313: return 0x315;   /* PA_SC_CONSERVATIVE_RASTERIZATION_CNTL */
   case 0x315: return 0x313;   /* PA_SC_BINNER_CNTL_2 */
   case 0x31b: return 0x319;   /* CB_COLOR0_VIEW */
   case 0x31c: return 0x360;   /* CB_COLOR0_INFO */
   case 0x31d: return 0x31a;   /* CB_COLOR0_ATTRIB */
   case 0x31e: return 0x31b;   /* CB_COLOR0_DCC_CONTROL */
   case 0x31f: return 0x31c;   /* CB_COLOR0_CMASK */
   case 0x321: return 0x31d;   /* CB_COLOR0_FMASK */
   case 0x323: return 0x368;   /* CB_COLOR0_CLEAR_WORD0 */
   case 0x324: return 0x369;   /* CB_COLOR0_CLEAR_WORD1 */
   case 0x325: return 0x31e;   /* CB_COLOR0_DCC_BASE */
   case 0x327: return 0x321;   /* CB_COLOR1_BASE */
   case 0x32a: return 0x322;   /* CB_COLOR1_VIEW */
   case 0x32b: return 0x361;   /* CB_COLOR1_INFO */
   case 0x32c: return 0x323;   /* CB_COLOR1_ATTRIB */
   case 0x32d: return 0x324;   /* CB_COLOR1_DCC_CONTROL */
   case 0x32e: return 0x325;   /* CB_COLOR1_CMASK */
   case 0x330: return 0x326;   /* CB_COLOR1_FMASK */
   case 0x332: return 0x36a;   /* CB_COLOR1_CLEAR_WORD0 */
   case 0x333: return 0x36b;   /* CB_COLOR1_CLEAR_WORD1 */
   case 0x334: return 0x327;   /* CB_COLOR1_DCC_BASE */
   case 0x336: return 0x32a;   /* CB_COLOR2_BASE */
   case 0x339: return 0x32b;   /* CB_COLOR2_VIEW */
   case 0x33a: return 0x362;   /* CB_COLOR2_INFO */
   case 0x33b: return 0x32c;   /* CB_COLOR2_ATTRIB */
   case 0x33c: return 0x32d;   /* CB_COLOR2_DCC_CONTROL */
   case 0x33d: return 0x32e;   /* CB_COLOR2_CMASK */
   case 0x33f: return 0x32f;   /* CB_COLOR2_FMASK */
   case 0x341: return 0x36c;   /* CB_COLOR2_CLEAR_WORD0 */
   case 0x342: return 0x36d;   /* CB_COLOR2_CLEAR_WORD1 */
   case 0x343: return 0x330;   /* CB_COLOR2_DCC_BASE */
   case 0x345: return 0x333;   /* CB_COLOR3_BASE */
   case 0x348: return 0x334;   /* CB_COLOR3_VIEW */
   case 0x349: return 0x363;   /* CB_COLOR3_INFO */
   case 0x34a: return 0x335;   /* CB_COLOR3_ATTRIB */
   case 0x34b: return 0x336;   /* CB_COLOR3_DCC_CONTROL */
   case 0x34c: return 0x337;   /* CB_COLOR3_CMASK */
   case 0x34e: return 0x338;   /* CB_COLOR3_FMASK */
   case 0x350: return 0x36e;   /* CB_COLOR3_CLEAR_WORD0 */
   case 0x351: return 0x36f;   /* CB_COLOR3_CLEAR_WORD1 */
   case 0x352: return 0x339;   /* CB_COLOR3_DCC_BASE */
   case 0x354: return 0x33c;   /* CB_COLOR4_BASE */
   case 0x357: return 0x33d;   /* CB_COLOR4_VIEW */
   case 0x358: return 0x364;   /* CB_COLOR4_INFO */
   case 0x359: return 0x33e;   /* CB_COLOR4_ATTRIB */
   case 0x35a: return 0x33f;   /* CB_COLOR4_DCC_CONTROL */
   case 0x35b: return 0x340;   /* CB_COLOR4_CMASK */
   case 0x35d: return 0x341;   /* CB_COLOR4_FMASK */
   case 0x35f: return 0x370;   /* CB_COLOR4_CLEAR_WORD0 */
   case 0x360: return 0x371;   /* CB_COLOR4_CLEAR_WORD1 */
   case 0x361: return 0x342;   /* CB_COLOR4_DCC_BASE */
   case 0x363: return 0x345;   /* CB_COLOR5_BASE */
   case 0x366: return 0x346;   /* CB_COLOR5_VIEW */
   case 0x367: return 0x365;   /* CB_COLOR5_INFO */
   case 0x368: return 0x347;   /* CB_COLOR5_ATTRIB */
   case 0x369: return 0x348;   /* CB_COLOR5_DCC_CONTROL */
   case 0x36a: return 0x349;   /* CB_COLOR5_CMASK */
   case 0x36c: return 0x34a;   /* CB_COLOR5_FMASK */
   case 0x36e: return 0x372;   /* CB_COLOR5_CLEAR_WORD0 */
   case 0x36f: return 0x373;   /* CB_COLOR5_CLEAR_WORD1 */
   case 0x370: return 0x34b;   /* CB_COLOR5_DCC_BASE */
   case 0x372: return 0x34e;   /* CB_COLOR6_BASE */
   case 0x375: return 0x34f;   /* CB_COLOR6_VIEW */
   case 0x376: return 0x366;   /* CB_COLOR6_INFO */
   case 0x377: return 0x350;   /* CB_COLOR6_ATTRIB */
   case 0x378: return 0x351;   /* CB_COLOR6_DCC_CONTROL */
   case 0x379: return 0x352;   /* CB_COLOR6_CMASK */
   case 0x37b: return 0x353;   /* CB_COLOR6_FMASK */
   case 0x37d: return 0x374;   /* CB_COLOR6_CLEAR_WORD0 */
   case 0x37e: return 0x375;   /* CB_COLOR6_CLEAR_WORD1 */
   case 0x37f: return 0x354;   /* CB_COLOR6_DCC_BASE */
   case 0x381: return 0x357;   /* CB_COLOR7_BASE */
   case 0x384: return 0x358;   /* CB_COLOR7_VIEW */
   case 0x385: return 0x367;   /* CB_COLOR7_INFO */
   case 0x386: return 0x359;   /* CB_COLOR7_ATTRIB */
   case 0x387: return 0x35a;   /* CB_COLOR7_DCC_CONTROL */
   case 0x388: return 0x35b;   /* CB_COLOR7_CMASK */
   case 0x38a: return 0x35c;   /* CB_COLOR7_FMASK */
   case 0x38c: return 0x376;   /* CB_COLOR7_CLEAR_WORD0 */
   case 0x38d: return 0x377;   /* CB_COLOR7_CLEAR_WORD1 */
   case 0x38e: return 0x35d;   /* CB_COLOR7_DCC_BASE */
   case 0x3b0: return 0x31f;   /* CB_COLOR0_ATTRIB2 */
   case 0x3b1: return 0x328;   /* CB_COLOR1_ATTRIB2 */
   case 0x3b2: return 0x331;   /* CB_COLOR2_ATTRIB2 */
   case 0x3b3: return 0x33a;   /* CB_COLOR3_ATTRIB2 */
   case 0x3b4: return 0x343;   /* CB_COLOR4_ATTRIB2 */
   case 0x3b5: return 0x34c;   /* CB_COLOR5_ATTRIB2 */
   case 0x3b6: return 0x355;   /* CB_COLOR6_ATTRIB2 */
   case 0x3b7: return 0x35e;   /* CB_COLOR7_ATTRIB2 */
   case 0x3b8: return 0x320;   /* CB_COLOR0_ATTRIB3 */
   case 0x3b9: return 0x329;   /* CB_COLOR1_ATTRIB3 */
   case 0x3ba: return 0x332;   /* CB_COLOR2_ATTRIB3 */
   case 0x3bb: return 0x33b;   /* CB_COLOR3_ATTRIB3 */
   case 0x3bc: return 0x344;   /* CB_COLOR4_ATTRIB3 */
   case 0x3bd: return 0x34d;   /* CB_COLOR5_ATTRIB3 */
   case 0x3be: return 0x356;   /* CB_COLOR6_ATTRIB3 */
   case 0x3bf: return 0x35f;   /* CB_COLOR7_ATTRIB3 */
   default: return dw;
   }
}

/* 13 SH registers move on TITAN. */
static inline uint32_t
ac_titan_kmap_sh(uint32_t dw)
{
   switch (dw) {
   case 0x007: return 0x000;   /* SPI_SHADER_PGM_RSRC3_PS */
   case 0x081: return 0x08b;   /* SPI_SHADER_PGM_RSRC4_GS */
   case 0x087: return 0x08a;   /* SPI_SHADER_PGM_RSRC3_GS */
   case 0x08a: return 0x084;   /* SPI_SHADER_PGM_RSRC1_GS */
   case 0x08b: return 0x085;   /* SPI_SHADER_PGM_RSRC2_GS */
   case 0x0c8: return 0x086;   /* SPI_SHADER_PGM_LO_ES */
   case 0x0c9: return 0x087;   /* SPI_SHADER_PGM_HI_ES */
   case 0x101: return 0x10b;   /* SPI_SHADER_PGM_RSRC4_HS */
   case 0x107: return 0x10a;   /* SPI_SHADER_PGM_RSRC3_HS */
   case 0x10a: return 0x104;   /* SPI_SHADER_PGM_RSRC1_HS */
   case 0x10b: return 0x105;   /* SPI_SHADER_PGM_RSRC2_HS */
   case 0x148: return 0x106;   /* SPI_SHADER_PGM_LO_LS */
   case 0x149: return 0x107;   /* SPI_SHADER_PGM_HI_LS */
   default: return dw;
   }
}

/* 2 UCFG registers move on TITAN. */
static inline uint32_t
ac_titan_kmap_ucfg(uint32_t dw)
{
   switch (dw) {
   case 0x261: return 0x262;   /* VGT_TF_MEMORY_BASE_HI */
   case 0x262: return 0x261;   /* GE_USER_VGPR_EN */
   default: return dw;
   }
}

/* Registers that change space (different PM4 opcode), handled at their emission site:
 *   VGT_GS_OUT_PRIM_TYPE               CTX 0x29b -> UCFG 0x241
 */
#define AC_TITAN_KMAP_SPACE_CHANGES 1

#endif
