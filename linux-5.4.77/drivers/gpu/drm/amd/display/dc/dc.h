/*
 * Copyright 2012-14 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef DC_INTERFACE_H_
#define DC_INTERFACE_H_

#include "dc_types.h"
#include "grph_object_defs.h"
#include "logger_types.h"
#include "gpio_types.h"
#include "link_service_types.h"
#include "grph_object_ctrl_defs.h"
#include <inc/hw/opp.h>

#include "inc/hw_sequencer.h"
#include "inc/compressor.h"
#include "inc/hw/dmcu.h"
#include "dml/display_mode_lib.h"

#define DC_VER "3.2.48"

#define MAX_SURFACES 3
#define MAX_PLANES 6
#define MAX_STREAMS 6
#define MAX_SINKS_PER_LINK 4

/*******************************************************************************
 * Display Core Interfaces
 ******************************************************************************/
struct dc_versions {
	const char *dc_ver;
	struct dmcu_version dmcu_version;
};

enum dc_plane_type {
	DC_PLANE_TYPE_INVALID,
	DC_PLANE_TYPE_DCE_RGB,
	DC_PLANE_TYPE_DCE_UNDERLAY,
	DC_PLANE_TYPE_DCN_UNIVERSAL,
};

struct dc_plane_cap {
	enum dc_plane_type type;
	uint32_t blends_with_above : 1;
	uint32_t blends_with_below : 1;
	uint32_t per_pixel_alpha : 1;
	struct {
		uint32_t argb8888 : 1;
		uint32_t nv12 : 1;
		uint32_t fp16 : 1;
		uint32_t p010 : 1;
		uint32_t ayuv : 1;
	} pixel_format_support;
	// max upscaling factor x1000
	// upscaling factors are always >= 1
	// for example, 1080p -> 8K is 4.0, or 4000 raw value
	struct {
		uint32_t argb8888;
		uint32_t nv12;
		uint32_t fp16;
	} max_upscale_factor;
	// max downscale factor x1000
	// downscale factors are always <= 1
	// for example, 8K -> 1080p is 0.25, or 250 raw value
	struct {
		uint32_t argb8888;
		uint32_t nv12;
		uint32_t fp16;
	} max_downscale_factor;
};

struct dc_caps {
	uint32_t max_streams;
	uint32_t max_links;
	uint32_t max_audios;
	uint32_t max_slave_planes;
	uint32_t max_planes;
	uint32_t max_downscale_ratio;
	uint32_t i2c_speed_in_khz;
	uint32_t dmdata_alloc_size;
	unsigned int max_cursor_size;
	unsigned int max_video_width;
	int linear_pitch_alignment;
	bool dcc_const_color;
	bool dynamic_audio;
	bool is_apu;
	bool dual_link_dvi;
	bool post_blend_color_processing;
	bool force_dp_tps4_for_cp2520;
	bool disable_dp_clk_share;
	bool psp_setup_panel_mode;
#ifdef CONFIG_DRM_AMD_DC_DCN2_0
	bool hw_3d_lut;
#endif
	struct dc_plane_cap planes[MAX_PLANES];
};

#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
struct dc_bug_wa {
	bool no_connect_phy_config;
	bool dedcn20_305_wa;
	bool skip_clock_update;
};
#endif

struct dc_dcc_surface_param {
	struct dc_size surface_size;
	enum surface_pixel_format format;
	enum swizzle_mode_values swizzle_mode;
	enum dc_scan_direction scan;
};

struct dc_dcc_setting {
	unsigned int max_compressed_blk_size;
	unsigned int max_uncompressed_blk_size;
	bool independent_64b_blks;
};

struct dc_surface_dcc_cap {
	union {
		struct {
			struct dc_dcc_setting rgb;
		} grph;

		struct {
			struct dc_dcc_setting luma;
			struct dc_dcc_setting chroma;
		} video;
	};

	bool capable;
	bool const_color_support;
};

struct dc_static_screen_events {
	bool force_trigger;
	bool cursor_update;
	bool surface_update;
	bool overlay_update;
};


