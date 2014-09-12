// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "math/lin/matrix4x4.h"

#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/HDRemaster.h"
#include "GPU/ge_constants.h"

#include "GPU/Directx9/VertexDecoderDX9.h"
#include "GPU/Directx9/VertexShaderGeneratorDX9.h"

namespace DX9 {


// Always use float for decoding data
#define USE_WEIGHT_HACK
#define USE_TC_HACK

static const u8 tcsize[4] = {0,2,4,8}, tcalign[4] = {0,1,2,4};
static const u8 colsize[8] = {0,0,0,0,2,2,2,4}, colalign[8] = {0,0,0,0,2,2,2,4};
static const u8 nrmsize[4] = {0,3,6,12}, nrmalign[4] = {0,1,2,4};
static const u8 possize[4] = {0,3,6,12}, posalign[4] = {0,1,2,4};
static const u8 wtsize[4] = {0,1,2,4}, wtalign[4] = {0,1,2,4};

inline int align(int n, int align) {
	return (n + (align - 1)) & ~(align - 1);
}

#if 0
// This is what the software transform spits out, and thus w
DecVtxFormat GetTransformedVtxFormat(const DecVtxFormat &fmt) {
	DecVtxFormat tfm = {0};
	int size = 0;
	int offset = 0;
	// Weights disappear during transform.
	if (fmt.uvfmt) {
		// UV always becomes float2.
		tfm.uvfmt = DEC_FLOAT_2;
		tfm.uvoff = offset;
		offset += DecFmtSize(tfm.uvfmt);
	}
	// We always (?) get two colors out, they're floats (although we'd probably be fine with less precision).
	tfm.c0fmt = DEC_FLOAT_4;
	tfm.c0off = offset;
	offset += DecFmtSize(tfm.c0fmt);
	tfm.c1fmt = DEC_FLOAT_3;  // color1 (specular) doesn't have alpha.
	tfm.c1off = offset;
	offset += DecFmtSize(tfm.c1fmt);
	// We never get a normal, it's gone.
	// But we do get a position, and it's always float3.
	tfm.posfmt = DEC_FLOAT_3;
	tfm.posoff = offset;
	offset += DecFmtSize(tfm.posfmt);
	// Update stride.
	tfm.stride = offset;
	return tfm;
}
#endif

void VertexDecoderDX9::Step_WeightsU8() const
{
#ifdef USE_WEIGHT_HACK
	float *wt = (float *)(decoded_ + decFmt.w0off);
	const u8 *wdata = (const u8*)(ptr_);
	int j;
	for (j = 0; j < nweights; j++) {
		wt[j] = wdata[j];
		wt[j] *= (1.0f/255.f);
	}
	while (j & 3)   // Zero additional weights rounding up to 4.
		wt[j++] = 0;
#else
	u8 *wt = (u8 *)(decoded_ + decFmt.w0off);
	const u8 *wdata = (const u8*)(ptr_);
	int j;
	for (j = 0; j < nweights; j++)
		wt[j] = wdata[j];
	while (j & 3)   // Zero additional weights rounding up to 4.
		wt[j++] = 0;
#endif
}

void VertexDecoderDX9::Step_WeightsU16() const
{
#ifdef USE_WEIGHT_HACK
	float *wt = (float *)(decoded_  + decFmt.w0off);
	const u16_le *wdata = (const u16_le*)(ptr_);
	int j;
	for (j = 0; j < nweights; j++) {
		wt[j] =wdata[j];
		wt[j] *= (1.0f/65535.f);
	}
	while (j & 3)   // Zero additional weights rounding up to 4.
		wt[j++] = 0.0f;
#else
	u16 *wt = (u16 *)(decoded_  + decFmt.w0off);
	const u16_le *wdata = (const u16_le*)(ptr_);
	int j;
	for (j = 0; j < nweights; j++)
		wt[j] =wdata[j];
	while (j & 3)   // Zero additional weights rounding up to 4.
		wt[j++] = 0;
#endif
}

// Float weights should be uncommon, we can live with having to multiply these by 2.0
// to avoid special checks in the vertex shader generator.
// (PSP uses 0.0-2.0 fixed point numbers for weights)
void VertexDecoderDX9::Step_WeightsFloat() const
{
	u32 *st = (u32 *)(decoded_ + decFmt.w0off);
	const u32_le *wdata = (const u32_le*)(ptr_);
	int j;
	for (j = 0; j < nweights; j++) {
		st[j] = wdata[j];
	}
	while (j & 3)   // Zero additional weights rounding up to 4.
		st[j++] = 0;
}

void VertexDecoderDX9::Step_TcU8() const
{
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u8 *uvdata = (const u8*)(ptr_ + tcoff);
	uv[0] = uvdata[0] * (1.0f / 128.f);
	uv[1] = uvdata[1] * (1.0f / 128.f);
}

void VertexDecoderDX9::Step_TcU16() const
{
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u16 *uvdata = (const u16*)(ptr_ + tcoff);
	uv[0] = uvdata[0] * (1.0f / 32768.f);
	uv[1] = uvdata[1] * (1.0f / 32768.f);
}

void VertexDecoderDX9::Step_TcU16Double() const
{
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u16 *uvdata = (const u16*)(ptr_ + tcoff);
	uv[0] = uvdata[0] * (1.0f / 16384.f);
	uv[1] = uvdata[1] * (1.0f / 16384.f);
}

void VertexDecoderDX9::Step_TcU16Through() const
{
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u16 *uvdata = (const u16*)(ptr_ + tcoff);
	uv[0] = uvdata[0];
	uv[1] = uvdata[1];
}

void VertexDecoderDX9::Step_TcU16ThroughDouble() const
{
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u16 *uvdata = (const u16*)(ptr_ + tcoff);
	uv[0] = uvdata[0] * 2.0f;
	uv[1] = uvdata[1] * 2.0f;
}

void VertexDecoderDX9::Step_TcFloat() const
{
	u32 *uv = (u32 *)(decoded_ + decFmt.uvoff);
	const u32_le *uvdata = (const u32_le*)(ptr_ + tcoff);
	uv[0] = uvdata[0];
	uv[1] = uvdata[1];
}

void VertexDecoderDX9::Step_TcFloatThrough() const
{
	u32 *uv = (u32 *)(decoded_ + decFmt.uvoff);
	const u32_le *uvdata = (const u32_le*)(ptr_ + tcoff);
	uv[0] = uvdata[0];
	uv[1] = uvdata[1];
}

void VertexDecoderDX9::Step_TcU8Prescale() const {
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u8 *uvdata = (const u8 *)(ptr_ + tcoff);
	uv[0] = (float)uvdata[0] * (1.f / 128.f) * gstate_c.uv.uScale + gstate_c.uv.uOff;
	uv[1] = (float)uvdata[1] * (1.f / 128.f) * gstate_c.uv.vScale + gstate_c.uv.vOff;
}

void VertexDecoderDX9::Step_TcU16Prescale() const {
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const u16_le *uvdata = (const u16_le *)(ptr_ + tcoff);
	uv[0] = (float)uvdata[0] * (1.f / 32768.f) * gstate_c.uv.uScale + gstate_c.uv.uOff;
	uv[1] = (float)uvdata[1] * (1.f / 32768.f) * gstate_c.uv.vScale + gstate_c.uv.vOff;
}

void VertexDecoderDX9::Step_TcFloatPrescale() const {
	float *uv = (float *)(decoded_ + decFmt.uvoff);
	const float_le *uvdata = (const float_le*)(ptr_ + tcoff);
	uv[0] = uvdata[0] * gstate_c.uv.uScale + gstate_c.uv.uOff;
	uv[1] = uvdata[1] * gstate_c.uv.vScale + gstate_c.uv.vOff;
}

void VertexDecoderDX9::Step_Color565() const
{
	u8 *c = decoded_ + decFmt.c0off;
	u16 cdata = (u16)(*(u16_le*)(ptr_ + coloff));
	c[0] = Convert5To8(cdata & 0x1f);
	c[1] = Convert6To8((cdata>>5) & 0x3f);
	c[2] = Convert5To8((cdata>>11) & 0x1f);
	c[3] = 255;
	// Always full alpha.
}

void VertexDecoderDX9::Step_Color5551() const
{
	u8 *c = decoded_ + decFmt.c0off;
	u16 cdata = (u16)(*(u16_le*)(ptr_ + coloff));
	c[0] = Convert5To8(cdata & 0x1f);
	c[1] = Convert5To8((cdata>>5) & 0x1f);
	c[2] = Convert5To8((cdata>>10) & 0x1f);
	c[3] = (cdata >> 15) ? 255 : 0;
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] != 0;
}

