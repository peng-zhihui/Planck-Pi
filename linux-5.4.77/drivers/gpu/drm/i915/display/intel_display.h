/*
 * Copyright © 2006-2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef _INTEL_DISPLAY_H_
#define _INTEL_DISPLAY_H_

#include <drm/drm_util.h>
#include <drm/i915_drm.h>

enum link_m_n_set;
struct dpll;
struct drm_connector;
struct drm_device;
struct drm_encoder;
struct drm_file;
struct drm_framebuffer;
struct drm_i915_error_state_buf;
struct drm_i915_gem_object;
struct drm_i915_private;
struct drm_modeset_acquire_ctx;
struct drm_plane;
struct drm_plane_state;
struct i915_ggtt_view;
struct intel_crtc;
struct intel_crtc_state;
struct intel_digital_port;
struct intel_dp;
struct intel_encoder;
struct intel_load_detect_pipe;
struct intel_plane;
struct intel_plane_state;
struct intel_remapped_info;
struct intel_rotation_info;

enum i915_gpio {
	GPIOA,
	GPIOB,
	GPIOC,
	GPIOD,
	GPIOE,
	GPIOF,
	GPIOG,
	GPIOH,
	__GPIOI_UNUSED,
	GPIOJ,
	GPIOK,
	GPIOL,
	GPIOM,
	GPION,
	GPIOO,
};

/*
 * Keep the pipe enum values fixed: the code assumes that PIPE_A=0, the
 * rest have consecutive values and match the enum values of transcoders
 * with a 1:1 transcoder -> pipe mapping.
 */
enum pipe {
	INVALID_PIPE = -1,

	PIPE_A = 0,
	PIPE_B,
	PIPE_C,
	PIPE_D,
	_PIPE_EDP,

	I915_MAX_PIPES = _PIPE_EDP
};

#define pipe_name(p) ((p) + 'A')

enum transcoder {
	/*
	 * The following transcoders have a 1:1 transcoder -> pipe mapping,
	 * keep their values fixed: the code assumes that TRANSCODER_A=0, the
	 * rest have consecutive values and match the enum values of the pipes
	 * they map to.
	 */
	TRANSCODER_A = PIPE_A,
	TRANSCODER_B = PIPE_B,
	TRANSCODER_C = PIPE_C,
	TRANSCODER_D = PIPE_D,

	/*
	 * The following transcoders can map to any pipe, their enum value
	 * doesn't need to stay fixed.
	 */
	TRANSCODER_EDP,
	TRANSCODER_DSI_0,
	TRANSCODER_DSI_1,
	TRANSCODER_DSI_A = TRANSCODER_DSI_0,	/* legacy DSI */
	TRANSCODER_DSI_C = TRANSCODER_DSI_1,	/* legacy DSI */

	I915_MAX_TRANSCODERS
};

static inline const char *transcoder_name(enum transcoder transcoder)
{
	switch (transcoder) {
	case TRANSCODER_A:
		return "A";
	case TRANSCODER_B:
		return "B";
	case TRANSCODER_C:
		return "C";
	case TRANSCODER_D:
		return "D";
	case TRANSCODER_EDP:
		return "EDP";
	case TRANSCODER_DSI_A:
		return "DSI A";
	case TRANSCODER_DSI_C:
		return "DSI C";
	default:
		return "<invalid>";
	}
}

static inline bool transcoder_is_dsi(enum transcoder transcoder)
{
	return transcoder == TRANSCODER_DSI_A || transcoder == TRANSCODER_DSI_C;
}

/*
 * Global legacy plane identifier. Valid only for primary/sprite
 * planes on pre-g4x, and only for primary planes on g4x-bdw.
 */
enum i9xx_plane_id {
	PLANE_A,
	PLANE_B,
	PLANE_C,
};

#define plane_name(p) ((p) + 'A')
#define sprite_name(p, s) ((p) * RUNTIME_INFO(dev_priv)->num_sprites[(p)] + (s) + 'A')

