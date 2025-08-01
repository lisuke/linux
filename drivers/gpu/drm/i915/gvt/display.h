/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Ke Yu
 *    Zhiyuan Lv <zhiyuan.lv@intel.com>
 *
 * Contributors:
 *    Terrence Xu <terrence.xu@intel.com>
 *    Changbin Du <changbin.du@intel.com>
 *    Bing Niu <bing.niu@intel.com>
 *    Zhi Wang <zhi.a.wang@intel.com>
 *
 */

#ifndef _GVT_DISPLAY_H_
#define _GVT_DISPLAY_H_

#include <linux/types.h>
#include <linux/hrtimer.h>

struct intel_gvt;
struct intel_vgpu;

#define SBI_REG_MAX	20
#define DPCD_SIZE	0x700

#define intel_vgpu_port(vgpu, port) \
	(&(vgpu->display.ports[port]))

#define intel_vgpu_has_monitor_on_port(vgpu, port) \
	(intel_vgpu_port(vgpu, port)->edid && \
		intel_vgpu_port(vgpu, port)->edid->data_valid)

#define intel_vgpu_port_is_dp(vgpu, port) \
	((intel_vgpu_port(vgpu, port)->type == GVT_DP_A) || \
	(intel_vgpu_port(vgpu, port)->type == GVT_DP_B) || \
	(intel_vgpu_port(vgpu, port)->type == GVT_DP_C) || \
	(intel_vgpu_port(vgpu, port)->type == GVT_DP_D))

#define INTEL_GVT_MAX_UEVENT_VARS	3

#define AUX_NATIVE_REPLY_NAK    (0x1 << 4)

#define AUX_BURST_SIZE          20

struct intel_vgpu_sbi_register {
	unsigned int offset;
	u32 value;
};

struct intel_vgpu_sbi {
	int number;
	struct intel_vgpu_sbi_register registers[SBI_REG_MAX];
};

enum intel_gvt_plane_type {
	PRIMARY_PLANE = 0,
	CURSOR_PLANE,
	SPRITE_PLANE,
	MAX_PLANE
};

struct intel_vgpu_dpcd_data {
	bool data_valid;
	u8 data[DPCD_SIZE];
};

enum intel_vgpu_port_type {
	GVT_CRT = 0,
	GVT_DP_A,
	GVT_DP_B,
	GVT_DP_C,
	GVT_DP_D,
	GVT_HDMI_B,
	GVT_HDMI_C,
	GVT_HDMI_D,
	GVT_PORT_MAX
};

enum intel_vgpu_edid {
	GVT_EDID_1024_768,
	GVT_EDID_1920_1200,
	GVT_EDID_NUM,
};

#define GVT_DEFAULT_REFRESH_RATE 60
struct intel_vgpu_port {
	/* per display EDID information */
	struct intel_vgpu_edid_data *edid;
	/* per display DPCD information */
	struct intel_vgpu_dpcd_data *dpcd;
	int type;
	enum intel_vgpu_edid id;
	/* x1000 to get accurate 59.94, 24.976, 29.94, etc. in timing std. */
	u32 vrefresh_k;
};

struct intel_vgpu_vblank_timer {
	struct hrtimer timer;
	u32 vrefresh_k;
	u64 period;
};

static inline char *vgpu_edid_str(enum intel_vgpu_edid id)
{
	switch (id) {
	case GVT_EDID_1024_768:
		return "1024x768";
	case GVT_EDID_1920_1200:
		return "1920x1200";
	default:
		return "";
	}
}

static inline unsigned int vgpu_edid_xres(enum intel_vgpu_edid id)
{
	switch (id) {
	case GVT_EDID_1024_768:
		return 1024;
	case GVT_EDID_1920_1200:
		return 1920;
	default:
		return 0;
	}
}

static inline unsigned int vgpu_edid_yres(enum intel_vgpu_edid id)
{
	switch (id) {
	case GVT_EDID_1024_768:
		return 768;
	case GVT_EDID_1920_1200:
		return 1200;
	default:
		return 0;
	}
}

void intel_vgpu_emulate_vblank(struct intel_vgpu *vgpu);
void vgpu_update_vblank_emulation(struct intel_vgpu *vgpu, bool turnon);

int intel_vgpu_init_display(struct intel_vgpu *vgpu, u64 resolution);
void intel_vgpu_reset_display(struct intel_vgpu *vgpu);
void intel_vgpu_clean_display(struct intel_vgpu *vgpu);

int pipe_is_enabled(struct intel_vgpu *vgpu, int pipe);

#endif