void VertexDecoderDX9::Step_Color4444() const
{
	u8 *c = decoded_ + decFmt.c0off;
	u16 cdata = (u16)(*(u16_le*)(ptr_ + coloff));
	c[0] =  Convert4To8((cdata >> (0)) & 0xF);
	c[1] =  Convert4To8((cdata >> (4)) & 0xF);
	c[2] =  Convert4To8((cdata >> (8)) & 0xF);
	c[3] =  Convert4To8((cdata >> (12)) & 0xF);
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] == 255;
}

void VertexDecoderDX9::Step_Color8888() const
{
	u8 *c = (u8*)(decoded_ + decFmt.c0off);
	const u8 *cdata = (const u8*)(ptr_ + coloff);
	c[0] = cdata[0];
	c[1] = cdata[1];
	c[2] = cdata[2];
	c[3] = cdata[3];
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] == 255;
}

void VertexDecoderDX9::Step_Color565Morph() const
{
	float col[3] = {0};
	for (int n = 0; n < morphcount; n++)
	{
		float w = gstate_c.morphWeights[n];		
		u16 cdata = (u16)(*(u16_le*)(ptr_ + onesize_*n + coloff));
		col[0] += w * (cdata & 0x1f) * (255.0f / 31.0f);
		col[1] += w * ((cdata>>5) & 0x3f) * (255.0f / 63.0f);
		col[2] += w * ((cdata>>11) & 0x1f) * (255.0f / 31.0f);
	}
	u8 *c = decoded_ + decFmt.c0off;
	for (int i = 0; i < 3; i++) {
		c[i] = clamp_u8((int)col[i]);
	}
	c[3] = 255;
	// Always full alpha.
}