/*
 * Per-pipe plane identifier.
 * I915_MAX_PLANES in the enum below is the maximum (across all platforms)
 * number of planes per CRTC.  Not all platforms really have this many planes,
 * which means some arrays of size I915_MAX_PLANES may have unused entries
 * between the topmost sprite plane and the cursor plane.
 *
 * This is expected to be passed to various register macros
 * (eg. PLANE_CTL(), PS_PLANE_SEL(), etc.) so adjust with care.
 */
enum plane_id {
	PLANE_PRIMARY,
	PLANE_SPRITE0,
	PLANE_SPRITE1,
	PLANE_SPRITE2,
	PLANE_SPRITE3,
	PLANE_SPRITE4,
	PLANE_SPRITE5,
	PLANE_CURSOR,

	I915_MAX_PLANES,
};

#define for_each_plane_id_on_crtc(__crtc, __p) \
	for ((__p) = PLANE_PRIMARY; (__p) < I915_MAX_PLANES; (__p)++) \
		for_each_if((__crtc)->plane_ids_mask & BIT(__p))

/*
 * Ports identifier referenced from other drivers.
 * Expected to remain stable over time
 */
static inline const char *port_identifier(enum port port)
{
	switch (port) {
	case PORT_A:
		return "Port A";
	case PORT_B:
		return "Port B";
	case PORT_C:
		return "Port C";
	case PORT_D:
		return "Port D";
	case PORT_E:
		return "Port E";
	case PORT_F:
		return "Port F";
	case PORT_G:
		return "Port G";
	case PORT_H:
		return "Port H";
	case PORT_I:
		return "Port I";
	default:
		return "<invalid>";
	}
}

enum tc_port {
	PORT_TC_NONE = -1,

	PORT_TC1 = 0,
	PORT_TC2,
	PORT_TC3,
	PORT_TC4,
	PORT_TC5,
	PORT_TC6,

	I915_MAX_TC_PORTS
};

enum tc_port_mode {
	TC_PORT_TBT_ALT,
	TC_PORT_DP_ALT,
	TC_PORT_LEGACY,
};

enum dpio_channel {
	DPIO_CH0,
	DPIO_CH1
};

enum dpio_phy {
	DPIO_PHY0,
	DPIO_PHY1,
	DPIO_PHY2,
};

#define I915_NUM_PHYS_VLV 2

enum aux_ch {
	AUX_CH_A,
	AUX_CH_B,
	AUX_CH_C,
	AUX_CH_D,
	AUX_CH_E, /* ICL+ */
	AUX_CH_F,
};

#define aux_ch_name(a) ((a) + 'A')

/* Used by dp and fdi links */
struct intel_link_m_n {
	u32 tu;
	u32 gmch_m;
	u32 gmch_n;
	u32 link_m;
	u32 link_n;
};

enum phy {
	PHY_NONE = -1,

	PHY_A = 0,
	PHY_B,
	PHY_C,
	PHY_D,
	PHY_E,
	PHY_F,
	PHY_G,
	PHY_H,
	PHY_I,

	I915_MAX_PHYS
};

#define phy_name(a) ((a) + 'A')

enum phy_fia {
	FIA1,
	FIA2,
	FIA3,
};

#define for_each_pipe(__dev_priv, __p) \
	for ((__p) = 0; (__p) < INTEL_INFO(__dev_priv)->num_pipes; (__p)++)

#define for_each_pipe_masked(__dev_priv, __p, __mask) \
	for ((__p) = 0; (__p) < INTEL_INFO(__dev_priv)->num_pipes; (__p)++) \
		for_each_if((__mask) & BIT(__p))

#define for_each_cpu_transcoder_masked(__dev_priv, __t, __mask) \
	for ((__t) = 0; (__t) < I915_MAX_TRANSCODERS; (__t)++)	\
		for_each_if ((__mask) & (1 << (__t)))