/* Surface update type is used by dc_update_surfaces_and_stream
 * The update type is determined at the very beginning of the function based
 * on parameters passed in and decides how much programming (or updating) is
 * going to be done during the call.
 *
 * UPDATE_TYPE_FAST is used for really fast updates that do not require much
 * logical calculations or hardware register programming. This update MUST be
 * ISR safe on windows. Currently fast update will only be used to flip surface
 * address.
 *
 * UPDATE_TYPE_MED is used for slower updates which require significant hw
 * re-programming however do not affect bandwidth consumption or clock
 * requirements. At present, this is the level at which front end updates
 * that do not require us to run bw_calcs happen. These are in/out transfer func
 * updates, viewport offset changes, recout size changes and pixel depth changes.
 * This update can be done at ISR, but we want to minimize how often this happens.
 *
 * UPDATE_TYPE_FULL is slow. Really slow. This requires us to recalculate our
 * bandwidth and clocks, possibly rearrange some pipes and reprogram anything front
 * end related. Any time viewport dimensions, recout dimensions, scaling ratios or
 * gamma need to be adjusted or pipe needs to be turned on (or disconnected) we do
 * a full update. This cannot be done at ISR level and should be a rare event.
 * Unless someone is stress testing mpo enter/exit, playing with colour or adjusting
 * underscan we don't expect to see this call at all.
 */

enum surface_update_type {
	UPDATE_TYPE_FAST, /* super fast, safe to execute in isr */
	UPDATE_TYPE_MED,  /* ISR safe, most of programming needed, no bw/clk change*/
	UPDATE_TYPE_FULL, /* may need to shuffle resources */
};

/* Forward declaration*/
struct dc;
struct dc_plane_state;
struct dc_state;


struct dc_cap_funcs {
	bool (*get_dcc_compression_cap)(const struct dc *dc,
			const struct dc_dcc_surface_param *input,
			struct dc_surface_dcc_cap *output);
};

struct link_training_settings;


/* Structure to hold configuration flags set by dm at dc creation. */
struct dc_config {
	bool gpu_vm_support;
	bool disable_disp_pll_sharing;
	bool fbc_support;
	bool optimize_edp_link_rate;
	bool disable_fractional_pwm;
	bool allow_seamless_boot_optimization;
	bool power_down_display_on_boot;
	bool edp_not_connected;
	bool forced_clocks;
	bool multi_mon_pp_mclk_switch;
};

enum visual_confirm {
	VISUAL_CONFIRM_DISABLE = 0,
	VISUAL_CONFIRM_SURFACE = 1,
	VISUAL_CONFIRM_HDR = 2,
};

enum dcc_option {
	DCC_ENABLE = 0,
	DCC_DISABLE = 1,
	DCC_HALF_REQ_DISALBE = 2,
};

enum pipe_split_policy {
	MPC_SPLIT_DYNAMIC = 0,
	MPC_SPLIT_AVOID = 1,
	MPC_SPLIT_AVOID_MULT_DISP = 2,
};

enum wm_report_mode {
	WM_REPORT_DEFAULT = 0,
	WM_REPORT_OVERRIDE = 1,
};

/*
 * For any clocks that may differ per pipe
 * only the max is stored in this structure
 */
struct dc_clocks {
	int dispclk_khz;
	int max_supported_dppclk_khz;
	int max_supported_dispclk_khz;
	int dppclk_khz;
	int bw_dppclk_khz; /*a copy of dppclk_khz*/
	int bw_dispclk_khz;
	int dcfclk_khz;
	int socclk_khz;
	int dcfclk_deep_sleep_khz;
	int fclk_khz;
	int phyclk_khz;
	int dramclk_khz;
	bool p_state_change_support;

	/*
	 * Elements below are not compared for the purposes of
	 * optimization required
	 */
	bool prev_p_state_change_support;
};

struct dc_bw_validation_profile {
	bool enable;

	unsigned long long total_ticks;
	unsigned long long voltage_level_ticks;
	unsigned long long watermark_ticks;
	unsigned long long rq_dlg_ticks;

	unsigned long long total_count;
	unsigned long long skip_fast_count;
	unsigned long long skip_pass_count;
	unsigned long long skip_fail_count;
};

#define BW_VAL_TRACE_SETUP() \
		unsigned long long end_tick = 0; \
		unsigned long long voltage_level_tick = 0; \
		unsigned long long watermark_tick = 0; \
		unsigned long long start_tick = dc->debug.bw_val_profile.enable ? \
				dm_get_timestamp(dc->ctx) : 0

#define BW_VAL_TRACE_COUNT() \
		if (dc->debug.bw_val_profile.enable) \
			dc->debug.bw_val_profile.total_count++