void VertexDecoderDX9::Step_Color5551Morph() const
{
	float col[4] = {0};
	for (int n = 0; n < morphcount; n++)
	{
		float w = gstate_c.morphWeights[n];
		u16 cdata = (u16)(*(u16_le*)(ptr_ + onesize_*n + coloff));
		col[0] += w * (cdata & 0x1f) * (255.0f / 31.0f);
		col[1] += w * ((cdata>>5) & 0x1f) * (255.0f / 31.0f);
		col[2] += w * ((cdata>>10) & 0x1f) * (255.0f / 31.0f);
		col[3] += w * ((cdata>>15) ? 255.0f : 0.0f);
	}
	u8 *c = decoded_ + decFmt.c0off;
	for (int i = 0; i < 4; i++) {
		c[i] = clamp_u8((int)col[i]);
	}
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] == 255;
}

void VertexDecoderDX9::Step_Color4444Morph() const
{
	float col[4] = {0};
	for (int n = 0; n < morphcount; n++)
	{
		float w = gstate_c.morphWeights[n];
		u16 cdata = (u16)(*(u16_le*)(ptr_ + onesize_*n + coloff));
		for (int j = 0; j < 4; j++)
			col[j] += w * ((cdata >> (j * 4)) & 0xF) * (255.0f / 15.0f);
	}
	u8 *c = decoded_ + decFmt.c0off;
	for (int i = 0; i < 4; i++) {
		c[i] = clamp_u8((int)col[i]);
	}
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] == 255;
}

void VertexDecoderDX9::Step_Color8888Morph() const
{
	float col[4] = {0};
	for (int n = 0; n < morphcount; n++)
	{
		float w = gstate_c.morphWeights[n];
		const u8 *cdata = (const u8*)(ptr_ + onesize_*n + coloff);
		for (int j = 0; j < 4; j++)
			col[j] += w * cdata[j];
	}
	u8 *c = decoded_ + decFmt.c0off;
	for (int i = 0; i < 4; i++) {
		c[i] = clamp_u8((int)col[i]);
	}
	gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && c[3] == 255;
}

void VertexDecoderDX9::Step_NormalS8() const
{
	float *normal = (float *)(decoded_ + decFmt.nrmoff);
	const s8 *sv = (const s8*)(ptr_ + nrmoff);
	for (int j = 0; j < 3; j++)
		normal[j] = (float)(sv[j]) * (1.0f/127.f);
	normal[3] = 0;
}