#define for_each_universal_plane(__dev_priv, __pipe, __p)		\
	for ((__p) = 0;							\
	     (__p) < RUNTIME_INFO(__dev_priv)->num_sprites[(__pipe)] + 1;	\
	     (__p)++)

#define for_each_sprite(__dev_priv, __p, __s)				\
	for ((__s) = 0;							\
	     (__s) < RUNTIME_INFO(__dev_priv)->num_sprites[(__p)];	\
	     (__s)++)

#define for_each_port_masked(__port, __ports_mask) \
	for ((__port) = PORT_A; (__port) < I915_MAX_PORTS; (__port)++)	\
		for_each_if((__ports_mask) & BIT(__port))

#define for_each_phy_masked(__phy, __phys_mask) \
	for ((__phy) = PHY_A; (__phy) < I915_MAX_PHYS; (__phy)++)	\
		for_each_if((__phys_mask) & BIT(__phy))

#define for_each_crtc(dev, crtc) \
	list_for_each_entry(crtc, &(dev)->mode_config.crtc_list, head)

#define for_each_intel_plane(dev, intel_plane) \
	list_for_each_entry(intel_plane,			\
			    &(dev)->mode_config.plane_list,	\
			    base.head)

#define for_each_intel_plane_mask(dev, intel_plane, plane_mask)		\
	list_for_each_entry(intel_plane,				\
			    &(dev)->mode_config.plane_list,		\
			    base.head)					\
		for_each_if((plane_mask) &				\
			    drm_plane_mask(&intel_plane->base)))

#define for_each_intel_plane_on_crtc(dev, intel_crtc, intel_plane)	\
	list_for_each_entry(intel_plane,				\
			    &(dev)->mode_config.plane_list,		\
			    base.head)					\
		for_each_if((intel_plane)->pipe == (intel_crtc)->pipe)

#define for_each_intel_crtc(dev, intel_crtc)				\
	list_for_each_entry(intel_crtc,					\
			    &(dev)->mode_config.crtc_list,		\
			    base.head)

#define for_each_intel_crtc_mask(dev, intel_crtc, crtc_mask)		\
	list_for_each_entry(intel_crtc,					\
			    &(dev)->mode_config.crtc_list,		\
			    base.head)					\
		for_each_if((crtc_mask) & drm_crtc_mask(&intel_crtc->base))

#define for_each_intel_encoder(dev, intel_encoder)		\
	list_for_each_entry(intel_encoder,			\
			    &(dev)->mode_config.encoder_list,	\
			    base.head)

#define for_each_intel_dp(dev, intel_encoder)			\
	for_each_intel_encoder(dev, intel_encoder)		\
		for_each_if(intel_encoder_is_dp(intel_encoder))

#define for_each_intel_connector_iter(intel_connector, iter) \
	while ((intel_connector = to_intel_connector(drm_connector_list_iter_next(iter))))

#define for_each_encoder_on_crtc(dev, __crtc, intel_encoder) \
	list_for_each_entry((intel_encoder), &(dev)->mode_config.encoder_list, base.head) \
		for_each_if((intel_encoder)->base.crtc == (__crtc))

#define for_each_connector_on_encoder(dev, __encoder, intel_connector) \
	list_for_each_entry((intel_connector), &(dev)->mode_config.connector_list, base.head) \
		for_each_if((intel_connector)->base.encoder == (__encoder))

#define for_each_old_intel_plane_in_state(__state, plane, old_plane_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.dev->mode_config.num_total_plane && \
		     ((plane) = to_intel_plane((__state)->base.planes[__i].ptr), \
		      (old_plane_state) = to_intel_plane_state((__state)->base.planes[__i].old_state), 1); \
	     (__i)++) \
		for_each_if(plane)

#define for_each_new_intel_plane_in_state(__state, plane, new_plane_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.dev->mode_config.num_total_plane && \
		     ((plane) = to_intel_plane((__state)->base.planes[__i].ptr), \
		      (new_plane_state) = to_intel_plane_state((__state)->base.planes[__i].new_state), 1); \
	     (__i)++) \
		for_each_if(plane)