#define BW_VAL_TRACE_SKIP(status) \
		if (dc->debug.bw_val_profile.enable) { \
			if (!voltage_level_tick) \
				voltage_level_tick = dm_get_timestamp(dc->ctx); \
			dc->debug.bw_val_profile.skip_ ## status ## _count++; \
		}

#define BW_VAL_TRACE_END_VOLTAGE_LEVEL() \
		if (dc->debug.bw_val_profile.enable) \
			voltage_level_tick = dm_get_timestamp(dc->ctx)

#define BW_VAL_TRACE_END_WATERMARKS() \
		if (dc->debug.bw_val_profile.enable) \
			watermark_tick = dm_get_timestamp(dc->ctx)

#define BW_VAL_TRACE_FINISH() \
		if (dc->debug.bw_val_profile.enable) { \
			end_tick = dm_get_timestamp(dc->ctx); \
			dc->debug.bw_val_profile.total_ticks += end_tick - start_tick; \
			dc->debug.bw_val_profile.voltage_level_ticks += voltage_level_tick - start_tick; \
			if (watermark_tick) { \
				dc->debug.bw_val_profile.watermark_ticks += watermark_tick - voltage_level_tick; \
				dc->debug.bw_val_profile.rq_dlg_ticks += end_tick - watermark_tick; \
			} \
		}

struct dc_debug_options {
	enum visual_confirm visual_confirm;
	bool sanity_checks;
	bool max_disp_clk;
	bool surface_trace;
	bool timing_trace;
	bool clock_trace;
	bool validation_trace;
	bool bandwidth_calcs_trace;
	int max_downscale_src_width;

	/* stutter efficiency related */
	bool disable_stutter;
	bool use_max_lb;
	enum dcc_option disable_dcc;
	enum pipe_split_policy pipe_split_policy;
	bool force_single_disp_pipe_split;
	bool voltage_align_fclk;

	bool disable_dfs_bypass;
	bool disable_dpp_power_gate;
	bool disable_hubp_power_gate;
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	bool disable_dsc_power_gate;
#endif
	bool disable_pplib_wm_range;
	enum wm_report_mode pplib_wm_report_mode;
	unsigned int min_disp_clk_khz;
	unsigned int min_dpp_clk_khz;
	int sr_exit_time_dpm0_ns;
	int sr_enter_plus_exit_time_dpm0_ns;
	int sr_exit_time_ns;
	int sr_enter_plus_exit_time_ns;
	int urgent_latency_ns;
	uint32_t underflow_assert_delay_us;
	int percent_of_ideal_drambw;
	int dram_clock_change_latency_ns;
	bool optimized_watermark;
	int always_scale;
	bool disable_pplib_clock_request;
	bool disable_clock_gate;
	bool disable_dmcu;
	bool disable_psr;
	bool force_abm_enable;
	bool disable_stereo_support;
	bool vsr_support;
	bool performance_trace;
	bool az_endpoint_mute_only;
	bool always_use_regamma;
	bool p010_mpo_support;
	bool recovery_enabled;
	bool avoid_vbios_exec_table;
	bool scl_reset_length10;
	bool hdmi20_disable;
	bool skip_detection_link_training;
	bool remove_disconnect_edp;
	unsigned int force_odm_combine; //bit vector based on otg inst
	unsigned int force_fclk_khz;
	bool disable_tri_buf;
	struct dc_bw_validation_profile bw_val_profile;
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	bool disable_fec;
#endif
#ifdef CONFIG_DRM_AMD_DC_DCN2_1
	bool disable_48mhz_pwrdwn;
#endif
	/* This forces a hard min on the DCFCLK requested to SMU/PP
	 * watermarks are not affected.
	 */
	unsigned int force_min_dcfclk_mhz;
	bool disable_timing_sync;
#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
	bool cm_in_bypass;
#endif
	int force_clock_mode;/*every mode change.*/
};

struct dc_debug_data {
	uint32_t ltFailCount;
	uint32_t i2cErrorCount;
	uint32_t auxErrorCount;
};

#ifdef CONFIG_DRM_AMD_DC_DCN2_0
struct dc_phy_addr_space_config {
	struct {
		uint64_t start_addr;
		uint64_t end_addr;
		uint64_t fb_top;
		uint64_t fb_offset;
		uint64_t fb_base;
		uint64_t agp_top;
		uint64_t agp_bot;
		uint64_t agp_base;
	} system_aperture;