void VertexDecoderDX9::Step_NormalS16() const
{
	s16 *normal = (s16 *)(decoded_ + decFmt.nrmoff);
	u16 xorval = 0;
	const s16_le *sv = (const s16_le*)(ptr_ + nrmoff);
	for (int j = 0; j < 3; j++)
		normal[j] = sv[j];
	normal[3] = 0;
}

void VertexDecoderDX9::Step_NormalFloat() const
{
	float *normal = (float *)(decoded_ + decFmt.nrmoff);
	const float *fv = (const float*)(ptr_ + nrmoff);

	u32 *v = (u32 *)normal;
	const u32_le *sv = (const u32_le*)fv;

	for (int j = 0; j < 3; j++) 
		v[j] = sv[j];
}

void VertexDecoderDX9::Step_NormalS8Morph() const
{
	float *normal = (float *)(decoded_ + decFmt.nrmoff);
	memset(normal, 0, sizeof(float)*3);
	for (int n = 0; n < morphcount; n++)
	{
		float multiplier = gstate_c.morphWeights[n];
		if (gstate.reversenormals & 1) {
			multiplier = -multiplier;
		}
		const s8 *bv = (const s8*)(ptr_ + onesize_*n + nrmoff);
		multiplier *= (1.0f/127.0f);
		for (int j = 0; j < 3; j++)
			normal[j] += bv[j] * multiplier;
	}
}

void VertexDecoderDX9::Step_NormalS16Morph() const
{
	float *normal = (float *)(decoded_ + decFmt.nrmoff);
	memset(normal, 0, sizeof(float)*3);
	for (int n = 0; n < morphcount; n++)
	{
		float multiplier = gstate_c.morphWeights[n];
		if (gstate.reversenormals & 1) {
			multiplier = -multiplier;
		}
		const s16_le *sv = (const s16_le *)(ptr_ + onesize_*n + nrmoff);
		multiplier *= (1.0f/32767.0f);
		for (int j = 0; j < 3; j++)
			normal[j] += sv[j] * multiplier;
	}
}

void VertexDecoderDX9::Step_NormalFloatMorph() const
{
	float *normal = (float *)(decoded_ + decFmt.nrmoff);
	u32 *v = (u32 *)normal;
	memset(normal, 0, sizeof(float)*3);
	for (int n = 0; n < morphcount; n++)
	{
		float multiplier = gstate_c.morphWeights[n];
		
		const float *fv = (const float*)(ptr_ + onesize_*n + nrmoff);
		const u32_le *sv = (const u32_le*)fv;
		for (int j = 0; j < 3; j++) {
			v[j] = sv[j];
		}
		
		for (int j = 0; j < 3; j++) {
			normal[j] += normal[j] * multiplier;
		}
	}
}

void VertexDecoderDX9::Step_PosS8() const
{
	float *v = (float *)(decoded_ + decFmt.posoff);
	const s8 *sv = (const s8*)(ptr_ + posoff);
	for (int j = 0; j < 3; j++)
		v[j] = (float)sv[j] * (1.0f / 128.0f);
	v[3] = 0;
}

void VertexDecoderDX9::Step_PosS16() const
{
	s16 *v = (s16 *)(decoded_ + decFmt.posoff);
	const s16_le *sv = (const s16_le*)(ptr_ + posoff);
	for (int j = 0; j < 3; j++)
		v[j] = sv[j];
	v[3] = 0;
}

void VertexDecoderDX9::Step_PosFloat() const
{
	u32 *v = (u32 *)(decoded_ + decFmt.posoff);
	const u32_le *sv = (const u32_le*)(ptr_ + posoff);
	v[0] = sv[0];
	v[1] = sv[1];
	v[2] = sv[2];
}

void VertexDecoderDX9::Step_PosS8Through() const
{
	float *v = (float *)(decoded_ + decFmt.posoff);
	const s8 *sv = (const s8*)(ptr_ + posoff);
	v[0] = sv[0];
	v[1] = sv[1];
	v[2] = sv[2];
}

void VertexDecoderDX9::Step_PosS16Through() const
{
	float *v = (float *)(decoded_ + decFmt.posoff);
	const s16_le *sv = (const s16_le *)(ptr_ + posoff);
	const u16_le *uv = (const u16_le *)(ptr_ + posoff);
	v[0] = sv[0];
	v[1] = sv[1];
	v[2] = uv[2];
}