#define for_each_new_intel_crtc_in_state(__state, crtc, new_crtc_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.dev->mode_config.num_crtc && \
		     ((crtc) = to_intel_crtc((__state)->base.crtcs[__i].ptr), \
		      (new_crtc_state) = to_intel_crtc_state((__state)->base.crtcs[__i].new_state), 1); \
	     (__i)++) \
		for_each_if(crtc)

#define for_each_oldnew_intel_plane_in_state(__state, plane, old_plane_state, new_plane_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.dev->mode_config.num_total_plane && \
		     ((plane) = to_intel_plane((__state)->base.planes[__i].ptr), \
		      (old_plane_state) = to_intel_plane_state((__state)->base.planes[__i].old_state), \
		      (new_plane_state) = to_intel_plane_state((__state)->base.planes[__i].new_state), 1); \
	     (__i)++) \
		for_each_if(plane)

#define for_each_oldnew_intel_crtc_in_state(__state, crtc, old_crtc_state, new_crtc_state, __i) \
	for ((__i) = 0; \
	     (__i) < (__state)->base.dev->mode_config.num_crtc && \
		     ((crtc) = to_intel_crtc((__state)->base.crtcs[__i].ptr), \
		      (old_crtc_state) = to_intel_crtc_state((__state)->base.crtcs[__i].old_state), \
		      (new_crtc_state) = to_intel_crtc_state((__state)->base.crtcs[__i].new_state), 1); \
	     (__i)++) \
		for_each_if(crtc)

void intel_link_compute_m_n(u16 bpp, int nlanes,
			    int pixel_clock, int link_clock,
			    struct intel_link_m_n *m_n,
			    bool constant_n, bool fec_enable);
bool is_ccs_modifier(u64 modifier);
void lpt_disable_clkout_dp(struct drm_i915_private *dev_priv);
u32 intel_plane_fb_max_stride(struct drm_i915_private *dev_priv,
			      u32 pixel_format, u64 modifier);
bool intel_plane_can_remap(const struct intel_plane_state *plane_state);
enum phy intel_port_to_phy(struct drm_i915_private *i915, enum port port);

void intel_plane_destroy(struct drm_plane *plane);
void i830_enable_pipe(struct drm_i915_private *dev_priv, enum pipe pipe);
void i830_disable_pipe(struct drm_i915_private *dev_priv, enum pipe pipe);
enum pipe intel_crtc_pch_transcoder(struct intel_crtc *crtc);
int vlv_get_hpll_vco(struct drm_i915_private *dev_priv);
int vlv_get_cck_clock(struct drm_i915_private *dev_priv,
		      const char *name, u32 reg, int ref_freq);
int vlv_get_cck_clock_hpll(struct drm_i915_private *dev_priv,
			   const char *name, u32 reg);
void lpt_disable_pch_transcoder(struct drm_i915_private *dev_priv);
void lpt_disable_iclkip(struct drm_i915_private *dev_priv);
void intel_init_display_hooks(struct drm_i915_private *dev_priv);
unsigned int intel_fb_xy_to_linear(int x, int y,
				   const struct intel_plane_state *state,
				   int plane);
unsigned int intel_fb_align_height(const struct drm_framebuffer *fb,
				   int color_plane, unsigned int height);
void intel_add_fb_offsets(int *x, int *y,
			  const struct intel_plane_state *state, int plane);
unsigned int intel_rotation_info_size(const struct intel_rotation_info *rot_info);
unsigned int intel_remapped_info_size(const struct intel_remapped_info *rem_info);
bool intel_has_pending_fb_unpin(struct drm_i915_private *dev_priv);
int intel_display_suspend(struct drm_device *dev);
void intel_pps_unlock_regs_wa(struct drm_i915_private *dev_priv);
void intel_encoder_destroy(struct drm_encoder *encoder);
struct drm_display_mode *
intel_encoder_current_mode(struct intel_encoder *encoder);
bool intel_phy_is_combo(struct drm_i915_private *dev_priv, enum phy phy);
bool intel_phy_is_tc(struct drm_i915_private *dev_priv, enum phy phy);
enum tc_port intel_port_to_tc(struct drm_i915_private *dev_priv,
			      enum port port);