	struct {
		uint64_t page_table_start_addr;
		uint64_t page_table_end_addr;
		uint64_t page_table_base_addr;
	} gart_config;

	bool valid;
	uint64_t page_table_default_page_addr;
};

struct dc_virtual_addr_space_config {
	uint64_t	page_table_base_addr;
	uint64_t	page_table_start_addr;
	uint64_t	page_table_end_addr;
	uint32_t	page_table_block_size_in_bytes;
	uint8_t		page_table_depth; // 1 = 1 level, 2 = 2 level, etc.  0 = invalid
};
#endif

struct dc_bounding_box_overrides {
	int sr_exit_time_ns;
	int sr_enter_plus_exit_time_ns;
	int urgent_latency_ns;
	int percent_of_ideal_drambw;
	int dram_clock_change_latency_ns;
	/* This forces a hard min on the DCFCLK we use
	 * for DML.  Unlike the debug option for forcing
	 * DCFCLK, this override affects watermark calculations
	 */
	int min_dcfclk_mhz;
};

struct dc_state;
struct resource_pool;
struct dce_hwseq;
struct gpu_info_soc_bounding_box_v1_0;
struct dc {
	struct dc_versions versions;
	struct dc_caps caps;
	struct dc_cap_funcs cap_funcs;
	struct dc_config config;
	struct dc_debug_options debug;
	struct dc_bounding_box_overrides bb_overrides;
#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
	struct dc_bug_wa work_arounds;
#endif
	struct dc_context *ctx;
#ifdef CONFIG_DRM_AMD_DC_DCN2_0
	struct dc_phy_addr_space_config vm_pa_config;
#endif

	uint8_t link_count;
	struct dc_link *links[MAX_PIPES * 2];

	struct dc_state *current_state;
	struct resource_pool *res_pool;

	struct clk_mgr *clk_mgr;

	/* Display Engine Clock levels */
	struct dm_pp_clock_levels sclk_lvls;

	/* Inputs into BW and WM calculations. */
	struct bw_calcs_dceip *bw_dceip;
	struct bw_calcs_vbios *bw_vbios;
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct dcn_soc_bounding_box *dcn_soc;
	struct dcn_ip_params *dcn_ip;
	struct display_mode_lib dml;
#endif

	/* HW functions */
	struct hw_sequencer_funcs hwss;
	struct dce_hwseq *hwseq;

	/* Require to optimize clocks and bandwidth for added/removed planes */
	bool optimized_required;

	/* Require to maintain clocks and bandwidth for UEFI enabled HW */
	bool optimize_seamless_boot;

	/* FBC compressor */
	struct compressor *fbc_compressor;

	struct dc_debug_data debug_data;

	const char *build_id;
#ifdef CONFIG_DRM_AMD_DC_DCN2_0
	struct vm_helper *vm_helper;
	const struct gpu_info_soc_bounding_box_v1_0 *soc_bounding_box;
#endif
};

enum frame_buffer_mode {
	FRAME_BUFFER_MODE_LOCAL_ONLY = 0,
	FRAME_BUFFER_MODE_ZFB_ONLY,
	FRAME_BUFFER_MODE_MIXED_ZFB_AND_LOCAL,
} ;

struct dchub_init_data {
	int64_t zfb_phys_addr_base;
	int64_t zfb_mc_base_addr;
	uint64_t zfb_size_in_byte;
	enum frame_buffer_mode fb_mode;
	bool dchub_initialzied;
	bool dchub_info_valid;
};

struct dc_init_data {
	struct hw_asic_id asic_id;
	void *driver; /* ctx */
	struct cgs_device *cgs_device;
	struct dc_bounding_box_overrides bb_overrides;

	int num_virtual_links;
	/*
	 * If 'vbios_override' not NULL, it will be called instead
	 * of the real VBIOS. Intended use is Diagnostics on FPGA.
	 */
	struct dc_bios *vbios_override;
	enum dce_environment dce_environment;

	struct dc_config flags;
	uint32_t log_mask;
#ifdef CONFIG_DRM_AMD_DC_DCN2_0
	/**
	 * gpu_info FW provided soc bounding box struct or 0 if not
	 * available in FW
	 */
	const struct gpu_info_soc_bounding_box_v1_0 *soc_bounding_box;
#endif
};