void VertexDecoderDX9::Step_PosFloatThrough() const
{
	u32 *v = (u32 *)(decoded_ + decFmt.posoff);
	const u32_le *fv = (const u32_le *)(ptr_ + posoff);
	v[0] = fv[0];
	v[1] = fv[1];
	v[2] = fv[2];
}

void VertexDecoderDX9::Step_PosS8Morph() const
{
	float *v = (float *)(decoded_ + decFmt.posoff);
	memset(v, 0, sizeof(float) * 3);
	for (int n = 0; n < morphcount; n++) {
		float multiplier = 1.0f / 128.0f;
		const s8 *sv = (const s8*)(ptr_ + onesize_*n + posoff);
		for (int j = 0; j < 3; j++)
			v[j] += (float)sv[j] * (multiplier * gstate_c.morphWeights[n]);
	}
}

void VertexDecoderDX9::Step_PosS16Morph() const
{
	float *v = (float *)(decoded_ + decFmt.posoff);
	memset(v, 0, sizeof(float) * 3);
	for (int n = 0; n < morphcount; n++) {
		float multiplier = 1.0f / 32768.0f;
		const s16_le *sv = (const s16_le*)(ptr_ + onesize_*n + posoff);
		for (int j = 0; j < 3; j++)
			v[j] += (float)sv[j] * (multiplier * gstate_c.morphWeights[n]);
	}
}

void VertexDecoderDX9::Step_PosFloatMorph() const
{
#if 0 // Swapping float is more heavy as swapping u32
	float *v = (float *)(decoded_ + decFmt.posoff);
	memset(v, 0, sizeof(float) * 3);
	for (int n = 0; n < morphcount; n++) {
		const float_le *fv = (const float_le*)(ptr_ + onesize_*n + posoff);
		for (int j = 0; j < 3; j++)
			v[j] += fv[j] * gstate_c.morphWeights[n];
	}
#else
	float *pos = (float *)(decoded_ + decFmt.posoff);
	u32 tmp_[4];
	float * tmpf_ =(float*)tmp_;

	memset(pos, 0, sizeof(float) * 3);

	for (int n = 0; n < morphcount; n++) {
		const u32_le *spos = (const u32_le*)(ptr_ + onesize_*n + posoff);

		for (int j = 0; j < 3; j++) {
			tmp_[j] = spos[j];
			pos[j] += tmpf_[j] * gstate_c.morphWeights[n];
		}
	}
#endif
}

static const StepFunction wtstep[4] = {
	0,
	&VertexDecoderDX9::Step_WeightsU8,
	&VertexDecoderDX9::Step_WeightsU16,
	&VertexDecoderDX9::Step_WeightsFloat,
};

static const StepFunction tcstep[4] = {
	0,
	&VertexDecoderDX9::Step_TcU8,
	&VertexDecoderDX9::Step_TcU16,
	&VertexDecoderDX9::Step_TcFloat,
};

static const StepFunction tcstep_prescale[4] = {
	0,
	&VertexDecoderDX9::Step_TcU8Prescale,
	&VertexDecoderDX9::Step_TcU16Prescale,
	&VertexDecoderDX9::Step_TcFloatPrescale,
};

static const StepFunction tcstep_through[4] = {
	0,
	&VertexDecoderDX9::Step_TcU8,
	&VertexDecoderDX9::Step_TcU16Through,
	&VertexDecoderDX9::Step_TcFloatThrough,
};

// Some HD Remaster games double the u16 texture coordinates.
static const StepFunction tcstep_Remaster[4] = {
	0,
	&VertexDecoderDX9::Step_TcU8,
	&VertexDecoderDX9::Step_TcU16Double,
	&VertexDecoderDX9::Step_TcFloat,
};

static const StepFunction tcstep_through_Remaster[4] = {
	0,
	&VertexDecoderDX9::Step_TcU8,
	&VertexDecoderDX9::Step_TcU16ThroughDouble,
	&VertexDecoderDX9::Step_TcFloatThrough,
};

// TODO: Tc Morph

