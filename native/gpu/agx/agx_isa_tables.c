/* GENERATED: do not edit by hand.
 * Source: Mesa src/asahi/isa/AGX2.xml at commit ba3dacb9832bd073198d22c402f4e0d75ec3a09a
 * (MIT, Copyright 2025 Valve Corporation), parsed with Mesa's isa.py, plus
 * AienOS additions for encodings observed in Apple's compiler output that
 * AGX2.xml does not describe (entries with .aienos = 1; enums "tgunk" and "b26").
 * Generator: ~/.hermes/cache/scratch/agx-work/scripts/gen_tables.py (L214). */
#include "agx_isa.h"

static const struct agx_enum_value enum_hint[] = {
	{1, "none", 1},
	{2, "cache", 0},
	{3, "discard", 0},
};
static const struct agx_enum_value enum_interpolation[] = {
	{0, "center", 1},
	{1, "sample", 0},
	{2, "centroid", 0},
	{3, "sample", 0},
};
static const struct agx_enum_value enum_coherent[] = {
	{0, "incoherent", 1},
	{1, "coherent1", 0},
	{2, "coherent2", 0},
	{3, "coherent", 0},
};
static const struct agx_enum_value enum_atomic[] = {
	{0, "add", 0},
	{1, "sub", 0},
	{2, "xchg", 0},
	{3, "cmpxchg", 0},
	{4, "umin", 0},
	{5, "imin", 0},
	{6, "umax", 0},
	{7, "imax", 0},
	{8, "and", 0},
	{9, "or", 0},
	{10, "xor", 0},
};
static const struct agx_enum_value enum_sr[] = {
	{0, "threadgroup_in_grid_x", 0},
	{1, "threadgroup_in_grid_y", 0},
	{2, "threadgroup_in_grid_z", 0},
	{4, "threads_per_threadgroup_x", 0},
	{5, "threads_per_threadgroup_y", 0},
	{6, "threads_per_threadgroup_z", 0},
	{8, "dispatch_threads_per_threadgroup_x", 0},
	{9, "dispatch_threads_per_threadgroup_y", 0},
	{10, "dispatch_threads_per_threadgroup_z", 0},
	{12, "threadgroup_width", 0},
	{13, "threadgroup_height", 0},
	{14, "samples_log2", 0},
	{20, "core_index", 0},
	{21, "vm_slot", 0},
	{48, "thread_in_threadgroup_x", 0},
	{49, "thread_in_threadgroup_y", 0},
	{50, "thread_in_threadgroup_z", 0},
	{51, "thread_index_in_threadgroup", 0},
	{52, "thread_index_in_simdgroup", 0},
	{53, "simdgroup_index_in_threadgroup", 0},
	{56, "active_thread_index_in_quadgroup", 0},
	{57, "total_active_threads_in_quadgroup", 0},
	{58, "active_thread_index_in_simdgroup", 0},
	{59, "total_active_threads_in_simdgroup", 0},
	{60, "internal_coverage_mask", 0},
	{62, "backfacing", 0},
	{63, "is_active_thread", 0},
	{80, "thread_in_grid_x", 0},
	{81, "thread_in_grid_y", 0},
	{82, "thread_in_grid_z", 0},
	{124, "input_sample_mask", 0},
	{144, "helper_op", 0},
	{146, "helper_arg_l", 0},
	{147, "helper_arg_h", 0},
};
static const struct agx_enum_value enum_round[] = {
	{0, "rtz", 0},
	{1, "rte", 0},
};
static const struct agx_enum_value enum_group[] = {
	{0, "a", 0},
	{1, "b", 0},
};
static const struct agx_enum_value enum_shift[] = {
	{0, NULL, 0},
	{1, "lsl 1", 0},
	{2, "lsl 2", 0},
	{3, "lsl 3", 0},
	{4, "lsl 4", 0},
};
static const struct agx_enum_value enum_ext[] = {
	{0, "sx", 0},
	{1, "zx", 0},
};
static const struct agx_enum_value enum_icond[] = {
	{0, "ieq", 0},
	{1, "ult", 0},
	{2, "ugt", 0},
	{4, "seq", 0},
	{5, "slt", 0},
	{6, "sgt", 0},
	{8, "ine", 0},
	{9, "ugte", 0},
	{10, "ulte", 0},
	{12, "nseq", 0},
	{13, "sgte", 0},
	{14, "slte", 0},
};
static const struct agx_enum_value enum_fcond[] = {
	{0, "feq", 0},
	{1, "flt", 0},
	{2, "fgt", 0},
	{3, "fltn", 0},
	{5, "fgte", 0},
	{6, "flte", 0},
	{7, "fgtn", 0},
	{8, "fneq", 0},
	{9, "fnlt", 0},
	{10, "fngt", 0},
	{11, "fnltn", 0},
	{13, "fngte", 0},
	{14, "fnlte", 0},
	{15, "fngtn", 0},
};
static const struct agx_enum_value enum_mask[] = {
	{0, "none", 0},
	{1, "x", 0},
	{2, "y", 0},
	{3, "xy", 0},
	{4, "z", 0},
	{5, "xz", 0},
	{6, "yz", 0},
	{7, "xyz", 0},
	{8, "w", 0},
	{9, "xw", 0},
	{10, "yw", 0},
	{11, "xyw", 0},
	{12, "zw", 0},
	{13, "xzw", 0},
	{14, "yzw", 0},
	{15, "xyzw", 0},
};
static const struct agx_enum_value enum_format[] = {
	{0, "i8", 0},
	{1, "i16", 0},
	{2, "i32", 0},
	{3, "f16", 0},
	{4, "u8norm", 0},
	{5, "s8norm", 0},
	{6, "u16norm", 0},
	{7, "s16norm", 0},
	{8, "rgb10a2", 0},
	{10, "srgba8", 0},
	{12, "rg11b10f", 0},
	{13, "rgb9e5", 0},
	{14, "rg11b10f_rtz", 0},
	{15, "rgb9e5_rtz", 0},
};
static const struct agx_enum_value enum_dim[] = {
	{0, "1d", 0},
	{1, "1d_array", 0},
	{2, "2d", 0},
	{3, "2d_array", 0},
	{4, "2d_ms", 0},
	{5, "3d", 0},
	{6, "cube", 0},
	{7, "cube_array", 0},
	{8, "2d_ms_array", 0},
};
static const struct agx_enum_value enum_gather[] = {
	{0, "none", 1},
	{1, "gather_r", 0},
	{3, "gather_g", 0},
	{5, "gather_b", 0},
	{7, "gather_a", 0},
};
static const struct agx_enum_value enum_lod_type[] = {
	{0, "auto_lod", 0},
	{1, "auto_lod_bias", 0},
	{2, "lod_min", 0},
	{4, "lod_grad", 0},
	{5, "auto_lod_bias", 0},
	{6, "lod_min", 0},
	{9, "auto_lod_bias_min", 0},
	{12, "lod_grad_min", 0},
	{13, "auto_lod_bias_min", 0},
};
static const struct agx_enum_value enum_quad[] = {
	{0, "quad", 0},
	{1, "simd", 1},
};
static const struct agx_enum_value enum_shuffle_op[] = {
	{0, "none", 1},
	{1, "xor", 0},
	{2, "up", 0},
	{3, "down", 0},
};
static const struct agx_enum_value enum_subop[] = {
	{0, "and", 0},
	{1, "fadd", 0},
	{2, "or", 0},
	{3, "fmul", 0},
	{4, "xor", 0},
	{5, "fmin", 0},
	{7, "fmax", 0},
	{16, "iadd", 0},
	{20, "smin", 0},
	{22, "smax", 0},
	{28, "umin", 0},
	{30, "umax", 0},
};
static const struct agx_enum_value enum_tgunk[] = {
	{0, NULL, 1},
	{1, "tg1", 0},
	{2, "tg2", 0},
	{3, "tg3", 0},
	{4, "tg4", 0},
	{5, "tg5", 0},
	{6, "tg6", 0},
	{7, "tg7", 0},
};
static const struct agx_enum_value enum_b26[] = {
	{1, NULL, 1},
	{0, "b26clr", 0},
};
const struct agx_enum agx_enums[] = {
	{"hint", 3, enum_hint},
	{"interpolation", 4, enum_interpolation},
	{"coherent", 4, enum_coherent},
	{"atomic", 11, enum_atomic},
	{"sr", 34, enum_sr},
	{"round", 2, enum_round},
	{"group", 2, enum_group},
	{"shift", 5, enum_shift},
	{"ext", 2, enum_ext},
	{"icond", 12, enum_icond},
	{"fcond", 14, enum_fcond},
	{"mask", 16, enum_mask},
	{"format", 14, enum_format},
	{"dim", 9, enum_dim},
	{"gather", 5, enum_gather},
	{"lod_type", 9, enum_lod_type},
	{"quad", 2, enum_quad},
	{"shuffle_op", 4, enum_shuffle_op},
	{"subop", 12, enum_subop},
	{"tgunk", 8, enum_tgunk},
	{"b26", 2, enum_b26},
};
const unsigned agx_enum_count = 21;