struct dc_callback_init {
	uint8_t reserved;
};

struct dc *dc_create(const struct dc_init_data *init_params);
int dc_get_vmid_use_vector(struct dc *dc);
#ifdef CONFIG_DRM_AMD_DC_DCN2_0
void dc_setup_vm_context(struct dc *dc, struct dc_virtual_addr_space_config *va_config, int vmid);
/* Returns the number of vmids supported */
int dc_setup_system_context(struct dc *dc, struct dc_phy_addr_space_config *pa_config);
#endif
void dc_init_callbacks(struct dc *dc,
		const struct dc_callback_init *init_params);
void dc_destroy(struct dc **dc);

/*******************************************************************************
 * Surface Interfaces
 ******************************************************************************/

enum {
	TRANSFER_FUNC_POINTS = 1025
};

struct dc_hdr_static_metadata {
	/* display chromaticities and white point in units of 0.00001 */
	unsigned int chromaticity_green_x;
	unsigned int chromaticity_green_y;
	unsigned int chromaticity_blue_x;
	unsigned int chromaticity_blue_y;
	unsigned int chromaticity_red_x;
	unsigned int chromaticity_red_y;
	unsigned int chromaticity_white_point_x;
	unsigned int chromaticity_white_point_y;

	uint32_t min_luminance;
	uint32_t max_luminance;
	uint32_t maximum_content_light_level;
	uint32_t maximum_frame_average_light_level;
};

enum dc_transfer_func_type {
	TF_TYPE_PREDEFINED,
	TF_TYPE_DISTRIBUTED_POINTS,
	TF_TYPE_BYPASS,
	TF_TYPE_HWPWL
};

struct dc_transfer_func_distributed_points {
	struct fixed31_32 red[TRANSFER_FUNC_POINTS];
	struct fixed31_32 green[TRANSFER_FUNC_POINTS];
	struct fixed31_32 blue[TRANSFER_FUNC_POINTS];

	uint16_t end_exponent;
	uint16_t x_point_at_y1_red;
	uint16_t x_point_at_y1_green;
	uint16_t x_point_at_y1_blue;
};

enum dc_transfer_func_predefined {
	TRANSFER_FUNCTION_SRGB,
	TRANSFER_FUNCTION_BT709,
	TRANSFER_FUNCTION_PQ,
	TRANSFER_FUNCTION_LINEAR,
	TRANSFER_FUNCTION_UNITY,
	TRANSFER_FUNCTION_HLG,
	TRANSFER_FUNCTION_HLG12,
	TRANSFER_FUNCTION_GAMMA22,
	TRANSFER_FUNCTION_GAMMA24,
	TRANSFER_FUNCTION_GAMMA26
};


struct dc_transfer_func {
	struct kref refcount;
	enum dc_transfer_func_type type;
	enum dc_transfer_func_predefined tf;
	/* FP16 1.0 reference level in nits, default is 80 nits, only for PQ*/
	uint32_t sdr_ref_white_level;
	struct dc_context *ctx;
	union {
		struct pwl_params pwl;
		struct dc_transfer_func_distributed_points tf_pts;
	};
};

#if defined(CONFIG_DRM_AMD_DC_DCN2_0)

union dc_3dlut_state {
	struct {
		uint32_t initialized:1;		/*if 3dlut is went through color module for initialization */
		uint32_t rmu_idx_valid:1;	/*if mux settings are valid*/
		uint32_t rmu_mux_num:3;		/*index of mux to use*/
		uint32_t mpc_rmu0_mux:4;	/*select mpcc on mux, one of the following : mpcc0, mpcc1, mpcc2, mpcc3*/
		uint32_t mpc_rmu1_mux:4;
		uint32_t mpc_rmu2_mux:4;
		uint32_t reserved:15;
	} bits;
	uint32_t raw;
};


struct dc_3dlut {
	struct kref refcount;
	struct tetrahedral_params lut_3d;
	uint32_t hdr_multiplier;
	bool initialized; /*remove after diag fix*/
	union dc_3dlut_state state;
	struct dc_context *ctx;
};
#endif
/*
 * This structure is filled in by dc_surface_get_status and contains
 * the last requested address and the currently active address so the called
 * can determine if there are any outstanding flips
 */