static const StepFunction colstep[8] = {
	0, 0, 0, 0,
	&VertexDecoderDX9::Step_Color565,
	&VertexDecoderDX9::Step_Color5551,
	&VertexDecoderDX9::Step_Color4444,
	&VertexDecoderDX9::Step_Color8888,
};

static const StepFunction colstep_morph[8] = {
	0, 0, 0, 0,
	&VertexDecoderDX9::Step_Color565Morph,
	&VertexDecoderDX9::Step_Color5551Morph,
	&VertexDecoderDX9::Step_Color4444Morph,
	&VertexDecoderDX9::Step_Color8888Morph,
};

static const StepFunction nrmstep[4] = {
	0,
	&VertexDecoderDX9::Step_NormalS8,
	&VertexDecoderDX9::Step_NormalS16,
	&VertexDecoderDX9::Step_NormalFloat,
};

static const StepFunction nrmstep_morph[4] = {
	0,
	&VertexDecoderDX9::Step_NormalS8Morph,
	&VertexDecoderDX9::Step_NormalS16Morph,
	&VertexDecoderDX9::Step_NormalFloatMorph,
};

static const StepFunction posstep[4] = {
	0,
	&VertexDecoderDX9::Step_PosS8,
	&VertexDecoderDX9::Step_PosS16,
	&VertexDecoderDX9::Step_PosFloat,
};

static const StepFunction posstep_morph[4] = {
	0,
	&VertexDecoderDX9::Step_PosS8Morph,
	&VertexDecoderDX9::Step_PosS16Morph,
	&VertexDecoderDX9::Step_PosFloatMorph,
};

static const StepFunction posstep_through[4] = {
	0,
	&VertexDecoderDX9::Step_PosS8Through,
	&VertexDecoderDX9::Step_PosS16Through,
	&VertexDecoderDX9::Step_PosFloatThrough,
};