static const struct agx_field enc_diter_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{0, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "count", .display = "count", .bits = {1, {{9, 2}}}},
};
static const struct agx_case enc_diter_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{1, 8}}}},
	 .nmods = 2, .mods = enc_diter_c0_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_dldcf_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{0, 1}}}},
};
static const struct agx_prop enc_dldcf_c0_props[] = {
	{"count", 3},
};
static const struct agx_case enc_dldcf_cases[] = {
	{.exact_mask = 0x600ull, .exact_value = 0x200ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{1, 8}}}},
	 .nmods = 1, .mods = enc_dldcf_c0_mods, .nprops = 1, .props = enc_dldcf_c0_props},
};
static const struct agx_field enc_dalu_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{0, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{1, 1}}}},
};
static const struct agx_case enc_dalu_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{2, 8}}}},
	 .nmods = 2, .mods = enc_dalu_c0_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_dhalf_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{0, 1}}}},
};
static const struct agx_case enc_dhalf_cases[] = {
	{.exact_mask = 0x2ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{2, 8}}}},
	 .nmods = 1, .mods = enc_dhalf_c0_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_dalu64_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{0, 1}}}},
};
static const struct agx_prop enc_dalu64_c0_props[] = {
	{"size64", 1},
};
static const struct agx_field enc_dalu64_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{0, 1}}}},
};
static const struct agx_prop enc_dalu64_c1_props[] = {
	{"size32", 1},
};
static const struct agx_field enc_dalu64_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{0, 1}}}},
};
static const struct agx_prop enc_dalu64_c2_props[] = {
	{"size16", 1},
};
static const struct agx_case enc_dalu64_cases[] = {
	{.exact_mask = 0x6ull, .exact_value = 0x6ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{3, 7}}}},
	 .nmods = 1, .mods = enc_dalu64_c0_mods, .nprops = 1, .props = enc_dalu64_c0_props},
	{.exact_mask = 0x6ull, .exact_value = 0x2ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{3, 7}}}},
	 .nmods = 1, .mods = enc_dalu64_c1_mods, .nprops = 1, .props = enc_dalu64_c1_props},
	{.exact_mask = 0x2ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{2, 8}}}},
	 .nmods = 1, .mods = enc_dalu64_c2_mods, .nprops = 1, .props = enc_dalu64_c2_props},
};
static const struct agx_case enc_ts_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "ts", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_ss_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "ss", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_uniform_dest_c0_props[] = {
	{"size64", 1},
};
static const struct agx_prop enc_uniform_dest_c1_props[] = {
	{"size32", 1},
};
static const struct agx_prop enc_uniform_dest_c2_props[] = {
	{"count", 4},
};
static const struct agx_prop enc_uniform_dest_c3_props[] = {
	{"count", 3},
};
static const struct agx_prop enc_uniform_dest_c4_props[] = {
	{"count", 2},
};
static const struct agx_case enc_uniform_dest_cases[] = {
	{.exact_mask = 0xf0001ull, .exact_value = 0xf0000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_dest_c0_props},
	{.exact_mask = 0xf0001ull, .exact_value = 0x30000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_dest_c1_props},
	{.exact_mask = 0xf0000ull, .exact_value = 0xf0000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_dest_c2_props},
	{.exact_mask = 0xf0000ull, .exact_value = 0x70000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_dest_c3_props},
	{.exact_mask = 0xf0000ull, .exact_value = 0x30000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_dest_c4_props},
	{.exact_mask = 0xf0000ull, .exact_value = 0x10000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_uniform_src_c0_props[] = {
	{"size64", 1},
};
static const struct agx_prop enc_uniform_src_c1_props[] = {
	{"size32", 1},
};
static const struct agx_prop enc_uniform_src_c2_props[] = {
	{"count", 4},
};
static const struct agx_prop enc_uniform_src_c3_props[] = {
	{"count", 3},
};
static const struct agx_prop enc_uniform_src_c4_props[] = {
	{"count", 2},
};
static const struct agx_case enc_uniform_src_cases[] = {
	{.exact_mask = 0xf01ull, .exact_value = 0xf00ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_src_c0_props},
	{.exact_mask = 0xf01ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_src_c1_props},
	{.exact_mask = 0xf00ull, .exact_value = 0xf00ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_src_c2_props},
	{.exact_mask = 0xf00ull, .exact_value = 0x700ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_src_c3_props},
	{.exact_mask = 0xf00ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_uniform_src_c4_props},
	{.exact_mask = 0xf00ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_coord_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "lu", .display = "lu", .bits = {1, {{8, 1}}}},
};
static const struct agx_prop enc_coord_c0_props[] = {
	{"size32", 1},
};
static const struct agx_field enc_coord_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "lu", .display = "lu", .bits = {1, {{8, 1}}}},
};
static const struct agx_prop enc_coord_c1_props[] = {
	{"size16", 1},
};
static const struct agx_case enc_coord_cases[] = {
	{.exact_mask = 0x200ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 1, .mods = enc_coord_c0_mods, .nprops = 1, .props = enc_coord_c0_props},
	{.exact_mask = 0x200ull, .exact_value = 0x200ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 1, .mods = enc_coord_c1_mods, .nprops = 1, .props = enc_coord_c1_props},
};
static const struct agx_prop enc_lod_c1_props[] = {
	{"size32", 1},
	{"count", 2},
};
static const struct agx_prop enc_lod_c2_props[] = {
	{"size16", 1},
	{"count", 5},
};
static const struct agx_prop enc_lod_c5_props[] = {
	{"count", 2},
};
static const struct agx_prop enc_lod_c6_props[] = {
	{"count", 2},
};
static const struct agx_case enc_lod_cases[] = {
	{.exact_mask = 0xfffull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 1}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0xf00ull, .exact_value = 0x400ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_lod_c1_props},
	{.exact_mask = 0xf00ull, .exact_value = 0xc00ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_lod_c2_props},
	{.exact_mask = 0xf00ull, .exact_value = 0x200ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0xf00ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0xf00ull, .exact_value = 0x900ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_lod_c5_props},
	{.exact_mask = 0xf00ull, .exact_value = 0xd00ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_lod_c6_props},
	{.exact_mask = 0xf00ull, .exact_value = 0x600ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0xf00ull, .exact_value = 0x500ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_ureg64_c0_props[] = {
	{"size64", 1},
};
static const struct agx_case enc_ureg64_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_ureg64_c0_props},
};
static const struct agx_prop enc_ureg64_shift2_c0_props[] = {
	{"size64", 1},
};
static const struct agx_case enc_ureg64_shift2_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 2, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 7}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_ureg64_shift2_c0_props},
};
static const struct agx_case enc_exreg16_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_exreg32_c0_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_exreg32_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_exreg32_c0_props},
};
static const struct agx_prop enc_heap_offs_c0_props[] = {
	{"size32", 1},
};
static const struct agx_prop enc_heap_offs_c1_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_heap_offs_cases[] = {
	{.exact_mask = 0x80000000ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 9}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_heap_offs_c0_props},
	{.exact_mask = 0x80000000ull, .exact_value = 0x80000000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 31}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_heap_offs_c1_props},
};
static const struct agx_prop enc_atomic_dest_c0_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_atomic_dest_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_atomic_dest_c0_props},
	{.exact_mask = 0x1ffull, .exact_value = 0x0ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_local_atomic_dest_c0_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_local_atomic_dest_cases[] = {
	{.exact_mask = 0x300ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_local_atomic_dest_c0_props},
	{.exact_mask = 0x1ffull, .exact_value = 0x0ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_atomic_source_c0_props[] = {
	{"count", 2},
	{"size32", 1},
};
static const struct agx_prop enc_atomic_source_c1_props[] = {
	{"count", 1},
	{"size32", 1},
};
static const struct agx_case enc_atomic_source_cases[] = {
	{.exact_mask = 0xf00ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_atomic_source_c0_props},
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_atomic_source_c1_props},
};
static const struct agx_field enc_memory_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{8, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "mask", .display = "mask", .bits = {1, {{9, 4}}}},
};
static const struct agx_case enc_memory_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 2, .mods = enc_memory_c0_mods, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_uvs_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_cf_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cf", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_cfperspective_cases[] = {
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cf", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_itersample_cases[] = {
	{.exact_mask = 0x300ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x1ffull, .exact_value = 0x0ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_pbesource_c0_props[] = {
	{"size16", 1},
	{"count", 4},
};
static const struct agx_prop enc_pbesource_c1_props[] = {
	{"size32", 1},
	{"count", 4},
};
static const struct agx_case enc_pbesource_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_pbesource_c0_props},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 2, .props = enc_pbesource_c1_props},
};
static const struct agx_prop enc_membase_c0_props[] = {
	{"size64", 1},
};
static const struct agx_prop enc_membase_c1_props[] = {
	{"size64", 1},
};
static const struct agx_case enc_membase_cases[] = {
	{.exact_mask = 0x101ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{1, 7}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_membase_c0_props},
	{.exact_mask = 0x101ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{1, 7}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_membase_c1_props},
};
static const struct agx_case enc_localmembase_cases[] = {
	{.exact_mask = 0x300ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x3ffull, .exact_value = 0x200ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {2, {{0, 8}, {9, 1}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_memindex_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "zx", .display = "zx", .bits = {1, {{17, 1}}}},
};
static const struct agx_prop enc_memindex_c0_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_memindex_cases[] = {
	{.exact_mask = 0x1ff01ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 1, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{1, 7}}}},
	 .nmods = 1, .mods = enc_memindex_c0_mods, .nprops = 1, .props = enc_memindex_c0_props},
	{.exact_mask = 0x30000ull, .exact_value = 0x10000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_SIGNED, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x30000ull, .exact_value = 0x30000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_localmemindex_cases[] = {
	{.exact_mask = 0x1ff00ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x10000ull, .exact_value = 0x10000ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_SIGNED, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 16}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_cmpsel_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{11, 1}}}},
};
static const struct agx_field enc_cmpsel_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{11, 1}}}},
};
static const struct agx_case enc_cmpsel_cases[] = {
	{.exact_mask = 0x1c0ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {2, {{0, 6}, {9, 2}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {9, 2}}}},
	 .nmods = 2, .mods = enc_cmpsel_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x180ull, .exact_value = 0x180ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {9, 2}, {6, 1}}}},
	 .nmods = 1, .mods = enc_cmpsel_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_alu_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{7, 1}}}},
};
static const struct agx_field enc_alu_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{9, 1}}}},
};
static const struct agx_case enc_alu_cases[] = {
	{.exact_mask = 0x3c0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {2, {{0, 6}, {10, 2}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {10, 2}, {6, 1}}}},
	 .nmods = 1, .mods = enc_alu_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {10, 2}}}},
	 .nmods = 2, .mods = enc_alu_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_mul_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
};
static const struct agx_field enc_mul_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{7, 1}}}},
};
static const struct agx_field enc_mul_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{9, 1}}}},
};
static const struct agx_case enc_mul_cases[] = {
	{.exact_mask = 0x3c0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 1, .mods = enc_mul_c0_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {11, 2}, {6, 1}}}},
	 .nmods = 2, .mods = enc_mul_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 3, .mods = enc_mul_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_add_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
};
static const struct agx_field enc_add_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{7, 1}}}},
};
static const struct agx_field enc_add_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sx", .display = "sx", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size64", .display = "size64", .bits = {1, {{8, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{9, 1}}}},
};
static const struct agx_case enc_add_cases[] = {
	{.exact_mask = 0x3c0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 1, .mods = enc_add_c0_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {11, 2}, {6, 1}}}},
	 .nmods = 2, .mods = enc_add_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 4, .mods = enc_add_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_float_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{11, 1}}}},
};
static const struct agx_field enc_float_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{11, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{7, 1}}}},
};
static const struct agx_field enc_float_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{11, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "size32", .display = "size32", .bits = {1, {{9, 1}}}},
};
static const struct agx_case enc_float_cases[] = {
	{.exact_mask = 0x3c0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "fimm", .display = NULL, .bits = {2, {{0, 6}, {12, 2}}}},
	 .nmods = 2, .mods = enc_float_c0_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {12, 2}, {6, 1}}}},
	 .nmods = 3, .mods = enc_float_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {12, 2}}}},
	 .nmods = 4, .mods = enc_float_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_field enc_half_c0_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{9, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{10, 1}}}},
};
static const struct agx_field enc_half_c1_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{9, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{10, 1}}}},
};
static const struct agx_field enc_half_c2_mods[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "abs", .display = "abs", .bits = {1, {{9, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "neg", .display = "neg", .bits = {1, {{10, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "hint", .display = "hint", .bits = {1, {{6, 2}}}},
};
static const struct agx_case enc_half_cases[] = {
	{.exact_mask = 0x7c0ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "fimm", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 2, .mods = enc_half_c0_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x180ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "uniform", .display = NULL, .bits = {3, {{0, 6}, {11, 2}, {6, 1}}}},
	 .nmods = 2, .mods = enc_half_c1_mods, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {2, {{0, 6}, {11, 2}}}},
	 .nmods = 3, .mods = enc_half_c2_mods, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_discard_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_case enc_samples_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
static const struct agx_prop enc_tilecoord_c1_props[] = {
	{"count", 2},
};
static const struct agx_case enc_tilecoord_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_tilecoord_c1_props},
};
static const struct agx_prop enc_zs_c1_props[] = {
	{"size32", 1},
};
static const struct agx_prop enc_zs_c3_props[] = {
	{"count", 3},
};
static const struct agx_case enc_zs_cases[] = {
	{.exact_mask = 0x3ffull, .exact_value = 0x0ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_zs_c1_props},
	{.exact_mask = 0x300ull, .exact_value = 0x200ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_zs_c3_props},
};
static const struct agx_prop enc_heap_c1_props[] = {
	{"size64", 1},
};
static const struct agx_case enc_heap_cases[] = {
	{.exact_mask = 0x3full, .exact_value = 0x0ull, .has_imm = 0,
	 .imm = {0},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x20ull, .exact_value = 0x20ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 2, .ref = -1, .name = "uniform", .display = NULL, .bits = {1, {{0, 5}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_heap_c1_props},
};
static const struct agx_prop enc_texture_c3_props[] = {
	{"size32", 1},
};
static const struct agx_case enc_texture_cases[] = {
	{.exact_mask = 0x300ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "ts", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x200ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x300ull, .exact_value = 0x300ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 1, .props = enc_texture_c3_props},
};
static const struct agx_case enc_sampler_cases[] = {
	{.exact_mask = 0x100ull, .exact_value = 0x0ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "ss", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
	{.exact_mask = 0x100ull, .exact_value = 0x100ull, .has_imm = 1,
	 .imm = {.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "reg", .display = NULL, .bits = {1, {{0, 8}}}},
	 .nmods = 0, .mods = NULL, .nprops = 0, .props = NULL},
};
const struct agx_encoding agx_encodings[] = {
	{"diter", 1, enc_diter_cases},
	{"dldcf", 1, enc_dldcf_cases},
	{"dalu", 1, enc_dalu_cases},
	{"dhalf", 1, enc_dhalf_cases},
	{"dalu64", 3, enc_dalu64_cases},
	{"ts", 1, enc_ts_cases},
	{"ss", 1, enc_ss_cases},
	{"uniform_dest", 6, enc_uniform_dest_cases},
	{"uniform_src", 6, enc_uniform_src_cases},
	{"coord", 2, enc_coord_cases},
	{"lod", 9, enc_lod_cases},
	{"ureg64", 1, enc_ureg64_cases},
	{"ureg64_shift2", 1, enc_ureg64_shift2_cases},
	{"exreg16", 1, enc_exreg16_cases},
	{"exreg32", 1, enc_exreg32_cases},
	{"heap_offs", 2, enc_heap_offs_cases},
	{"atomic_dest", 2, enc_atomic_dest_cases},
	{"local_atomic_dest", 2, enc_local_atomic_dest_cases},
	{"atomic_source", 2, enc_atomic_source_cases},
	{"memory", 1, enc_memory_cases},
	{"uvs", 2, enc_uvs_cases},
	{"cf", 2, enc_cf_cases},
	{"cfperspective", 1, enc_cfperspective_cases},
	{"itersample", 3, enc_itersample_cases},
	{"pbesource", 2, enc_pbesource_cases},
	{"membase", 2, enc_membase_cases},
	{"localmembase", 3, enc_localmembase_cases},
	{"memindex", 3, enc_memindex_cases},
	{"localmemindex", 2, enc_localmemindex_cases},
	{"cmpsel", 3, enc_cmpsel_cases},
	{"alu", 3, enc_alu_cases},
	{"mul", 3, enc_mul_cases},
	{"add", 3, enc_add_cases},
	{"float", 3, enc_float_cases},
	{"half", 3, enc_half_cases},
	{"discard", 2, enc_discard_cases},
	{"samples", 2, enc_samples_cases},
	{"tilecoord", 2, enc_tilecoord_cases},
	{"zs", 4, enc_zs_cases},
	{"heap", 2, enc_heap_cases},
	{"texture", 4, enc_texture_cases},
	{"sampler", 2, enc_sampler_cases},
};
const unsigned agx_encoding_count = 42;

static const struct agx_field ins_ldimm16_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{16, 16}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_ldimm32_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{16, 32}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_mfsr_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {28, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "sr", .display = "sr", .bits = {2, {{16, 6}, {26, 2}}}},
};
static const struct agx_field ins_iadd_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "dalu64", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{16, 11}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{28, 11}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {2, {{39, 1}, {52, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
};
static const struct agx_field ins_isub_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "dalu64", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{16, 11}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{28, 11}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {2, {{39, 1}, {52, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
};
static const struct agx_field ins_imul_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "dalu64", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{16, 11}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{28, 11}, {56, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
};
static const struct agx_field ins_imadd_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "dalu64", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{16, 11}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{28, 11}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{40, 11}, {54, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {2, {{39, 1}, {52, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
};
static const struct agx_field ins_imsub_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 4, .name = "dalu64", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{16, 11}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 31, .name = "mul", .display = NULL, .bits = {2, {{28, 11}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 32, .name = "add", .display = NULL, .bits = {2, {{40, 11}, {54, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {2, {{39, 1}, {52, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
};
static const struct agx_field ins_u8_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_s8_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_f_to_u16_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_f_to_s16_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_u16_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_s16_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_f_to_u32_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_f_to_s32_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_u32_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_s32_to_f_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "round", .display = "round", .bits = {1, {{26, 2}}}},
};
static const struct agx_field ins_shl_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
};
static const struct agx_field ins_shr_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
};
static const struct agx_field ins_bfi_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_bfeil_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_extr_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_shlhi_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_shrhi_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_asr_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_asrh_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{40, 10}, {54, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "bitmask", .display = NULL, .bits = {3, {{38, 2}, {50, 2}, {63, 1}}}},
};
static const struct agx_field ins_ffma_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{40, 12}, {54, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_hfma_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 3, .name = "dhalf", .display = NULL, .bits = {2, {{7, 8}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{16, 11}, {58, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{28, 11}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{40, 11}, {54, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_fadd_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_fmul_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_hadd_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 3, .name = "dhalf", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{16, 11}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{28, 11}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_hmul_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 3, .name = "dhalf", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{16, 11}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 34, .name = "half", .display = NULL, .bits = {2, {{28, 11}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_not_a_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_mov_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_not_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_mov_a_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_zero_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_and_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_andn1_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_andn2_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_xor_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_or_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_nor_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_xnor_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_orn1_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_orn2_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_nand_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_one_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_bitrev_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_popcnt_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_ffs_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
};
static const struct agx_field ins_intl_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
};
static const struct agx_field ins_floor_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_rsqrts_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_rcp_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_rsqrt_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_sinr_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_dfdx_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_dfdy_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_sinc_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_log2_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_exp2_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_ceil_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_trunc_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_rint_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "saturate", .display = "sat", .bits = {1, {{6, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_stop_fields[] = {
};
static const struct agx_field ins_trap_fields[] = {
};
static const struct agx_field ins_jmp_incomplete_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {1, {{16, 8}}}},
};
static const struct agx_field ins_ret_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "exreg32", .display = NULL, .bits = {1, {{9, 7}}}},
};
static const struct agx_field ins_call_reg_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "exreg32", .display = NULL, .bits = {1, {{9, 7}}}},
};
static const struct agx_field ins_jmp_any_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_SIGNED, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {1, {{16, 32}}}},
};
static const struct agx_field ins_call_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_SIGNED, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {1, {{16, 32}}}},
};
static const struct agx_field ins_jmp_none_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_SIGNED, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {1, {{16, 32}}}},
};
static const struct agx_field ins_pop_exec_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
};
static const struct agx_field ins_if_icmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_else_icmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_while_icmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_update_exec_fields[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
};
static const struct agx_field ins_if_fcmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_else_fcmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_while_fcmp_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "nest", .display = NULL, .bits = {1, {{11, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "cache", .display = "cache", .bits = {1, {{7, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "cond", .display = "cond", .bits = {2, {{13, 3}, {8, 1}}}},
};
static const struct agx_field ins_icsel_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {76, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {74, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {72, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 29, .name = "cmpsel", .display = NULL, .bits = {3, {{40, 9}, {70, 2}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 29, .name = "cmpsel", .display = NULL, .bits = {3, {{52, 9}, {68, 2}, {8, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "condition", .display = "condition", .bits = {1, {{61, 3}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_fcsel_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {76, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 12}, {74, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 12}, {72, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 29, .name = "cmpsel", .display = NULL, .bits = {3, {{40, 9}, {70, 2}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 29, .name = "cmpsel", .display = NULL, .bits = {3, {{52, 9}, {68, 2}, {8, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "condition", .display = "condition", .bits = {1, {{61, 3}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_ballot_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
};
static const struct agx_field ins_icmp_ballot_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 16, .name = "quad", .display = "quad", .bits = {1, {{48, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "condition", .display = "condition", .bits = {2, {{61, 3}, {47, 1}}}},
};
static const struct agx_field ins_fcmp_ballot_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 33, .name = "float", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 16, .name = "quad", .display = "quad", .bits = {1, {{48, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "condition", .display = "condition", .bits = {2, {{61, 3}, {47, 1}}}},
};
static const struct agx_field ins_shuffle_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{28, 10}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 16, .name = "quad", .display = "quad", .bits = {1, {{26, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "rotate", .display = "rotate", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 17, .name = "op", .display = "op", .bits = {1, {{38, 2}}}},
};
static const struct agx_field ins_pixwait_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "i", .display = NULL, .bits = {1, {{8, 10}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "j", .display = NULL, .bits = {1, {{22, 2}}}},
};
static const struct agx_field ins_pixrel_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "i", .display = NULL, .bits = {1, {{8, 10}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "j", .display = NULL, .bits = {1, {{22, 2}}}},
};
static const struct agx_field ins_wait_fields[] = {
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "group", .display = "group", .bits = {1, {{8, 1}}}},
};
static const struct agx_field ins_no_var_fields[] = {
};
static const struct agx_field ins_st_var_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "exreg32", .display = NULL, .bits = {2, {{10, 5}, {24, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 20, .name = "uvs", .display = NULL, .bits = {3, {{16, 6}, {26, 2}, {23, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "last", .display = "last", .bits = {1, {{7, 1}}}},
};
static const struct agx_field ins_load_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{10, 6}, {40, 2}, {49, 1}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 25, .name = "membase", .display = NULL, .bits = {3, {{16, 4}, {36, 4}, {27, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {1, {{42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{7, 3}, {48, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "coherency", .display = "coherency", .bits = {1, {{44, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 20, .name = "b26", .display = "b26", .bits = {1, {{26, 1}}}},
};
static const struct agx_field ins_atomic_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 16, .name = "atomic_dest", .display = NULL, .bits = {3, {{10, 6}, {40, 2}, {47, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 18, .name = "atomic_source", .display = NULL, .bits = {2, {{48, 8}, {6, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 25, .name = "membase", .display = NULL, .bits = {3, {{16, 4}, {36, 4}, {27, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 3, .name = "op", .display = "op", .bits = {1, {{6, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "coherency", .display = "coherency", .bits = {1, {{44, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
};
static const struct agx_field ins_latomic_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 17, .name = "local_atomic_dest", .display = NULL, .bits = {4, {{9, 6}, {60, 2}, {38, 1}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 18, .name = "atomic_source", .display = NULL, .bits = {2, {{64, 8}, {24, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 26, .name = "localmembase", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {22, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 28, .name = "localmemindex", .display = NULL, .bits = {3, {{28, 6}, {48, 10}, {34, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 3, .name = "op", .display = "op", .bits = {1, {{24, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_stack_load_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{10, 6}, {40, 2}, {49, 1}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {1, {{42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{8, 2}, {50, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
};
static const struct agx_field ins_stack_adjust_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
};
static const struct agx_field ins_stack_store_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{10, 6}, {40, 2}, {49, 1}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {1, {{42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{8, 2}, {50, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
};
static const struct agx_field ins_uniform_store_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "uniform_dest", .display = NULL, .bits = {4, {{20, 4}, {32, 4}, {56, 8}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 8, .name = "uniform_src", .display = NULL, .bits = {3, {{10, 6}, {40, 2}, {52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
};
static const struct agx_field ins_uniform_store_unk_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{10, 6}, {40, 2}, {49, 1}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {1, {{42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{7, 3}, {48, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
};
static const struct agx_field ins_tex_state_store_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 5, .name = "ts", .display = NULL, .bits = {1, {{8, 7}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "ureg64_shift2", .display = NULL, .bits = {1, {{59, 5}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 15, .name = "heap_offs", .display = NULL, .bits = {2, {{26, 31}, {58, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "group", .display = "group", .bits = {1, {{16, 1}}}},
};
static const struct agx_field ins_sampler_state_store_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "ss", .display = NULL, .bits = {1, {{8, 7}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "ureg64_shift2", .display = NULL, .bits = {1, {{59, 5}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 15, .name = "heap_offs", .display = NULL, .bits = {2, {{26, 31}, {58, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "group", .display = "group", .bits = {1, {{16, 1}}}},
};
static const struct agx_field ins_store_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{10, 6}, {40, 2}, {49, 1}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 25, .name = "membase", .display = NULL, .bits = {3, {{16, 4}, {36, 4}, {27, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 27, .name = "memindex", .display = NULL, .bits = {5, {{20, 4}, {32, 4}, {56, 8}, {24, 1}, {25, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{47, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 7, .name = "shift", .display = "shift", .bits = {1, {{42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{7, 3}, {48, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "coherency", .display = "coherency", .bits = {1, {{44, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "waitgroup", .display = "waitgroup", .bits = {1, {{30, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 20, .name = "b26", .display = "b26", .bits = {1, {{26, 1}}}},
};
static const struct agx_field ins_lload_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {60, 2}, {8, 1}, {36, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 26, .name = "localmembase", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {22, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 28, .name = "localmemindex", .display = NULL, .bits = {3, {{28, 6}, {48, 10}, {34, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {1, {{24, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{36, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "tgu", .display = "tgu", .bits = {1, {{40, 3}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "tg47", .display = "tg47", .bits = {1, {{47, 1}}}},
};
static const struct agx_field ins_lstore_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {60, 2}, {8, 1}, {36, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 26, .name = "localmembase", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {22, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 28, .name = "localmemindex", .display = NULL, .bits = {3, {{28, 6}, {48, 10}, {34, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {1, {{24, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{36, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "tgu", .display = "tgu", .bits = {1, {{40, 3}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "tg47", .display = "tg47", .bits = {1, {{47, 1}}}},
};
static const struct agx_field ins_barrier_fields[] = {
};
static const struct agx_field ins_doorbell_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "imm", .display = NULL, .bits = {1, {{8, 8}}}},
};
static const struct agx_field ins_jmp_if_skipping_doorbell_fields[] = {
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {1, {{16, 8}}}},
};
static const struct agx_field ins_unmap_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "exreg32", .display = NULL, .bits = {2, {{11, 5}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "target", .display = NULL, .bits = {1, {{36, 1}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "v", .display = NULL, .bits = {3, {{20, 4}, {32, 4}, {56, 8}}}},
};
static const struct agx_field ins_map_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "exreg32", .display = NULL, .bits = {2, {{11, 5}, {40, 2}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "target", .display = NULL, .bits = {1, {{36, 1}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "v", .display = NULL, .bits = {3, {{20, 4}, {32, 4}, {56, 8}}}},
};
static const struct agx_field ins_memory_barrier_fields[] = {
};
static const struct agx_field ins_image_barrier_1_fields[] = {
};
static const struct agx_field ins_image_barrier_2_fields[] = {
};
static const struct agx_field ins_image_barrier_3_fields[] = {
};
static const struct agx_field ins_image_barrier_4_fields[] = {
};
static const struct agx_field ins_flush_memory_to_texture_fields[] = {
};
static const struct agx_field ins_memory_barrier_2_fields[] = {
};
static const struct agx_field ins_memory_barrier_3_fields[] = {
};
static const struct agx_field ins_unknown_barrier_1_fields[] = {
};
static const struct agx_field ins_unknown_barrier_2_fields[] = {
};
static const struct agx_field ins_device_barrier_1_fields[] = {
};
static const struct agx_field ins_device_barrier_2_fields[] = {
};
static const struct agx_field ins_quad_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "prefix", .display = "prefix", .bits = {1, {{30, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 18, .name = "op", .display = "op", .bits = {3, {{28, 1}, {38, 3}, {47, 1}}}},
};
static const struct agx_field ins_simd_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "dalu", .display = NULL, .bits = {2, {{7, 8}, {44, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 30, .name = "alu", .display = NULL, .bits = {2, {{16, 10}, {42, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "prefix", .display = "prefix", .bits = {1, {{30, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 18, .name = "op", .display = "op", .bits = {3, {{28, 1}, {38, 3}, {47, 1}}}},
};
static const struct agx_field ins_ld_tile_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {60, 2}, {8, 1}, {36, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 37, .name = "tilecoord", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {55, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 36, .name = "samples", .display = NULL, .bits = {3, {{42, 6}, {56, 2}, {22, 1}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {2, {{28, 7}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {1, {{24, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{36, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_st_tile_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {60, 2}, {8, 1}, {36, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 37, .name = "tilecoord", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {55, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 36, .name = "samples", .display = NULL, .bits = {3, {{42, 6}, {56, 2}, {22, 1}}}},
	{.type = AGX_FT_IMM, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "offset", .display = NULL, .bits = {2, {{28, 7}, {40, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "explicit_coords", .display = "explicit_coords", .bits = {1, {{35, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {1, {{24, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{36, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_sample_mask_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 35, .name = "discard", .display = NULL, .bits = {3, {{9, 6}, {24, 2}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 35, .name = "discard", .display = NULL, .bits = {3, {{16, 6}, {26, 2}, {23, 1}}}},
};
static const struct agx_field ins_zs_emit_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 35, .name = "discard", .display = NULL, .bits = {3, {{9, 6}, {24, 2}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 38, .name = "zs", .display = NULL, .bits = {4, {{16, 6}, {26, 2}, {29, 1}, {30, 1}}}},
};
static const struct agx_field ins_iter_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "diter", .display = NULL, .bits = {3, {{8, 7}, {56, 2}, {30, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 21, .name = "cf", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {23, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 23, .name = "itersample", .display = NULL, .bits = {2, {{32, 8}, {48, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "fwd", .display = "fwd", .bits = {1, {{22, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "elide", .display = "elide", .bits = {1, {{46, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 1, .name = "interpolation", .display = "interpolation", .bits = {1, {{48, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "kill", .display = "kill", .bits = {1, {{52, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_iterproj_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 0, .name = "diter", .display = NULL, .bits = {3, {{8, 7}, {56, 2}, {30, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 21, .name = "cf", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {23, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 22, .name = "cfperspective", .display = NULL, .bits = {2, {{24, 6}, {60, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 23, .name = "itersample", .display = NULL, .bits = {2, {{32, 8}, {48, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "fwd", .display = "fwd", .bits = {1, {{22, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "elide", .display = "elide", .bits = {1, {{46, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 1, .name = "interpolation", .display = "interpolation", .bits = {1, {{48, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "kill", .display = "kill", .bits = {1, {{52, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_ldcf_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 1, .name = "dldcf", .display = NULL, .bits = {3, {{8, 7}, {56, 2}, {30, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 21, .name = "cf", .display = NULL, .bits = {3, {{16, 6}, {58, 2}, {23, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 23, .name = "itersample", .display = NULL, .bits = {2, {{32, 8}, {48, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_smpf_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {72, 2}, {8, 1}, {48, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 39, .name = "heap", .display = NULL, .bits = {2, {{64, 5}, {39, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 40, .name = "texture", .display = NULL, .bits = {3, {{32, 6}, {78, 2}, {38, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 41, .name = "sampler", .display = NULL, .bits = {3, {{56, 6}, {92, 2}, {62, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "coord", .display = NULL, .bits = {4, {{16, 6}, {74, 2}, {22, 1}, {47, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "lod", .display = NULL, .bits = {3, {{24, 6}, {76, 2}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 38, .name = "zs", .display = NULL, .bits = {4, {{80, 6}, {94, 2}, {23, 1}, {91, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "shadow", .display = "shadow", .bits = {1, {{23, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "query_lod", .display = "query_lod", .bits = {1, {{31, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "rw", .display = "rw", .bits = {1, {{43, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "coherent", .display = "coherent", .bits = {1, {{44, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sparse", .display = "sparse", .bits = {1, {{70, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "kill", .display = "kill", .bits = {1, {{69, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 13, .name = "dim", .display = "dim", .bits = {2, {{40, 3}, {71, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{48, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 15, .name = "lod_type", .display = "lod_type", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "gather", .display = "gather", .bits = {1, {{87, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "group", .display = "group", .bits = {1, {{63, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_smpi_fields[] = {
	{.type = AGX_FT_DEST, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 19, .name = "memory", .display = NULL, .bits = {4, {{9, 6}, {72, 2}, {8, 1}, {48, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 39, .name = "heap", .display = NULL, .bits = {2, {{64, 5}, {39, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 40, .name = "texture", .display = NULL, .bits = {3, {{32, 6}, {78, 2}, {38, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 41, .name = "sampler", .display = NULL, .bits = {3, {{56, 6}, {92, 2}, {62, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "coord", .display = NULL, .bits = {4, {{16, 6}, {74, 2}, {22, 1}, {47, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 10, .name = "lod", .display = NULL, .bits = {3, {{24, 6}, {76, 2}, {52, 4}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 38, .name = "zs", .display = NULL, .bits = {4, {{80, 6}, {94, 2}, {23, 1}, {91, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "shadow", .display = "shadow", .bits = {1, {{23, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "query_lod", .display = "query_lod", .bits = {1, {{31, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "rw", .display = "rw", .bits = {1, {{43, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "coherent", .display = "coherent", .bits = {1, {{44, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "sparse", .display = "sparse", .bits = {1, {{70, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "kill", .display = "kill", .bits = {1, {{69, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 13, .name = "dim", .display = "dim", .bits = {2, {{40, 3}, {71, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 11, .name = "mask", .display = "mask", .bits = {1, {{48, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 15, .name = "lod_type", .display = "lod_type", .bits = {1, {{52, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 14, .name = "gather", .display = "gather", .bits = {1, {{87, 4}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 6, .name = "group", .display = "group", .bits = {1, {{63, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_imgw_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 24, .name = "pbesource", .display = NULL, .bits = {3, {{9, 6}, {56, 2}, {8, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 39, .name = "heap", .display = NULL, .bits = {2, {{48, 5}, {39, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 40, .name = "texture", .display = NULL, .bits = {3, {{32, 6}, {62, 2}, {38, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "coord", .display = NULL, .bits = {4, {{16, 6}, {58, 2}, {22, 1}, {47, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 20, .name = "uvs", .display = NULL, .bits = {3, {{24, 6}, {60, 2}, {31, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 13, .name = "dim", .display = "dim", .bits = {2, {{40, 3}, {55, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "rtz", .display = "rtz", .bits = {1, {{53, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 2, .name = "coherency", .display = "coherency", .bits = {1, {{44, 2}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
static const struct agx_field ins_imgwblk_fields[] = {
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 13, .name = "exreg16", .display = NULL, .bits = {2, {{9, 6}, {56, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 39, .name = "heap", .display = NULL, .bits = {2, {{48, 5}, {39, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 40, .name = "texture", .display = NULL, .bits = {3, {{32, 6}, {62, 2}, {38, 2}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 9, .name = "coord", .display = NULL, .bits = {4, {{16, 6}, {58, 2}, {22, 1}, {47, 1}}}},
	{.type = AGX_FT_SRC, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 20, .name = "uvs", .display = NULL, .bits = {3, {{24, 6}, {60, 2}, {31, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "explicit_coords", .display = "explicit_coords", .bits = {1, {{23, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 13, .name = "dim", .display = "dim", .bits = {2, {{40, 3}, {55, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = 12, .name = "format", .display = "format", .bits = {2, {{8, 1}, {64, 3}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "rtz", .display = "rtz", .bits = {1, {{53, 1}}}},
	{.type = AGX_FT_MOD, .imm_kind = AGX_IMM_INT, .shift = 0, .ref = -1, .name = "length", .display = "length", .bits = {1, {{15, 1}}}},
};
const struct agx_ins_desc agx_ins_table[] = {
	{.name = "ldimm16", .display = "ldimm", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x17full, 0x0ull}, .exact_value = {0x62ull, 0x0ull},
	 .mask = {0x3000ffffffffull, 0x0ull}, .nfields = 3, .fields = ins_ldimm16_fields, .aienos = 0},
	{.name = "ldimm32", .display = "ldimm", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x17full, 0x0ull}, .exact_value = {0x162ull, 0x0ull},
	 .mask = {0x3000ffffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_ldimm32_fields, .aienos = 0},
	{.name = "mfsr", .display = "mov", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0x807full, 0x0ull}, .exact_value = {0x72ull, 0x0ull},
	 .mask = {0x3c3fffffull, 0x0ull}, .nfields = 2, .fields = ins_mfsr_fields, .aienos = 0},
	{.name = "iadd", .display = "iadd", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x800803full, 0x0ull}, .exact_value = {0xeull, 0x0ull},
	 .mask = {0x303fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_iadd_fields, .aienos = 0},
	{.name = "isub", .display = "isub", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x800803full, 0x0ull}, .exact_value = {0x800000eull, 0x0ull},
	 .mask = {0x303fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_isub_fields, .aienos = 0},
	{.name = "imul", .display = "imul", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xf7ff800800803full, 0x0ull}, .exact_value = {0x1eull, 0x0ull},
	 .mask = {0x3ff7ffffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_imul_fields, .aienos = 0},
	{.name = "imadd", .display = "imadd", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x800803full, 0x0ull}, .exact_value = {0x1eull, 0x0ull},
	 .mask = {0x3ff7ffffffffffffull, 0x0ull}, .nfields = 6, .fields = ins_imadd_fields, .aienos = 0},
	{.name = "imsub", .display = "imsub", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x800803full, 0x0ull}, .exact_value = {0x800001eull, 0x0ull},
	 .mask = {0x3ff7ffffffffffffull, 0x0ull}, .nfields = 6, .fields = ins_imsub_fields, .aienos = 0},
	{.name = "u8_to_f", .display = "u8_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_u8_to_f_fields, .aienos = 0},
	{.name = "s8_to_f", .display = "s8_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x1803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_s8_to_f_fields, .aienos = 0},
	{.name = "f_to_u16", .display = "f_to_u16", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x4803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_f_to_u16_fields, .aienos = 0},
	{.name = "f_to_s16", .display = "f_to_s16", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x5803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_f_to_s16_fields, .aienos = 0},
	{.name = "u16_to_f", .display = "u16_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x6803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_u16_to_f_fields, .aienos = 0},
	{.name = "s16_to_f", .display = "s16_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x7803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_s16_to_f_fields, .aienos = 0},
	{.name = "f_to_u32", .display = "f_to_u32", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x8803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_f_to_u32_fields, .aienos = 0},
	{.name = "f_to_s32", .display = "f_to_s32", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0x9803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_f_to_s32_fields, .aienos = 0},
	{.name = "u32_to_f", .display = "u32_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0xa803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_u32_to_f_fields, .aienos = 0},
	{.name = "s32_to_f", .display = "s32_to_f", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xcc003ff807full, 0x0ull}, .exact_value = {0xb803eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_s32_to_f_fields, .aienos = 0},
	{.name = "shl", .display = "shl", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x8c0c00c00fff807full, 0x0ull}, .exact_value = {0x2eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_shl_fields, .aienos = 0},
	{.name = "shr", .display = "shr", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x8c0c00c00fff807full, 0x0ull}, .exact_value = {0x802eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_shr_fields, .aienos = 0},
	{.name = "bfi", .display = "bfi", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x2eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_bfi_fields, .aienos = 0},
	{.name = "bfeil", .display = "bfeil", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x802eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_bfeil_fields, .aienos = 0},
	{.name = "extr", .display = "extr", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x400002eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_extr_fields, .aienos = 0},
	{.name = "shlhi", .display = "shlhi", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x800002eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_shlhi_fields, .aienos = 0},
	{.name = "shrhi", .display = "shrhi", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x800802eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_shrhi_fields, .aienos = 0},
	{.name = "asr", .display = "asr", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0x400802eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_asr_fields, .aienos = 0},
	{.name = "asrh", .display = "asrh", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xc00807full, 0x0ull}, .exact_value = {0xc00802eull, 0x0ull},
	 .mask = {0xbfcfffffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_asrh_fields, .aienos = 0},
	{.name = "ffma", .display = "ffma", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x3aull, 0x0ull},
	 .mask = {0x3fcfffffffffffffull, 0x0ull}, .nfields = 6, .fields = ins_ffma_fields, .aienos = 0},
	{.name = "hfma", .display = "hfma", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x36ull, 0x0ull},
	 .mask = {0x3fc7ff7ff7ffffffull, 0x0ull}, .nfields = 6, .fields = ins_hfma_fields, .aienos = 0},
	{.name = "fadd", .display = "fadd", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x2aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_fadd_fields, .aienos = 0},
	{.name = "fmul", .display = "fmul", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x1aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_fmul_fields, .aienos = 0},
	{.name = "hadd", .display = "hadd", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x26ull, 0x0ull},
	 .mask = {0x3f7ff7ffffffull, 0x0ull}, .nfields = 5, .fields = ins_hadd_fields, .aienos = 0},
	{.name = "hmul", .display = "hmul", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3full, 0x0ull}, .exact_value = {0x16ull, 0x0ull},
	 .mask = {0x3f7ff7ffffffull, 0x0ull}, .nfields = 5, .fields = ins_hmul_fields, .aienos = 0},
	{.name = "not_a", .display = "not_a", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x3fffc00807full, 0x0ull}, .exact_value = {0xc00000007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_not_a_fields, .aienos = 0},
	{.name = "mov", .display = "mov", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x3fffc00807full, 0x0ull}, .exact_value = {0x800800007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_mov_fields, .aienos = 0},
	{.name = "not", .display = "not", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x3fffc00807full, 0x0ull}, .exact_value = {0x400400007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_not_fields, .aienos = 0},
	{.name = "mov_a", .display = "mov_a", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x3fffc00807full, 0x0ull}, .exact_value = {0xc00007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_mov_a_fields, .aienos = 0},
	{.name = "zero", .display = "zero", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x7eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_zero_fields, .aienos = 0},
	{.name = "and", .display = "and", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x800000007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_and_fields, .aienos = 0},
	{.name = "andn1", .display = "andn1", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x400000007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_andn1_fields, .aienos = 0},
	{.name = "andn2", .display = "andn2", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x800007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_andn2_fields, .aienos = 0},
	{.name = "xor", .display = "xor", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x400800007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_xor_fields, .aienos = 0},
	{.name = "or", .display = "or", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0xc00800007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_or_fields, .aienos = 0},
	{.name = "nor", .display = "nor", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x400007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_nor_fields, .aienos = 0},
	{.name = "xnor", .display = "xnor", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x800400007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_xnor_fields, .aienos = 0},
	{.name = "orn1", .display = "orn1", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0xc00400007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_orn1_fields, .aienos = 0},
	{.name = "orn2", .display = "orn2", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x800c00007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_orn2_fields, .aienos = 0},
	{.name = "nand", .display = "nand", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x400c00007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_nand_fields, .aienos = 0},
	{.name = "one", .display = "one", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0xc00c00007eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_one_fields, .aienos = 0},
	{.name = "bitrev", .display = "bitrev", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xfffc00807full, 0x0ull}, .exact_value = {0x400003eull, 0x0ull},
	 .mask = {0x3cffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_bitrev_fields, .aienos = 0},
	{.name = "popcnt", .display = "popcnt", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xfffc00807full, 0x0ull}, .exact_value = {0x800003eull, 0x0ull},
	 .mask = {0x3cffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_popcnt_fields, .aienos = 0},
	{.name = "ffs", .display = "ffs", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xfffc00807full, 0x0ull}, .exact_value = {0xc00003eull, 0x0ull},
	 .mask = {0x3cffffffffffull, 0x0ull}, .nfields = 2, .fields = ins_ffs_fields, .aienos = 0},
	{.name = "intl", .display = "intl", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xc00c00807full, 0x0ull}, .exact_value = {0x3eull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_intl_fields, .aienos = 0},
	{.name = "floor", .display = "floor", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0xaull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_floor_fields, .aienos = 0},
	{.name = "rsqrts", .display = "rsqrts", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x1000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_rsqrts_fields, .aienos = 0},
	{.name = "rcp", .display = "rcp", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x8000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_rcp_fields, .aienos = 0},
	{.name = "rsqrt", .display = "rsqrt", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x9000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_rsqrt_fields, .aienos = 0},
	{.name = "sinr", .display = "sinr", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0xa000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_sinr_fields, .aienos = 0},
	{.name = "dfdx", .display = "dfdx", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x4000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_dfdx_fields, .aienos = 0},
	{.name = "dfdy", .display = "dfdy", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x6000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_dfdy_fields, .aienos = 0},
	{.name = "sinc", .display = "sinc", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0xe000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_sinc_fields, .aienos = 0},
	{.name = "log2", .display = "log2", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0xc000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_log2_fields, .aienos = 0},
	{.name = "exp2", .display = "exp2", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0xd000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_exp2_fields, .aienos = 0},
	{.name = "ceil", .display = "ceil", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x10000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_ceil_fields, .aienos = 0},
	{.name = "trunc", .display = "trunc", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x20000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_trunc_fields, .aienos = 0},
	{.name = "rint", .display = "rint", .nlen = 2, .len = {4, 6}, .length_bit = 15,
	 .exact_mask = {0x3fff000003full, 0x0ull}, .exact_value = {0x30000000aull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_rint_fields, .aienos = 0},
	{.name = "stop", .display = "stop", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x88ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_stop_fields, .aienos = 0},
	{.name = "trap", .display = "trap", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x8ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_trap_fields, .aienos = 0},
	{.name = "jmp_incomplete", .display = "jmp_incomplete", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0xff00ffffull, 0x0ull}, .exact_value = {0x0ull, 0x0ull},
	 .mask = {0xffffffffull, 0x0ull}, .nfields = 1, .fields = ins_jmp_incomplete_fields, .aienos = 0},
	{.name = "ret", .display = "ret", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x14ull, 0x0ull},
	 .mask = {0xfe7full, 0x0ull}, .nfields = 1, .fields = ins_ret_fields, .aienos = 0},
	{.name = "call_reg", .display = "call", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x4ull, 0x0ull},
	 .mask = {0xfe7full, 0x0ull}, .nfields = 1, .fields = ins_call_reg_fields, .aienos = 0},
	{.name = "jmp_any", .display = "jmp_any", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xc000ull, 0x0ull},
	 .mask = {0xffffffffffffull, 0x0ull}, .nfields = 1, .fields = ins_jmp_any_fields, .aienos = 0},
	{.name = "call", .display = "call", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xc010ull, 0x0ull},
	 .mask = {0xffffffffffffull, 0x0ull}, .nfields = 1, .fields = ins_call_fields, .aienos = 0},
	{.name = "jmp_none", .display = "jmp_none", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xc020ull, 0x0ull},
	 .mask = {0xffffffffffffull, 0x0ull}, .nfields = 1, .fields = ins_jmp_none_fields, .aienos = 0},
	{.name = "pop_exec", .display = "pop_exec", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0xffffffffe67full, 0x0ull}, .exact_value = {0x652ull, 0x0ull},
	 .mask = {0xfffffffffeffull, 0x0ull}, .nfields = 2, .fields = ins_pop_exec_fields, .aienos = 0},
	{.name = "if_icmp", .display = "if", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30c00c00067full, 0x0ull}, .exact_value = {0x52ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_if_icmp_fields, .aienos = 0},
	{.name = "else_icmp", .display = "else", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30c00c00067full, 0x0ull}, .exact_value = {0x252ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_else_icmp_fields, .aienos = 0},
	{.name = "while_icmp", .display = "while", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30c00c00067full, 0x0ull}, .exact_value = {0x452ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_while_icmp_fields, .aienos = 0},
	{.name = "update_exec", .display = "update_exec", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x3fffffffff7full, 0x0ull}, .exact_value = {0x42ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 1, .fields = ins_update_exec_fields, .aienos = 0},
	{.name = "if_fcmp", .display = "if", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30000000067full, 0x0ull}, .exact_value = {0x42ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_if_fcmp_fields, .aienos = 0},
	{.name = "else_fcmp", .display = "else", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30000000067full, 0x0ull}, .exact_value = {0x242ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_else_fcmp_fields, .aienos = 0},
	{.name = "while_fcmp", .display = "while", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x30000000067full, 0x0ull}, .exact_value = {0x442ull, 0x0ull},
	 .mask = {0x3fffffffffffull, 0x0ull}, .nfields = 5, .fields = ins_while_fcmp_fields, .aienos = 0},
	{.name = "icsel", .display = "csel", .nlen = 2, .len = {8, 10}, .length_bit = 15,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x12ull, 0x0ull},
	 .mask = {0xfff1ff3ff3ffffffull, 0x3ff0ull}, .nfields = 7, .fields = ins_icsel_fields, .aienos = 0},
	{.name = "fcsel", .display = "csel", .nlen = 2, .len = {8, 10}, .length_bit = 15,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x2ull, 0x0ull},
	 .mask = {0xfff1ffffffffffffull, 0x3ff0ull}, .nfields = 7, .fields = ins_fcsel_fields, .aienos = 0},
	{.name = "ballot", .display = "ballot", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xffff803ff3ff007full, 0x0ull}, .exact_value = {0x1800000010032ull, 0x0ull},
	 .mask = {0xffffb03ff3ff7fffull, 0x0ull}, .nfields = 1, .fields = ins_ballot_fields, .aienos = 0},
	{.name = "icmp_ballot", .display = "ballot", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x1ffe00000000007full, 0x0ull}, .exact_value = {0x32ull, 0x0ull},
	 .mask = {0xffffbf3ff3ff7fffull, 0x0ull}, .nfields = 5, .fields = ins_icmp_ballot_fields, .aienos = 0},
	{.name = "fcmp_ballot", .display = "ballot", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x1ffe00000000007full, 0x0ull}, .exact_value = {0x22ull, 0x0ull},
	 .mask = {0xffffbf3ff3ff7fffull, 0x0ull}, .nfields = 5, .fields = ins_fcmp_ballot_fields, .aienos = 0},
	{.name = "shuffle", .display = "shuffle", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x800807full, 0x0ull}, .exact_value = {0x6full, 0x0ull},
	 .mask = {0xbfffffffffffull, 0x0ull}, .nfields = 6, .fields = ins_shuffle_fields, .aienos = 0},
	{.name = "pixwait", .display = "pixwait", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x48ull, 0x0ull},
	 .mask = {0xc3ffffull, 0x0ull}, .nfields = 2, .fields = ins_pixwait_fields, .aienos = 0},
	{.name = "pixrel", .display = "pixrel", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x58ull, 0x0ull},
	 .mask = {0xc3ffffull, 0x0ull}, .nfields = 2, .fields = ins_pixrel_fields, .aienos = 0},
	{.name = "wait", .display = "wait", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x38ull, 0x0ull},
	 .mask = {0x1ffull, 0x0ull}, .nfields = 1, .fields = ins_wait_fields, .aienos = 0},
	{.name = "no_var", .display = "no_var", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0x804003ffull, 0x0ull}, .exact_value = {0x80000051ull, 0x0ull},
	 .mask = {0x804003ffull, 0x0ull}, .nfields = 0, .fields = ins_no_var_fields, .aienos = 0},
	{.name = "st_var", .display = "st_var", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0x8040037full, 0x0ull}, .exact_value = {0x80000011ull, 0x0ull},
	 .mask = {0x8fff7fffull, 0x0ull}, .nfields = 3, .fields = ins_st_var_fields, .aienos = 0},
	{.name = "load", .display = "load", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0x40000000007full, 0x0ull}, .exact_value = {0x400000000005ull, 0x0ull},
	 .mask = {0xfff3ffff4fffffffull, 0x0ull}, .nfields = 10, .fields = ins_load_fields, .aienos = 1},
	{.name = "atomic", .display = "atomic", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x40008400003full, 0x0ull}, .exact_value = {0x400084000015ull, 0x0ull},
	 .mask = {0xfffff3ffcfffffffull, 0x0ull}, .nfields = 7, .fields = ins_atomic_fields, .aienos = 0},
	{.name = "latomic", .display = "latomic", .nlen = 2, .len = {6, 10}, .length_bit = 15,
	 .exact_mask = {0x80100000003full, 0x0ull}, .exact_value = {0x801000000019ull, 0x0ull},
	 .mask = {0x3fff8057ffffff3full, 0xffull}, .nfields = 6, .fields = ins_latomic_fields, .aienos = 0},
	{.name = "stack_load", .display = "stack_load", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0x7070040000ffull, 0x0ull}, .exact_value = {0x400004000035ull, 0x0ull},
	 .mask = {0xfffeff7f47f0ffffull, 0x0ull}, .nfields = 7, .fields = ins_stack_load_fields, .aienos = 0},
	{.name = "stack_adjust", .display = "stack_adjust", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0x7070040100ffull, 0x0ull}, .exact_value = {0x20040100b5ull, 0x0ull},
	 .mask = {0xff00f07f07f100ffull, 0x0ull}, .nfields = 2, .fields = ins_stack_adjust_fields, .aienos = 0},
	{.name = "stack_store", .display = "stack_store", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0x7070040000ffull, 0x0ull}, .exact_value = {0x4000040000b5ull, 0x0ull},
	 .mask = {0xfffeff7f47f0ffffull, 0x0ull}, .nfields = 7, .fields = ins_stack_store_fields, .aienos = 0},
	{.name = "uniform_store", .display = "mov", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0xd0cf07f0f03ffull, 0x0ull}, .exact_value = {0x3d0000c5ull, 0x0ull},
	 .mask = {0xfffd8fff7fffffffull, 0x0ull}, .nfields = 3, .fields = ins_uniform_store_fields, .aienos = 0},
	{.name = "uniform_store_unk", .display = "uniform_store_unk", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0xc00f03c0f007full, 0x0ull}, .exact_value = {0x3c000045ull, 0x0ull},
	 .mask = {0xffff8fff7fffffffull, 0x0ull}, .nfields = 7, .fields = ins_uniform_store_unk_fields, .aienos = 0},
	{.name = "tex_state_store", .display = "load", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x1000ffull, 0x0ull}, .exact_value = {0x1000edull, 0x0ull},
	 .mask = {0xfdfffffffc117fffull, 0x0ull}, .nfields = 4, .fields = ins_tex_state_store_fields, .aienos = 0},
	{.name = "sampler_state_store", .display = "load", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0x1000ffull, 0x0ull}, .exact_value = {0x1000adull, 0x0ull},
	 .mask = {0xfdfffffffc117fffull, 0x0ull}, .nfields = 4, .fields = ins_sampler_state_store_fields, .aienos = 0},
	{.name = "store", .display = "store", .nlen = 2, .len = {6, 8}, .length_bit = 47,
	 .exact_mask = {0x40000000007full, 0x0ull}, .exact_value = {0x400000000045ull, 0x0ull},
	 .mask = {0xfff3ffff4fffffffull, 0x0ull}, .nfields = 10, .fields = ins_store_fields, .aienos = 1},
	{.name = "lload", .display = "lload", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x69ull, 0x0ull},
	 .mask = {0x3fff87f7ffffff7full, 0x0ull}, .nfields = 8, .fields = ins_lload_fields, .aienos = 1},
	{.name = "lstore", .display = "lstore", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x29ull, 0x0ull},
	 .mask = {0x3fff87f7ffffff7full, 0x0ull}, .nfields = 8, .fields = ins_lstore_fields, .aienos = 1},
	{.name = "barrier", .display = "barrier", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x68ull, 0x0ull},
	 .mask = {0xffull, 0x0ull}, .nfields = 0, .fields = ins_barrier_fields, .aienos = 0},
	{.name = "doorbell", .display = "doorbell", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x28ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 1, .fields = ins_doorbell_fields, .aienos = 0},
	{.name = "jmp_if_skipping_doorbell", .display = "jmp_if_skipping_doorbell", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0xff00ffffull, 0x0ull}, .exact_value = {0x20ull, 0x0ull},
	 .mask = {0xffffffffull, 0x0ull}, .nfields = 1, .fields = ins_jmp_if_skipping_doorbell_fields, .aienos = 0},
	{.name = "unmap", .display = "unmap", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xfffce0ff0f07ffull, 0x0ull}, .exact_value = {0x10c00005000075ull, 0x0ull},
	 .mask = {0xffffffffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_unmap_fields, .aienos = 0},
	{.name = "map", .display = "map", .nlen = 1, .len = {8, 0}, .length_bit = -1,
	 .exact_mask = {0xfffce0ff0f07ffull, 0x0ull}, .exact_value = {0x10c00005010075ull, 0x0ull},
	 .mask = {0xffffffffffffffffull, 0x0ull}, .nfields = 3, .fields = ins_map_fields, .aienos = 0},
	{.name = "memory_barrier", .display = "memory_barrier", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x96f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_memory_barrier_fields, .aienos = 0},
	{.name = "image_barrier_1", .display = "image_barrier_1", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xaaf5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_image_barrier_1_fields, .aienos = 0},
	{.name = "image_barrier_2", .display = "image_barrier_2", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xaef5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_image_barrier_2_fields, .aienos = 0},
	{.name = "image_barrier_3", .display = "image_barrier_3", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xa9f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_image_barrier_3_fields, .aienos = 0},
	{.name = "image_barrier_4", .display = "image_barrier_4", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0xadf5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_image_barrier_4_fields, .aienos = 0},
	{.name = "flush_memory_to_texture", .display = "flush_memory_to_texture", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x40f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_flush_memory_to_texture_fields, .aienos = 0},
	{.name = "memory_barrier_2", .display = "memory_barrier_2", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x9af5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_memory_barrier_2_fields, .aienos = 0},
	{.name = "memory_barrier_3", .display = "memory_barrier_3", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x99f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_memory_barrier_3_fields, .aienos = 0},
	{.name = "unknown_barrier_1", .display = "unknown_barrier_1", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x33f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_unknown_barrier_1_fields, .aienos = 0},
	{.name = "unknown_barrier_2", .display = "unknown_barrier_2", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x3f5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_unknown_barrier_2_fields, .aienos = 0},
	{.name = "device_barrier_1", .display = "device_barrier_1", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x9df5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_device_barrier_1_fields, .aienos = 0},
	{.name = "device_barrier_2", .display = "device_barrier_2", .nlen = 1, .len = {2, 0}, .length_bit = -1,
	 .exact_mask = {0xffffull, 0x0ull}, .exact_value = {0x9ef5ull, 0x0ull},
	 .mask = {0xffffull, 0x0ull}, .nfields = 0, .fields = ins_device_barrier_2_fields, .aienos = 0},
	{.name = "quad", .display = "quad", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x23fac00807full, 0x0ull}, .exact_value = {0x806full, 0x0ull},
	 .mask = {0xbfffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_quad_fields, .aienos = 0},
	{.name = "simd", .display = "simd", .nlen = 1, .len = {6, 0}, .length_bit = -1,
	 .exact_mask = {0x23fac00807full, 0x0ull}, .exact_value = {0x2000806full, 0x0ull},
	 .mask = {0xbfffffffffffull, 0x0ull}, .nfields = 4, .fields = ins_simd_fields, .aienos = 0},
	{.name = "ld_tile", .display = "ld_tile", .nlen = 2, .len = {8, 10}, .length_bit = 15,
	 .exact_mask = {0x80000007full, 0x0ull}, .exact_value = {0x800000049ull, 0x0ull},
	 .mask = {0x3f80ffffff7fff7full, 0x0ull}, .nfields = 7, .fields = ins_ld_tile_fields, .aienos = 0},
	{.name = "st_tile", .display = "st_tile", .nlen = 2, .len = {8, 10}, .length_bit = 15,
	 .exact_mask = {0x7full, 0x0ull}, .exact_value = {0x9ull, 0x0ull},
	 .mask = {0x3f80ffffff7fff7full, 0x0ull}, .nfields = 8, .fields = ins_st_tile_fields, .aienos = 0},
	{.name = "sample_mask", .display = "sample_mask", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0x80ffull, 0x0ull}, .exact_value = {0xc1ull, 0x0ull},
	 .mask = {0xfbfffffull, 0x0ull}, .nfields = 2, .fields = ins_sample_mask_fields, .aienos = 0},
	{.name = "zs_emit", .display = "zs_emit", .nlen = 1, .len = {4, 0}, .length_bit = -1,
	 .exact_mask = {0xc080ffull, 0x0ull}, .exact_value = {0x41ull, 0x0ull},
	 .mask = {0x6fffffffull, 0x0ull}, .nfields = 2, .fields = ins_zs_emit_fields, .aienos = 0},
	{.name = "iter", .display = "iter", .nlen = 2, .len = {4, 8}, .length_bit = 15,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x21ull, 0x0ull},
	 .mask = {0xf1340ffc0ffffffull, 0x0ull}, .nfields = 8, .fields = ins_iter_fields, .aienos = 0},
	{.name = "iterproj", .display = "iter", .nlen = 2, .len = {4, 8}, .length_bit = 15,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0x61ull, 0x0ull},
	 .mask = {0x3f1340ffffffffffull, 0x0ull}, .nfields = 9, .fields = ins_iterproj_fields, .aienos = 0},
	{.name = "ldcf", .display = "ldcf", .nlen = 2, .len = {4, 8}, .length_bit = 15,
	 .exact_mask = {0xffull, 0x0ull}, .exact_value = {0xa1ull, 0x0ull},
	 .mask = {0xf0300ffc0bfffffull, 0x0ull}, .nfields = 4, .fields = ins_ldcf_fields, .aienos = 0},
	{.name = "smpf", .display = "smpf", .nlen = 2, .len = {8, 12}, .length_bit = 15,
	 .exact_mask = {0x6000000000ffull, 0x0ull}, .exact_value = {0x600000000031ull, 0x0ull},
	 .mask = {0xffffffffbfffffffull, 0xffbfffffull}, .nfields = 19, .fields = ins_smpf_fields, .aienos = 0},
	{.name = "smpi", .display = "smpi", .nlen = 2, .len = {8, 12}, .length_bit = 15,
	 .exact_mask = {0x6000000000ffull, 0x0ull}, .exact_value = {0x600000000071ull, 0x0ull},
	 .mask = {0xffffffffbfffffffull, 0xffbfffffull}, .nfields = 19, .fields = ins_smpi_fields, .aienos = 0},
	{.name = "imgw", .display = "imgw", .nlen = 2, .len = {6, 8}, .length_bit = 15,
	 .exact_mask = {0x4800008000ffull, 0x0ull}, .exact_value = {0x4800008000f1ull, 0x0ull},
	 .mask = {0xffbfffffbfffffffull, 0x0ull}, .nfields = 9, .fields = ins_imgw_fields, .aienos = 0},
	{.name = "imgwblk", .display = "imgwblk", .nlen = 2, .len = {6, 10}, .length_bit = 15,
	 .exact_mask = {0x4800000000ffull, 0x3ff8ull}, .exact_value = {0x4800000000b1ull, 0x8ull},
	 .mask = {0xffbfcfffbfffffffull, 0x3fffull}, .nfields = 10, .fields = ins_imgwblk_fields, .aienos = 0},
};
const unsigned agx_ins_count = 136;