struct dc_plane_status {
	struct dc_plane_address requested_address;
	struct dc_plane_address current_address;
	bool is_flip_pending;
	bool is_right_eye;
};

union surface_update_flags {

	struct {
		uint32_t addr_update:1;
		/* Medium updates */
		uint32_t dcc_change:1;
		uint32_t color_space_change:1;
		uint32_t horizontal_mirror_change:1;
		uint32_t per_pixel_alpha_change:1;
		uint32_t global_alpha_change:1;
		uint32_t sdr_white_level:1;
		uint32_t rotation_change:1;
		uint32_t swizzle_change:1;
		uint32_t scaling_change:1;
		uint32_t position_change:1;
		uint32_t in_transfer_func_change:1;
		uint32_t input_csc_change:1;
		uint32_t coeff_reduction_change:1;
		uint32_t output_tf_change:1;
		uint32_t pixel_format_change:1;
		uint32_t plane_size_change:1;

		/* Full updates */
		uint32_t new_plane:1;
		uint32_t bpp_change:1;
		uint32_t gamma_change:1;
		uint32_t bandwidth_change:1;
		uint32_t clock_change:1;
		uint32_t stereo_format_change:1;
		uint32_t full_update:1;
	} bits;

	uint32_t raw;
};

struct dc_plane_state {
	struct dc_plane_address address;
	struct dc_plane_flip_time time;
#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
	bool triplebuffer_flips;
#endif
	struct scaling_taps scaling_quality;
	struct rect src_rect;
	struct rect dst_rect;
	struct rect clip_rect;

	struct plane_size plane_size;
	union dc_tiling_info tiling_info;

	struct dc_plane_dcc_param dcc;

	struct dc_gamma *gamma_correction;
	struct dc_transfer_func *in_transfer_func;
	struct dc_bias_and_scale *bias_and_scale;
	struct dc_csc_transform input_csc_color_matrix;
	struct fixed31_32 coeff_reduction_factor;
	uint32_t sdr_white_level;

	// TODO: No longer used, remove
	struct dc_hdr_static_metadata hdr_static_ctx;

	enum dc_color_space color_space;

#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
	struct dc_3dlut *lut3d_func;
	struct dc_transfer_func *in_shaper_func;
	struct dc_transfer_func *blend_tf;
#endif

	enum surface_pixel_format format;
	enum dc_rotation_angle rotation;
	enum plane_stereo_format stereo_format;

	bool is_tiling_rotated;
	bool per_pixel_alpha;
	bool global_alpha;
	int  global_alpha_value;
	bool visible;
	bool flip_immediate;
	bool horizontal_mirror;
	int layer_index;

	union surface_update_flags update_flags;
	/* private to DC core */
	struct dc_plane_status status;
	struct dc_context *ctx;

	/* HACK: Workaround for forcing full reprogramming under some conditions */
	bool force_full_update;

	/* private to dc_surface.c */
	enum dc_irq_source irq_source;
	struct kref refcount;
};

struct dc_plane_info {
	struct plane_size plane_size;
	union dc_tiling_info tiling_info;
	struct dc_plane_dcc_param dcc;
	enum surface_pixel_format format;
	enum dc_rotation_angle rotation;
	enum plane_stereo_format stereo_format;
	enum dc_color_space color_space;
	unsigned int sdr_white_level;
	bool horizontal_mirror;
	bool visible;
	bool per_pixel_alpha;
	bool global_alpha;
	int  global_alpha_value;
	bool input_csc_enabled;
	int layer_index;
};

struct dc_scaling_info {
	struct rect src_rect;
	struct rect dst_rect;
	struct rect clip_rect;
	struct scaling_taps scaling_quality;
};

struct dc_surface_update {
	struct dc_plane_state *surface;

	/* isr safe update parameters.  null means no updates */
	const struct dc_flip_addrs *flip_addr;
	const struct dc_plane_info *plane_info;
	const struct dc_scaling_info *scaling_info;

	/* following updates require alloc/sleep/spin that is not isr safe,
	 * null means no updates
	 */
	const struct dc_gamma *gamma;
	const struct dc_transfer_func *in_transfer_func;

	const struct dc_csc_transform *input_csc_color_matrix;
	const struct fixed31_32 *coeff_reduction_factor;
#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
	const struct dc_transfer_func *func_shaper;
	const struct dc_3dlut *lut3d_func;
	const struct dc_transfer_func *blend_tf;
#endif
};