void VertexDecoderDX9::SetVertexType(u32 fmt) {
	fmt_ = fmt;
	throughmode = (fmt & GE_VTYPE_THROUGH) != 0;
	numSteps_ = 0;

	int biggest = 0;
	size = 0;

	tc = fmt & 0x3;
	col = (fmt >> 2) & 0x7;
	nrm = (fmt >> 5) & 0x3;
	pos = (fmt >> 7) & 0x3;
	weighttype = (fmt >> 9) & 0x3;
	idx = (fmt >> 11) & 0x3;
	morphcount = ((fmt >> 18) & 0x7)+1;
	nweights = ((fmt >> 14) & 0x7)+1;

	int decOff = 0;
	memset(&decFmt, 0, sizeof(decFmt));

	DEBUG_LOG(G3D,"VTYPE: THRU=%i TC=%i COL=%i POS=%i NRM=%i WT=%i NW=%i IDX=%i MC=%i", (int)throughmode, tc,col,pos,nrm,weighttype,nweights,idx,morphcount);

	if (weighttype) { // && nweights?
		//size = align(size, wtalign[weighttype]);	unnecessary
		size += wtsize[weighttype] * nweights;
		if (wtalign[weighttype] > biggest)
			biggest = wtalign[weighttype];

		steps_[numSteps_++] = wtstep[weighttype];

#ifndef USE_WEIGHT_HACK
		int fmtBase = DEC_FLOAT_1;
		if (weighttype == GE_VTYPE_WEIGHT_8BIT >> GE_VTYPE_WEIGHT_SHIFT) {
			fmtBase = DEC_U8_1;
		} else if (weighttype == GE_VTYPE_WEIGHT_16BIT >> GE_VTYPE_WEIGHT_SHIFT) {
			fmtBase = DEC_U16_1;
		} else if (weighttype == GE_VTYPE_WEIGHT_FLOAT >> GE_VTYPE_WEIGHT_SHIFT) {
			fmtBase = DEC_FLOAT_1;
		}
#else
		// Hack
		int fmtBase = DEC_FLOAT_1;
#endif

		int numWeights = TranslateNumBonesDX9(nweights);

		if (numWeights <= 4) {
			decFmt.w0off = decOff;
			decFmt.w0fmt = fmtBase + numWeights - 1;
			decOff += DecFmtSize(decFmt.w0fmt);
		} else {
			decFmt.w0off = decOff;
			decFmt.w0fmt = fmtBase + 3;
			decOff += DecFmtSize(decFmt.w0fmt);
			decFmt.w1off = decOff;
			decFmt.w1fmt = fmtBase + numWeights - 5;
			decOff += DecFmtSize(decFmt.w1fmt);
		}
	}

	if (tc) {
		size = align(size, tcalign[tc]);
		tcoff = size;
		size += tcsize[tc];
		if (tcalign[tc] > biggest)
			biggest = tcalign[tc];

		if (g_Config.bPrescaleUV && !throughmode && gstate.getTextureFunction() == 0) {
			steps_[numSteps_++] = tcstep_prescale[tc];
		} else {
			if (g_DoubleTextureCoordinates)
				steps_[numSteps_++] = throughmode ? tcstep_through_Remaster[tc] : tcstep_Remaster[tc];
			else
				steps_[numSteps_++] = throughmode ? tcstep_through[tc] : tcstep[tc];
		}

		decFmt.uvfmt = DEC_FLOAT_2;
		decFmt.uvoff = decOff;
		decOff += DecFmtSize(decFmt.uvfmt);
	}

	if (col) {
		size = align(size, colalign[col]);
		coloff = size;
		size += colsize[col];
		if (colalign[col] > biggest)
			biggest = colalign[col]; 

		steps_[numSteps_++] = morphcount == 1 ? colstep[col] : colstep_morph[col];

		// All color formats decode to DEC_U8_4 currently.
		// They can become floats later during transform though.
		decFmt.c0fmt = DEC_U8_4;
		decFmt.c0off = decOff;
		decOff += DecFmtSize(decFmt.c0fmt);
	} else {
		coloff = 0;
	}

	if (nrm) {
		size = align(size, nrmalign[nrm]);
		nrmoff = size;
		size += nrmsize[nrm];
		if (nrmalign[nrm] > biggest)
			biggest = nrmalign[nrm]; 

		steps_[numSteps_++] = morphcount == 1 ? nrmstep[nrm] : nrmstep_morph[nrm];

		if (morphcount == 1) {
			// The normal formats match the gl formats perfectly, let's use 'em.
			switch (nrm) {
			//case GE_VTYPE_NRM_8BIT >> GE_VTYPE_NRM_SHIFT: decFmt.nrmfmt = DEC_S8_3; break;
			case GE_VTYPE_NRM_8BIT >> GE_VTYPE_NRM_SHIFT: decFmt.nrmfmt = DEC_FLOAT_3; break;
			case GE_VTYPE_NRM_16BIT >> GE_VTYPE_NRM_SHIFT: decFmt.nrmfmt = DEC_S16_3; break;
			case GE_VTYPE_NRM_FLOAT >> GE_VTYPE_NRM_SHIFT: decFmt.nrmfmt = DEC_FLOAT_3; break;
			}
		} else {
			decFmt.nrmfmt = DEC_FLOAT_3;
		}

		// Actually, temporarily let's not.
		decFmt.nrmoff = decOff;
		decOff += DecFmtSize(decFmt.nrmfmt);
	}

	if (pos)  // there's always a position
	{
		size = align(size, posalign[pos]);
		posoff = size;
		size += possize[pos];
		if (posalign[pos] > biggest)
			biggest = posalign[pos];

		if (throughmode) {
			steps_[numSteps_++] = posstep_through[pos];
			decFmt.posfmt = DEC_FLOAT_3;
		} else {
			steps_[numSteps_++] = morphcount == 1 ? posstep[pos] : posstep_morph[pos];

			if (morphcount == 1) {
				// The non-through-mode position formats match the gl formats perfectly, let's use 'em.
				switch (pos) {
				//case GE_VTYPE_POS_8BIT >> GE_VTYPE_POS_SHIFT: decFmt.posfmt = DEC_S8_3; break;
				case GE_VTYPE_POS_8BIT >> GE_VTYPE_POS_SHIFT: decFmt.posfmt = DEC_FLOAT_3; break;
				case GE_VTYPE_POS_16BIT >> GE_VTYPE_POS_SHIFT: decFmt.posfmt = DEC_S16_3; break;
				case GE_VTYPE_POS_FLOAT >> GE_VTYPE_POS_SHIFT: decFmt.posfmt = DEC_FLOAT_3; break;
				}
			} else {
				// Actually, temporarily let's not.
				decFmt.posfmt = DEC_FLOAT_3;
			}
		}
		decFmt.posoff = decOff;
		decOff += DecFmtSize(decFmt.posfmt);
	} else {
		ERROR_LOG_REPORT(G3D, "Vertices without position found");
	}
		
	decFmt.stride = decOff;

	size = align(size, biggest);
	onesize_ = size;
	size *= morphcount;
	DEBUG_LOG(G3D,"SVT : size = %i, aligned to biggest %i", size, biggest);
}