int intel_get_pipe_from_crtc_id_ioctl(struct drm_device *dev, void *data,
				      struct drm_file *file_priv);
enum transcoder intel_pipe_to_cpu_transcoder(struct drm_i915_private *dev_priv,
					     enum pipe pipe);
u32 intel_crtc_get_vblank_counter(struct intel_crtc *crtc);

int ironlake_get_lanes_required(int target_clock, int link_bw, int bpp);
void vlv_wait_port_ready(struct drm_i915_private *dev_priv,
			 struct intel_digital_port *dport,
			 unsigned int expected_mask);
int intel_get_load_detect_pipe(struct drm_connector *connector,
			       const struct drm_display_mode *mode,
			       struct intel_load_detect_pipe *old,
			       struct drm_modeset_acquire_ctx *ctx);
void intel_release_load_detect_pipe(struct drm_connector *connector,
				    struct intel_load_detect_pipe *old,
				    struct drm_modeset_acquire_ctx *ctx);
struct i915_vma *
intel_pin_and_fence_fb_obj(struct drm_framebuffer *fb,
			   const struct i915_ggtt_view *view,
			   bool uses_fence,
			   unsigned long *out_flags);
void intel_unpin_fb_vma(struct i915_vma *vma, unsigned long flags);
struct drm_framebuffer *
intel_framebuffer_create(struct drm_i915_gem_object *obj,
			 struct drm_mode_fb_cmd2 *mode_cmd);
int intel_prepare_plane_fb(struct drm_plane *plane,
			   struct drm_plane_state *new_state);
void intel_cleanup_plane_fb(struct drm_plane *plane,
			    struct drm_plane_state *old_state);

void assert_pch_transcoder_disabled(struct drm_i915_private *dev_priv,
				    enum pipe pipe);

int vlv_force_pll_on(struct drm_i915_private *dev_priv, enum pipe pipe,
		     const struct dpll *dpll);
void vlv_force_pll_off(struct drm_i915_private *dev_priv, enum pipe pipe);
int lpt_get_iclkip(struct drm_i915_private *dev_priv);
bool intel_fuzzy_clock_check(int clock1, int clock2);

void intel_prepare_reset(struct drm_i915_private *dev_priv);
void intel_finish_reset(struct drm_i915_private *dev_priv);
void intel_dp_get_m_n(struct intel_crtc *crtc,
		      struct intel_crtc_state *pipe_config);
void intel_dp_set_m_n(const struct intel_crtc_state *crtc_state,
		      enum link_m_n_set m_n);
void intel_dp_ycbcr_420_enable(struct intel_dp *intel_dp,
			       const struct intel_crtc_state *crtc_state);
int intel_dotclock_calculate(int link_freq, const struct intel_link_m_n *m_n);
bool bxt_find_best_dpll(struct intel_crtc_state *crtc_state,
			struct dpll *best_clock);
int chv_calc_dpll_params(int refclk, struct dpll *pll_clock);

bool intel_crtc_active(struct intel_crtc *crtc);
bool hsw_crtc_state_ips_capable(const struct intel_crtc_state *crtc_state);
void hsw_enable_ips(const struct intel_crtc_state *crtc_state);
void hsw_disable_ips(const struct intel_crtc_state *crtc_state);
enum intel_display_power_domain intel_port_to_power_domain(enum port port);
enum intel_display_power_domain
intel_aux_power_domain(struct intel_digital_port *dig_port);
void intel_mode_from_pipe_config(struct drm_display_mode *mode,
				 struct intel_crtc_state *pipe_config);
void intel_crtc_arm_fifo_underrun(struct intel_crtc *crtc,
				  struct intel_crtc_state *crtc_state);