/*
 * Create a new surface with default parameters;
 */
struct dc_plane_state *dc_create_plane_state(struct dc *dc);
const struct dc_plane_status *dc_plane_get_status(
		const struct dc_plane_state *plane_state);

void dc_plane_state_retain(struct dc_plane_state *plane_state);
void dc_plane_state_release(struct dc_plane_state *plane_state);

void dc_gamma_retain(struct dc_gamma *dc_gamma);
void dc_gamma_release(struct dc_gamma **dc_gamma);
struct dc_gamma *dc_create_gamma(void);

void dc_transfer_func_retain(struct dc_transfer_func *dc_tf);
void dc_transfer_func_release(struct dc_transfer_func *dc_tf);
struct dc_transfer_func *dc_create_transfer_func(void);

#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
struct dc_3dlut *dc_create_3dlut_func(void);
void dc_3dlut_func_release(struct dc_3dlut *lut);
void dc_3dlut_func_retain(struct dc_3dlut *lut);
#endif
/*
 * This structure holds a surface address.  There could be multiple addresses
 * in cases such as Stereo 3D, Planar YUV, etc.  Other per-flip attributes such
 * as frame durations and DCC format can also be set.
 */
struct dc_flip_addrs {
	struct dc_plane_address address;
	unsigned int flip_timestamp_in_us;
	bool flip_immediate;
	/* TODO: add flip duration for FreeSync */
};

bool dc_post_update_surfaces_to_stream(
		struct dc *dc);

#include "dc_stream.h"

/*
 * Structure to store surface/stream associations for validation
 */
struct dc_validation_set {
	struct dc_stream_state *stream;
	struct dc_plane_state *plane_states[MAX_SURFACES];
	uint8_t plane_count;
};

bool dc_validate_seamless_boot_timing(const struct dc *dc,
				const struct dc_sink *sink,
				struct dc_crtc_timing *crtc_timing);

enum dc_status dc_validate_plane(struct dc *dc, const struct dc_plane_state *plane_state);

void get_clock_requirements_for_state(struct dc_state *state, struct AsicStateEx *info);

bool dc_set_generic_gpio_for_stereo(bool enable,
		struct gpio_service *gpio_service);

/*
 * fast_validate: we return after determining if we can support the new state,
 * but before we populate the programming info
 */
enum dc_status dc_validate_global_state(
		struct dc *dc,
		struct dc_state *new_ctx,
		bool fast_validate);


void dc_resource_state_construct(
		const struct dc *dc,
		struct dc_state *dst_ctx);

void dc_resource_state_copy_construct(
		const struct dc_state *src_ctx,
		struct dc_state *dst_ctx);

void dc_resource_state_copy_construct_current(
		const struct dc *dc,
		struct dc_state *dst_ctx);

void dc_resource_state_destruct(struct dc_state *context);

/*
 * TODO update to make it about validation sets
 * Set up streams and links associated to drive sinks
 * The streams parameter is an absolute set of all active streams.
 *
 * After this call:
 *   Phy, Encoder, Timing Generator are programmed and enabled.
 *   New streams are enabled with blank stream; no memory read.
 */
bool dc_commit_state(struct dc *dc, struct dc_state *context);


struct dc_state *dc_create_state(struct dc *dc);
struct dc_state *dc_copy_state(struct dc_state *src_ctx);
void dc_retain_state(struct dc_state *context);
void dc_release_state(struct dc_state *context);

/*******************************************************************************
 * Link Interfaces
 ******************************************************************************/

struct dpcd_caps {
	union dpcd_rev dpcd_rev;
	union max_lane_count max_ln_count;
	union max_down_spread max_down_spread;
	union dprx_feature dprx_feature;

	/* valid only for eDP v1.4 or higher*/
	uint8_t edp_supported_link_rates_count;
	enum dc_link_rate edp_supported_link_rates[8];

	/* dongle type (DP converter, CV smart dongle) */
	enum display_dongle_type dongle_type;
	/* branch device or sink device */
	bool is_branch_dev;
	/* Dongle's downstream count. */
	union sink_count sink_count;
	/* If dongle_type == DISPLAY_DONGLE_DP_HDMI_CONVERTER,
	indicates 'Frame Sequential-to-lllFrame Pack' conversion capability.*/
	struct dc_dongle_caps dongle_caps;