void GetIndexBoundsDX9(void *inds, int count, u32 vertType, u16 *indexLowerBound, u16 *indexUpperBound) {
	// Find index bounds. Could cache this in display lists.
	// Also, this could be greatly sped up with SSE2/NEON, although rarely a bottleneck.
	int lowerBound = 0x7FFFFFFF;
	int upperBound = 0;
	u32 idx = vertType & GE_VTYPE_IDX_MASK;
	if (idx == GE_VTYPE_IDX_8BIT) {
		const u8 *ind8 = (const u8 *)inds;
		for (int i = 0; i < count; i++) {
			if (ind8[i] > upperBound)
				upperBound = ind8[i];
			if (ind8[i] < lowerBound)
				lowerBound = ind8[i];
		}
	} else if (idx == GE_VTYPE_IDX_16BIT) {
		const u16 *ind16 = (const u16*)inds;
		for (int i = 0; i < count; i++) {
			if (ind16[i] > upperBound)
				upperBound = ind16[i];
			if (ind16[i] < lowerBound)
				lowerBound = ind16[i];
		}
	} else {
		lowerBound = 0;
		upperBound = count - 1;
	}
	*indexLowerBound = (u16)lowerBound;
	*indexUpperBound = (u16)upperBound;
}

void VertexDecoderDX9::DecodeVerts(u8 *decodedptr, const void *verts, int indexLowerBound, int indexUpperBound) const {
	// Decode the vertices within the found bounds, once each
	// decoded_ and ptr_ are used in the steps, so can't be turned into locals for speed.
	decoded_ = decodedptr;
	ptr_ = (const u8*)verts + indexLowerBound * size;
	int stride = decFmt.stride;
	for (int index = indexLowerBound; index <= indexUpperBound; index++) {
		for (int i = 0; i < numSteps_; i++) {
			((*this).*steps_[i])();
		}
		ptr_ += size;
		decoded_ += stride;
	}
}

// TODO: Does not support morphs, skinning etc.
u32 VertexDecoderDX9::InjectUVs(u8 *decoded, const void *verts, float *customuv, int count) const {
	u32 customVertType = (gstate.vertType & ~GE_VTYPE_TC_MASK) | GE_VTYPE_TC_FLOAT;
	VertexDecoderDX9 decOut;
	decOut.SetVertexType(customVertType);
	
	const u8 *inp = (const u8 *)verts;
	u8 *out = decoded;
	for (int i = 0; i < count; i++) {
		if (pos) memcpy(out + decOut.posoff, inp + posoff, possize[pos]);
		if (nrm) memcpy(out + decOut.nrmoff, inp + nrmoff, nrmsize[nrm]);
		if (col) memcpy(out + decOut.coloff, inp + coloff, colsize[col]);
		// Ignore others for now, this is all we need for puzbob.
		// Inject!
		memcpy(out + decOut.tcoff, &customuv[i * 2], tcsize[decOut.tc]);
		inp += this->onesize_;
		out += decOut.onesize_;
	}
	return customVertType;
}

int VertexDecoderDX9::ToString(char *output) const {
	char * start = output;
	output += sprintf(output, "P: %i ", pos);
	if (nrm)
		output += sprintf(output, "N: %i ", nrm);
	if (col)
		output += sprintf(output, "C: %i ", col);
	if (tc)
		output += sprintf(output, "T: %i ", tc);
	if (weighttype)
		output += sprintf(output, "W: %i ", weighttype);
	if (idx)
		output += sprintf(output, "I: %i ", idx);
	if (morphcount > 1)
		output += sprintf(output, "Morph: %i ", morphcount);
	output += sprintf(output, "Verts: %i ", stats_[STAT_VERTSSUBMITTED]);
	if (throughmode)
		output += sprintf(output, " (through)");

	output += sprintf(output, " (size: %i)", VertexSize());
	return output - start;
}

}  // namespace DX9