u16 skl_scaler_calc_phase(int sub, int scale, bool chroma_center);
int skl_update_scaler_crtc(struct intel_crtc_state *crtc_state);
int skl_max_scale(const struct intel_crtc_state *crtc_state,
		  u32 pixel_format);
u32 glk_plane_color_ctl(const struct intel_crtc_state *crtc_state,
			const struct intel_plane_state *plane_state);
u32 glk_plane_color_ctl_crtc(const struct intel_crtc_state *crtc_state);
u32 skl_plane_ctl(const struct intel_crtc_state *crtc_state,
		  const struct intel_plane_state *plane_state);
u32 skl_plane_ctl_crtc(const struct intel_crtc_state *crtc_state);
u32 skl_plane_stride(const struct intel_plane_state *plane_state,
		     int plane);
int skl_check_plane_surface(struct intel_plane_state *plane_state);
int i9xx_check_plane_surface(struct intel_plane_state *plane_state);
int skl_format_to_fourcc(int format, bool rgb_order, bool alpha);
unsigned int i9xx_plane_max_stride(struct intel_plane *plane,
				   u32 pixel_format, u64 modifier,
				   unsigned int rotation);
int bdw_get_pipemisc_bpp(struct intel_crtc *crtc);

struct intel_display_error_state *
intel_display_capture_error_state(struct drm_i915_private *dev_priv);
void intel_display_print_error_state(struct drm_i915_error_state_buf *e,
				     struct intel_display_error_state *error);

/* modesetting */
void intel_modeset_init_hw(struct drm_device *dev);
int intel_modeset_init(struct drm_device *dev);
void intel_modeset_driver_remove(struct drm_device *dev);
int intel_modeset_vga_set_state(struct drm_i915_private *dev_priv, bool state);
void intel_display_resume(struct drm_device *dev);
void i915_redisable_vga(struct drm_i915_private *dev_priv);
void i915_redisable_vga_power_on(struct drm_i915_private *dev_priv);
void intel_init_pch_refclk(struct drm_i915_private *dev_priv);

/* modesetting asserts */
void assert_panel_unlocked(struct drm_i915_private *dev_priv,
			   enum pipe pipe);
void assert_pll(struct drm_i915_private *dev_priv,
		enum pipe pipe, bool state);
#define assert_pll_enabled(d, p) assert_pll(d, p, true)
#define assert_pll_disabled(d, p) assert_pll(d, p, false)
void assert_dsi_pll(struct drm_i915_private *dev_priv, bool state);
#define assert_dsi_pll_enabled(d) assert_dsi_pll(d, true)
#define assert_dsi_pll_disabled(d) assert_dsi_pll(d, false)
void assert_fdi_rx_pll(struct drm_i915_private *dev_priv,
		       enum pipe pipe, bool state);
#define assert_fdi_rx_pll_enabled(d, p) assert_fdi_rx_pll(d, p, true)
#define assert_fdi_rx_pll_disabled(d, p) assert_fdi_rx_pll(d, p, false)
void assert_pipe(struct drm_i915_private *dev_priv, enum pipe pipe, bool state);
#define assert_pipe_enabled(d, p) assert_pipe(d, p, true)
#define assert_pipe_disabled(d, p) assert_pipe(d, p, false)

/* Use I915_STATE_WARN(x) and I915_STATE_WARN_ON() (rather than WARN() and
 * WARN_ON()) for hw state sanity checks to check for unexpected conditions
 * which may not necessarily be a user visible problem.  This will either
 * WARN() or DRM_ERROR() depending on the verbose_checks moduleparam, to
 * enable distros and users to tailor their preferred amount of i915 abrt
 * spam.
 */
#define I915_STATE_WARN(condition, format...) ({			\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		if (!WARN(i915_modparams.verbose_state_checks, format))	\
			DRM_ERROR(format);				\
	unlikely(__ret_warn_on);					\
})

#define I915_STATE_WARN_ON(x)						\
	I915_STATE_WARN((x), "%s", "WARN_ON(" __stringify(x) ")")

#endif