	uint32_t sink_dev_id;
	int8_t sink_dev_id_str[6];
	int8_t sink_hw_revision;
	int8_t sink_fw_revision[2];

	uint32_t branch_dev_id;
	int8_t branch_dev_name[6];
	int8_t branch_hw_revision;
	int8_t branch_fw_revision[2];

	bool allow_invalid_MSA_timing_param;
	bool panel_mode_edp;
	bool dpcd_display_control_capable;
	bool ext_receiver_cap_field_present;
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	union dpcd_fec_capability fec_cap;
	struct dpcd_dsc_capabilities dsc_caps;
#endif
};

#include "dc_link.h"

/*******************************************************************************
 * Sink Interfaces - A sink corresponds to a display output device
 ******************************************************************************/

struct dc_container_id {
	// 128bit GUID in binary form
	unsigned char  guid[16];
	// 8 byte port ID -> ELD.PortID
	unsigned int   portId[2];
	// 128bit GUID in binary formufacturer name -> ELD.ManufacturerName
	unsigned short manufacturerName;
	// 2 byte product code -> ELD.ProductCode
	unsigned short productCode;
};


#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
struct dc_sink_dsc_caps {
	// 'true' if these are virtual DPCD's DSC caps (immediately upstream of sink in MST topology),
	// 'false' if they are sink's DSC caps
	bool is_virtual_dpcd_dsc;
	struct dsc_dec_dpcd_caps dsc_dec_caps;
};
#endif

/*
 * The sink structure contains EDID and other display device properties
 */
struct dc_sink {
	enum signal_type sink_signal;
	struct dc_edid dc_edid; /* raw edid */
	struct dc_edid_caps edid_caps; /* parse display caps */
	struct dc_container_id *dc_container_id;
	uint32_t dongle_max_pix_clk;
	void *priv;
	struct stereo_3d_features features_3d[TIMING_3D_FORMAT_MAX];
	bool converter_disable_audio;

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	struct dc_sink_dsc_caps sink_dsc_caps;
#endif

	/* private to DC core */
	struct dc_link *link;
	struct dc_context *ctx;

	uint32_t sink_id;

	/* private to dc_sink.c */
	// refcount must be the last member in dc_sink, since we want the
	// sink structure to be logically cloneable up to (but not including)
	// refcount
	struct kref refcount;
};

void dc_sink_retain(struct dc_sink *sink);
void dc_sink_release(struct dc_sink *sink);

struct dc_sink_init_data {
	enum signal_type sink_signal;
	struct dc_link *link;
	uint32_t dongle_max_pix_clk;
	bool converter_disable_audio;
};

struct dc_sink *dc_sink_create(const struct dc_sink_init_data *init_params);

/* Newer interfaces  */
struct dc_cursor {
	struct dc_plane_address address;
	struct dc_cursor_attributes attributes;
};


/*******************************************************************************
 * Interrupt interfaces
 ******************************************************************************/
enum dc_irq_source dc_interrupt_to_irq_source(
		struct dc *dc,
		uint32_t src_id,
		uint32_t ext_id);
bool dc_interrupt_set(struct dc *dc, enum dc_irq_source src, bool enable);
void dc_interrupt_ack(struct dc *dc, enum dc_irq_source src);
enum dc_irq_source dc_get_hpd_irq_source_at_index(
		struct dc *dc, uint32_t link_index);

/*******************************************************************************
 * Power Interfaces
 ******************************************************************************/

void dc_set_power_state(
		struct dc *dc,
		enum dc_acpi_cm_power_state power_state);
void dc_resume(struct dc *dc);
unsigned int dc_get_current_backlight_pwm(struct dc *dc);
unsigned int dc_get_target_backlight_pwm(struct dc *dc);

bool dc_is_dmcu_initialized(struct dc *dc);

enum dc_status dc_set_clock(struct dc *dc, enum dc_clock_type clock_type, uint32_t clk_khz, uint32_t stepping);
void dc_get_clock(struct dc *dc, enum dc_clock_type clock_type, struct dc_clock_config *clock_cfg);
#if defined(CONFIG_DRM_AMD_DC_DSC_SUPPORT)
/*******************************************************************************
 * DSC Interfaces
 ******************************************************************************/
#include "dc_dsc.h"
#endif
#endif /* DC_INTERFACE_H_ */
