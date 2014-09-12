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
#include "Core/Host.h"
#include "Core/MemMap.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/Reporting.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"

#include "helper/dx_state.h"
#include "helper/fbo.h"

#include "GPU/Common/FramebufferCommon.h"
#include "GPU/Directx9/FramebufferDX9.h"
#include "GPU/Directx9/TextureCacheDX9.h"
#include "GPU/Directx9/ShaderManagerDX9.h"

#include <algorithm>

namespace DX9 {
	inline u16 RGBA8888toRGB565(u32 px) {
		return ((px >> 3) & 0x001F) | ((px >> 5) & 0x07E0) | ((px >> 8) & 0xF800);
	}

	inline u16 RGBA8888toRGBA4444(u32 px) {
		return ((px >> 4) & 0x000F) | ((px >> 8) & 0x00F0) | ((px >> 12) & 0x0F00) | ((px >> 16) & 0xF000);
	}

	inline u16 RGBA8888toRGBA5551(u32 px) {
		return ((px >> 3) & 0x001F) | ((px >> 6) & 0x03E0) | ((px >> 9) & 0x7C00) | ((px >> 16) & 0x8000);
	}

	static void ConvertFromRGBA8888(u8 *dst, u8 *src, u32 stride, u32 height, GEBufferFormat format);

	void CenterRect(float *x, float *y, float *w, float *h,
                float origW, float origH, float frameW, float frameH) {
		if (g_Config.bStretchToDisplay) {
				*x = 0;
				*y = 0;
				*w = frameW;
				*h = frameH;
				return;
		}

		float origRatio = origW/origH;
		float frameRatio = frameW/frameH;

		if (origRatio > frameRatio) {
				// Image is wider than frame. Center vertically.
				float scale = origW / frameW;
				*x = 0.0f;
				*w = frameW;
				*h = frameW / origRatio;
				// Stretch a little bit
				if (g_Config.bPartialStretch)
						*h = (frameH + *h) / 2.0f; // (408 + 720) / 2 = 564
				*y = (frameH - *h) / 2.0f;
		} else {
				// Image is taller than frame. Center horizontally.
				float scale = origH / frameH;
				*y = 0.0f;
				*h = frameH;
				*w = frameH * origRatio;
				*x = (frameW - *w) / 2.0f;
		}
	}

	void FramebufferManagerDX9::ClearBuffer() {
		dxstate.scissorTest.disable();
		dxstate.depthWrite.set(TRUE);
		dxstate.colorMask.set(true, true, true, true);
		dxstate.stencilFunc.set(D3DCMP_ALWAYS, 0, 0);
		dxstate.stencilMask.set(0xFF);
		pD3Ddevice->Clear(0, NULL, D3DCLEAR_STENCIL|D3DCLEAR_TARGET |D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 0, 0);
	}

	void FramebufferManagerDX9::ClearDepthBuffer() {
		dxstate.scissorTest.disable();
		dxstate.depthWrite.set(TRUE);
		dxstate.colorMask.set(false, false, false, false);
		dxstate.stencilFunc.set(D3DCMP_NEVER, 0, 0);
		pD3Ddevice->Clear(0, NULL, D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 0, 0);
	}

	void FramebufferManagerDX9::DisableState() {
		dxstate.blend.disable();
		dxstate.cullMode.set(false, false);
		dxstate.depthTest.disable();
		dxstate.scissorTest.disable();
		dxstate.stencilTest.disable();
		dxstate.colorMask.set(true, true, true, true);
		dxstate.stencilMask.set(0xFF);
	}


	FramebufferManagerDX9::FramebufferManagerDX9() :
		drawPixelsTex_(0),
		drawPixelsTexFormat_(GE_FORMAT_INVALID),
		convBuf(0)
	{
		// And an initial clear. We don't clear per frame as the games are supposed to handle that
		// by themselves.
		ClearBuffer();
		// TODO: Check / use D3DCAPS2_DYNAMICTEXTURES?
		int usage = 0;
		D3DPOOL pool = D3DPOOL_MANAGED;
		if (pD3DdeviceEx) {
			pool = D3DPOOL_DEFAULT;
			usage = D3DUSAGE_DYNAMIC;
		}
		HRESULT hr = pD3Ddevice->CreateTexture(512, 272, 1, usage, D3DFMT(D3DFMT_A8R8G8B8), pool, &drawPixelsTex_, NULL);
		if (FAILED(hr)) {
			drawPixelsTex_ = nullptr;
			ERROR_LOG(G3D, "Failed to create drawpixels texture");
		}
		BeginFrame();
	}

	FramebufferManagerDX9::~FramebufferManagerDX9() {
		if(drawPixelsTex_) {
			drawPixelsTex_->Release();
		}
		delete [] convBuf;
	}

	static inline void ARGB8From4444(u16 c, u32 * dst) {
		*dst = ((c & 0xf) << 4) | (((c >> 4) & 0xf) << 12) | (((c >> 8) & 0xf) << 20) | ((c >> 12) << 28);
	}
	static inline void ARGB8From565(u16 c, u32 * dst) {
		*dst = ((c & 0x001f) << 19) | (((c >> 5) & 0x003f) << 11) | ((((c >> 10) & 0x001f) << 3)) | 0xFF000000;
	}
	static inline void ARGB8From5551(u16 c, u32 * dst) {
		*dst = ((c & 0x001f) << 19) | (((c >> 5) & 0x001f) << 11) | ((((c >> 10) & 0x001f) << 3)) | 0xFF000000;
	}

	static inline u32 ABGR2RGBA(u32 src) {
		return (src >> 8) | (src << 24); 
	}

	void FramebufferManagerDX9::DrawPixels(const u8 *framebuf, GEBufferFormat pixelFormat, int linesize) {
		u8 * convBuf = NULL;
		D3DLOCKED_RECT rect;

		if (!drawPixelsTex_) {
			return;
		}

		drawPixelsTex_->LockRect(0, &rect, NULL, 0);

		convBuf = (u8*)rect.pBits;

		// Final format is ARGB(directx)

		// TODO: We can just change the texture format and flip some bits around instead of this.
		if (pixelFormat != GE_FORMAT_8888 || linesize != 512) {
			for (int y = 0; y < 272; y++) {
				switch (pixelFormat) {
					// not tested
				case GE_FORMAT_565:
					{
						const u16 *src = (const u16 *)framebuf + linesize * y;
						u32 *dst = (u32*)(convBuf + rect.Pitch * y);
						for (int x = 0; x < 480; x++) {
							u16_le col0 = src[x+0];
							ARGB8From565(col0, &dst[x + 0]);
						}
					}
					break;
					// faster
				case GE_FORMAT_5551:
					{
						const u16 *src = (const u16 *)framebuf + linesize * y;
						u32 *dst = (u32*)(convBuf + rect.Pitch * y);
						for (int x = 0; x < 480; x++) {
							u16_le col0 = src[x+0];
							ARGB8From5551(col0, &dst[x + 0]);
						}
					}
					break;
					// not tested
				case GE_FORMAT_4444:
					{
						const u16 *src = (const u16 *)framebuf + linesize * y;
						u32 *dst = (u32*)(convBuf + rect.Pitch * y);
						for (int x = 0; x < 480; x++)
						{
							u16_le col = src[x];
							dst[x * 4 + 0] = (col >> 12) << 4;
							dst[x * 4 + 1] = ((col >> 8) & 0xf) << 4;
							dst[x * 4 + 2] = ((col >> 4) & 0xf) << 4;
							dst[x * 4 + 3] = (col & 0xf) << 4;
						}
					}
					break;

				case GE_FORMAT_8888:
					{
						const u32 *src = (const u32 *)framebuf + linesize * y;
						u32 *dst = (u32*)(convBuf + rect.Pitch * y);
						for (int x = 0; x < 480; x++)
						{
							dst[x] = ABGR2RGBA(src[x]);
						}
					}
					break;
				}
			}
		} else {
			for (int y = 0; y < 272; y++) {
				const u32 *src = (const u32 *)framebuf + linesize * y;
				u32 *dst = (u32*)(convBuf + rect.Pitch * y);
				for (int x = 0; x < 512; x++)
				{
					dst[x] = ABGR2RGBA(src[x]);
				}
			}
		}

		drawPixelsTex_->UnlockRect(0);
		// D3DXSaveTextureToFile("game:\\cc.png", D3DXIFF_PNG, drawPixelsTex_, NULL);

		float x, y, w, h;
		CenterRect(&x, &y, &w, &h, 480.0f, 272.0f, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight);
		DrawActiveTexture(drawPixelsTex_, x, y, w, h, false, 480.0f / 512.0f);
	}

	// Depth in ogl is between -1;1 we need between 0;1
	static void ConvertMatrices(Matrix4x4 & in) {
		/*
		in.zz *= 0.5f;
		in.wz += 1.f;
		*/
		Matrix4x4 s;
		Matrix4x4 t;
		s.setScaling(Vec3(1, 1, 0.5f));
		t.setTranslation(Vec3(0, 0, 0.5f));
		in = in * s;
		in = in * t;
	}

	void FramebufferManagerDX9::DrawActiveTexture(LPDIRECT3DTEXTURE9 tex, float x, float y, float w, float h, float destW, float destH, bool flip, float uscale, float vscale) {
		float u2 = uscale;
		// Since we're flipping, 0 is down.  That's where the scale goes.
		float v1 = flip ? 1.0f : 1.0f - vscale;
		float v2 = flip ? 1.0f - vscale : 1.0f;

		float coord[] = { 
			x,	 y,	  0,	0,	v1,
			x+w, y,	  0,	u2, v1,
			x+w, y+h, 0,	u2, v2,
			x,	 y+h, 0,	0,	v2
		}; 

		for (int i = 0; i < 4; i++) {
			coord[i * 5] = coord[i * 5] / (destW * 0.5) - 1.0f;
			coord[i * 5 + 1] = -(coord[i * 5 + 1] / (destH * 0.5) - 1.0f);
		}

		//pD3Ddevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
		pD3Ddevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		pD3Ddevice->SetVertexDeclaration(pFramebufferVertexDecl);
		pD3Ddevice->SetPixelShader(pFramebufferPixelShader);
		pD3Ddevice->SetVertexShader(pFramebufferVertexShader);
		if (tex != NULL) {
			pD3Ddevice->SetTexture(0, tex);
		}
		pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, coord, 5 * sizeof(float));
	}

	void FramebufferManagerDX9::DestroyFramebuf(VirtualFramebuffer *v) {
		textureCache_->NotifyFramebuffer(v->fb_address, v, NOTIFY_FB_DESTROYED);
		if (v->fbo) {
			fbo_destroy(v->fbo);
			v->fbo = 0;
		}

		// Wipe some pointers
		if (currentRenderVfb_ == v)
			currentRenderVfb_ = 0;
		if (displayFramebuf_ == v)
			displayFramebuf_ = 0;
		if (prevDisplayFramebuf_ == v)
			prevDisplayFramebuf_ = 0;
		if (prevPrevDisplayFramebuf_ == v)
			prevPrevDisplayFramebuf_ = 0;

		delete v;
	}

	void FramebufferManagerDX9::ResizeFramebufFBO(VirtualFramebuffer *vfb, u16 w, u16 h, bool force) {
		float renderWidthFactor = (float)vfb->renderWidth / (float)vfb->bufferWidth;
		float renderHeightFactor = (float)vfb->renderHeight / (float)vfb->bufferHeight;
		VirtualFramebuffer old = *vfb;

		if (force) {
			vfb->bufferWidth = w;
			vfb->bufferHeight = h;
		} else {
			if (vfb->bufferWidth >= w && vfb->bufferHeight >= h) {
				return;
			}

			// In case it gets thin and wide, don't resize down either side.
			vfb->bufferWidth = std::max(vfb->bufferWidth, w);
			vfb->bufferHeight = std::max(vfb->bufferHeight, h);
		}

		vfb->renderWidth = vfb->bufferWidth * renderWidthFactor;
		vfb->renderHeight = vfb->bufferHeight * renderHeightFactor;

		bool trueColor = g_Config.bTrueColor;
		if (hackForce04154000Download_ && vfb->fb_address == 0x00154000) {
			trueColor = true;
		}

		if (trueColor) {
			vfb->colorDepth = FBO_8888;
		} else {
			switch (vfb->format) {
			case GE_FORMAT_4444:
				vfb->colorDepth = FBO_4444;
				break;
			case GE_FORMAT_5551:
				vfb->colorDepth = FBO_5551;
				break;
			case GE_FORMAT_565:
				vfb->colorDepth = FBO_565;
				break;
			case GE_FORMAT_8888:
			default:
				vfb->colorDepth = FBO_8888;
				break;
			}
		}

		textureCache_->ForgetLastTexture();
		fbo_unbind();

		if (!useBufferedRendering_) {
			if (vfb->fbo) {
				fbo_destroy(vfb->fbo);
				vfb->fbo = 0;
			}
			return;
		}

		vfb->fbo = fbo_create(vfb->renderWidth, vfb->renderHeight, 1, true, (FBOColorDepth)vfb->colorDepth);
		if (old.fbo) {
			INFO_LOG(SCEGE, "Resizing FBO for %08x : %i x %i x %i", vfb->fb_address, w, h, vfb->format);
			if (vfb->fbo) {
				ClearBuffer();
				if (!g_Config.bDisableSlowFramebufEffects) {
					// TODO
					//BlitFramebuffer_(vfb, 0, 0, &old, 0, 0, std::min(vfb->bufferWidth, vfb->width), std::min(vfb->height, vfb->bufferHeight), 0);
				}
			}
			fbo_destroy(old.fbo);
			if (vfb->fbo) {
				fbo_bind_as_render_target(vfb->fbo);
			}
		}

		if (!vfb->fbo) {
			ERROR_LOG(SCEGE, "Error creating FBO! %i x %i", vfb->renderWidth, vfb->renderHeight);
		}
	}

	void FramebufferManagerDX9::NotifyRenderFramebufferCreated(VirtualFramebuffer *vfb) {
		if (!useBufferedRendering_) {
			fbo_unbind();
			// Let's ignore rendering to targets that have not (yet) been displayed.
			gstate_c.skipDrawReason |= SKIPDRAW_NON_DISPLAYED_FB;
		}

		textureCache_->NotifyFramebuffer(vfb->fb_address, vfb, NOTIFY_FB_CREATED);

		ClearBuffer();

		// ugly...
		if (gstate_c.curRTWidth != vfb->width || gstate_c.curRTHeight != vfb->height) {
			shaderManager_->DirtyUniform(DIRTY_PROJTHROUGHMATRIX);
		}
	}

	void FramebufferManagerDX9::NotifyRenderFramebufferSwitched(VirtualFramebuffer *prevVfb, VirtualFramebuffer *vfb) {
		if (ShouldDownloadFramebuffer(vfb) && !vfb->memoryUpdated) {
			// TODO
			//ReadFramebufferToMemory(vfb, true, 0, 0, vfb->width, vfb->height);
		}
		textureCache_->ForgetLastTexture();

		if (useBufferedRendering_) {
			if (vfb->fbo) {
				fbo_bind_as_render_target(vfb->fbo);
			} else {
				// wtf? This should only happen very briefly when toggling bBufferedRendering
				fbo_unbind();
			}
		} else {
			if (vfb->fbo) {
				// wtf? This should only happen very briefly when toggling bBufferedRendering
				textureCache_->NotifyFramebuffer(vfb->fb_address, vfb, NOTIFY_FB_DESTROYED);
				fbo_destroy(vfb->fbo);
				vfb->fbo = 0;
			}
			fbo_unbind();

			// Let's ignore rendering to targets that have not (yet) been displayed.
			if (vfb->usageFlags & FB_USAGE_DISPLAYED_FRAMEBUFFER) {
				gstate_c.skipDrawReason &= ~SKIPDRAW_NON_DISPLAYED_FB;
			} else {
				gstate_c.skipDrawReason |= SKIPDRAW_NON_DISPLAYED_FB;
			}
		}
		textureCache_->NotifyFramebuffer(vfb->fb_address, vfb, NOTIFY_FB_UPDATED);

		// Copy depth pixel value from the read framebuffer to the draw framebuffer
		if (prevVfb && !g_Config.bDisableSlowFramebufEffects) {
			// TODO
			//BlitFramebufferDepth(prevVfb, vfb);
		}
		if (vfb->drawnFormat != vfb->format) {
			// TODO: Might ultimately combine this with the resize step in DoSetRenderFrameBuffer().
			// TODO
			//ReformatFramebufferFrom(vfb, vfb->drawnFormat);
		}

		// ugly...
		if (gstate_c.curRTWidth != vfb->width || gstate_c.curRTHeight != vfb->height) {
			shaderManager_->DirtyUniform(DIRTY_PROJTHROUGHMATRIX);
		}
	}

	void FramebufferManagerDX9::NotifyRenderFramebufferUpdated(VirtualFramebuffer *vfb, bool vfbFormatChanged) {
		if (vfbFormatChanged) {
			textureCache_->NotifyFramebuffer(vfb->fb_address, vfb, NOTIFY_FB_UPDATED);
			if (vfb->drawnFormat != vfb->format) {
				// TODO
				//ReformatFramebufferFrom(vfb, vfb->drawnFormat);
			}
		}

		// ugly...
		if (gstate_c.curRTWidth != vfb->width || gstate_c.curRTHeight != vfb->height) {
			shaderManager_->DirtyUniform(DIRTY_PROJTHROUGHMATRIX);
		}
	}

	void FramebufferManagerDX9::CopyDisplayToOutput() {

		fbo_unbind();
		dxstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
		currentRenderVfb_ = 0;

		VirtualFramebuffer *vfb = GetVFBAt(displayFramebufPtr_);
		if (!vfb) {
			if (Memory::IsValidAddress(displayFramebufPtr_)) {
				// The game is displaying something directly from RAM. In GTA, it's decoded video.

				// First check that it's not a known RAM copy of a VRAM framebuffer though, as in MotoGP
				for (auto iter = knownFramebufferCopies_.begin(); iter != knownFramebufferCopies_.end(); ++iter) {
					if (iter->second == displayFramebufPtr_) {
						vfb = GetVFBAt(iter->first);
					}
				}

				if (!vfb) {
					// Just a pointer to plain memory to draw. Draw it.
					DrawPixels(Memory::GetPointer(displayFramebufPtr_), displayFormat_, displayStride_);
					return;
				}
			} else {
				DEBUG_LOG(SCEGE, "Found no FBO to display! displayFBPtr = %08x", displayFramebufPtr_);
				// No framebuffer to display! Clear to black.
				ClearBuffer();
				return;
			}
		}

		vfb->usageFlags |= FB_USAGE_DISPLAYED_FRAMEBUFFER;
		vfb->dirtyAfterDisplay = false;
		vfb->reallyDirtyAfterDisplay = false;

		if (prevDisplayFramebuf_ != displayFramebuf_) {
			prevPrevDisplayFramebuf_ = prevDisplayFramebuf_;
		}
		if (displayFramebuf_ != vfb) {
			prevDisplayFramebuf_ = displayFramebuf_;
		}
		displayFramebuf_ = vfb;

		if (resized_) {
			ClearBuffer();
		}

		if (vfb->fbo) {
			DEBUG_LOG(SCEGE, "Displaying FBO %08x", vfb->fb_address);
			DisableState();
			LPDIRECT3DTEXTURE9 colorTexture = fbo_get_color_texture(vfb->fbo);

			// Output coordinates
			float x, y, w, h;
			CenterRect(&x, &y, &w, &h, 480.0f, 272.0f, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight);

			// TODO ES3: Use glInvalidateFramebuffer to discard depth/stencil data at the end of frame.
			// and to discard extraFBOs_ after using them.

			if (1) {
				dxstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
				// These are in the output display coordinates
				if (g_Config.iBufFilter == SCALE_LINEAR) {
					dxstate.texMagFilter.set(D3DTEXF_LINEAR);
					dxstate.texMinFilter.set(D3DTEXF_LINEAR);
				} else {
					dxstate.texMagFilter.set(D3DTEXF_POINT);
					dxstate.texMinFilter.set(D3DTEXF_POINT);
				}
				dxstate.texMipFilter.set(D3DTEXF_NONE);
				dxstate.texMipLodBias.set(0);
				DrawActiveTexture(colorTexture, x, y, w, h, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight, false, 480.0f / (float)vfb->width, 272.0f / (float)vfb->height);
			}
			/* 
			else if (usePostShader_ && extraFBOs_.size() == 1 && !postShaderAtOutputResolution_) {
			// An additional pass, post-processing shader to the extra FBO.
			fbo_bind_as_render_target(extraFBOs_[0]);
			int fbo_w, fbo_h;
			fbo_get_dimensions(extraFBOs_[0], &fbo_w, &fbo_h);
			glstate.viewport.set(0, 0, fbo_w, fbo_h);
			DrawActiveTexture(colorTexture, 0, 0, fbo_w, fbo_h, fbo_w, fbo_h, true, 1.0f, 1.0f, postShaderProgram_);

			fbo_unbind();

			// Use the extra FBO, with applied post-processing shader, as a texture.
			// fbo_bind_color_as_texture(extraFBOs_[0], 0);
			if (extraFBOs_.size() == 0) {
			ERROR_LOG(G3D, "WTF?");
			return;
			}
			colorTexture = fbo_get_color_texture(extraFBOs_[0]);
			glstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
			// These are in the output display coordinates
			DrawActiveTexture(colorTexture, x, y, w, h, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight, true, 480.0f / (float)vfb->width, 272.0f / (float)vfb->height);
			} else {
			// Use post-shader, but run shader at output resolution.
			glstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
			// These are in the output display coordinates
			DrawActiveTexture(colorTexture, x, y, w, h, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight, true, 480.0f / (float)vfb->width, 272.0f / (float)vfb->height, postShaderProgram_);
			}
			*/
			pD3Ddevice->SetTexture(0, NULL);
		}
	}

	void FramebufferManagerDX9::ReadFramebufferToMemory(VirtualFramebuffer *vfb, bool sync) {
#if 0
		if (sync) {
			PackFramebufferAsync_(NULL); // flush async just in case when we go for synchronous update
		}
#endif

		if(vfb) {
			// We'll pseudo-blit framebuffers here to get a resized and flipped version of vfb.
			// For now we'll keep these on the same struct as the ones that can get displayed
			// (and blatantly copy work already done above while at it).
			VirtualFramebuffer *nvfb = 0;

			// We maintain a separate vector of framebuffer objects for blitting.
			for (size_t i = 0; i < bvfbs_.size(); ++i) {
				VirtualFramebuffer *v = bvfbs_[i];
				if (MaskedEqual(v->fb_address, vfb->fb_address) && v->format == vfb->format) {
					if (v->bufferWidth == vfb->bufferWidth && v->bufferHeight == vfb->bufferHeight) {
						nvfb = v;
						v->fb_stride = vfb->fb_stride;
						v->width = vfb->width;
						v->height = vfb->height;
						break;
					}
				}
			}

			// Create a new fbo if none was found for the size
			if(!nvfb) {
				nvfb = new VirtualFramebuffer();
				nvfb->fbo = 0;
				nvfb->fb_address = vfb->fb_address;
				nvfb->fb_stride = vfb->fb_stride;
				nvfb->z_address = vfb->z_address;
				nvfb->z_stride = vfb->z_stride;
				nvfb->width = vfb->width;
				nvfb->height = vfb->height;
				nvfb->renderWidth = vfb->width;
				nvfb->renderHeight = vfb->height;
				nvfb->bufferWidth = vfb->bufferWidth;
				nvfb->bufferHeight = vfb->bufferHeight;
				nvfb->format = vfb->format;
				nvfb->usageFlags = FB_USAGE_RENDERTARGET;
				nvfb->dirtyAfterDisplay = true;

				// When updating VRAM, it need to be exact format.
				switch (vfb->format) {
				case GE_FORMAT_4444:
					nvfb->colorDepth = FBO_4444;
					break;
				case GE_FORMAT_5551:
					nvfb->colorDepth = FBO_5551;
					break;
				case GE_FORMAT_565:
					nvfb->colorDepth = FBO_565;
					break;
				case GE_FORMAT_8888:
				default:
					nvfb->colorDepth = FBO_8888;
					break;
				}

				nvfb->fbo = fbo_create(nvfb->width, nvfb->height, 1, true, (FBOColorDepth)nvfb->colorDepth);
				if (!(nvfb->fbo)) {
					ERROR_LOG(SCEGE, "Error creating FBO! %i x %i", nvfb->renderWidth, nvfb->renderHeight);
					return;
				}

				nvfb->last_frame_render = gpuStats.numFlips;
				bvfbs_.push_back(nvfb);
				fbo_bind_as_render_target(nvfb->fbo); 
				ClearBuffer();
			} else {
				nvfb->usageFlags |= FB_USAGE_RENDERTARGET;
				gstate_c.textureChanged = true;
				nvfb->last_frame_render = gpuStats.numFlips;
				nvfb->dirtyAfterDisplay = true;

#if 0
				if (nvfb->fbo) {
					fbo_bind_as_render_target(nvfb->fbo);
				}

				// Some tiled mobile GPUs benefit IMMENSELY from clearing an FBO before rendering
				// to it. This broke stuff before, so now it only clears on the first use of an
				// FBO in a frame. This means that some games won't be able to avoid the on-some-GPUs
				// performance-crushing framebuffer reloads from RAM, but we'll have to live with that.
				if (nvfb->last_frame_render != gpuStats.numFlips)	{
					ClearBuffer();
				}
#endif
			}

			vfb->memoryUpdated = true;
			BlitFramebuffer_(vfb, nvfb, false);

#if 0
#ifdef USING_GLES2
			PackFramebufferSync_(nvfb); // synchronous glReadPixels
#else
			if (gl_extensions.PBO_ARB || !gl_extensions.ATIClampBug) {
				if (!sync) {
					PackFramebufferAsync_(nvfb); // asynchronous glReadPixels using PBOs
				} else {
					PackFramebufferSync_(nvfb); // synchronous glReadPixels
				}
			}
#endif
#endif
		}
	}

	void FramebufferManagerDX9::BlitFramebuffer_(VirtualFramebuffer *src, VirtualFramebuffer *dst, bool flip, float upscale, float vscale) {
		if (dst->fbo) {
			fbo_bind_as_render_target(dst->fbo);
		} else {
			ERROR_LOG_REPORT_ONCE(dstfbozero, SCEGE, "BlitFramebuffer_: dst->fbo == 0");
			fbo_unbind();
			return;
		}

		/*
		if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		ERROR_LOG(HLE, "Incomplete target framebuffer, aborting blit");
		fbo_unbind();
		return;
		}
		*/

		dxstate.viewport.set(0, 0, dst->width, dst->height);
		DisableState();

		if (src->fbo) {
			fbo_bind_color_as_texture(src->fbo, 0);
		} else {
			ERROR_LOG_REPORT_ONCE(srcfbozero, SCEGE, "BlitFramebuffer_: src->fbo == 0");
			fbo_unbind();
			return;
		}

		float x, y, w, h;
		CenterRect(&x, &y, &w, &h, 480.0f, 272.0f, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight);

		DrawActiveTexture(0, x, y, w, h, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight, flip, upscale, vscale);

		pD3Ddevice->SetTexture(0, NULL);

		fbo_unbind();
	}

	// TODO: SSE/NEON
	// Could also make C fake-simd for 64-bit, two 8888 pixels fit in a register :)
	void ConvertFromRGBA8888(u8 *dst, u8 *src, u32 stride, u32 height, GEBufferFormat format) {
		if(format == GE_FORMAT_8888) {
			if(src == dst) {
				return;
			} else { // Here lets assume they don't intersect
				memcpy(dst, src, stride * height * 4);
			}
		} else { // But here it shouldn't matter if they do
			int size = height * stride;
			const u32 *src32 = (const u32 *)src;
			u16 *dst16 = (u16 *)dst;
			switch (format) {
			case GE_FORMAT_565: // BGR 565
				for(int i = 0; i < size; i++) {
					dst16[i] = RGBA8888toRGB565(src32[i]);
				}
				break;
			case GE_FORMAT_5551: // ABGR 1555
				for(int i = 0; i < size; i++) {
					dst16[i] = RGBA8888toRGBA5551(src32[i]);
				}
				break;
			case GE_FORMAT_4444: // ABGR 4444
				for(int i = 0; i < size; i++) {
					dst16[i] = RGBA8888toRGBA4444(src32[i]);
				}
				break;
			case GE_FORMAT_8888:
				// Not possible.
				break;
			default:
				break;
			}
		}
	}

	void FramebufferManagerDX9::PackFramebufferDirectx9_(VirtualFramebuffer *vfb) {
		if (vfb->fbo) {
			fbo_bind_for_read(vfb->fbo);
		} else {
			ERROR_LOG_REPORT_ONCE(vfbfbozero, SCEGE, "PackFramebufferSync_: vfb->fbo == 0");
			fbo_unbind();
			return;
		}

		// Pixel size always 4 here because we always request RGBA8888
		size_t bufSize = vfb->fb_stride * vfb->height * 4;
		u32 fb_address = (0x04000000) | vfb->fb_address;

		u8 *packed = 0;
		if(vfb->format == GE_FORMAT_8888) {
			packed = (u8 *)Memory::GetPointer(fb_address);
		} else { // End result may be 16-bit but we are reading 32-bit, so there may not be enough space at fb_address
			packed = (u8 *)malloc(bufSize * sizeof(u8));
		}

		if(packed) {
			DEBUG_LOG(HLE, "Reading framebuffer to mem, bufSize = %u, packed = %p, fb_address = %08x", 
				(u32)bufSize, packed, fb_address);

			// Resolve(packed, vfb);

			if(vfb->format != GE_FORMAT_8888) { // If not RGBA 8888 we need to convert
				ConvertFromRGBA8888(Memory::GetPointer(fb_address), packed, vfb->fb_stride, vfb->height, vfb->format);
				free(packed);
			}
		}

		fbo_unbind();
	}
	void FramebufferManagerDX9::EndFrame() {
		if (resized_) {
			DestroyAllFBOs();
			dxstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
			resized_ = false;
		}
#if 0
		// We flush to memory last requested framebuffer, if any
		PackFramebufferAsync_(NULL);
#endif
	}

	void FramebufferManagerDX9::DeviceLost() {
		DestroyAllFBOs();
		resized_ = false;
	}

	std::vector<FramebufferInfo> FramebufferManagerDX9::GetFramebufferList() {
		std::vector<FramebufferInfo> list;

		for (size_t i = 0; i < vfbs_.size(); ++i) {
			VirtualFramebuffer *vfb = vfbs_[i];

			FramebufferInfo info;
			info.fb_address = vfb->fb_address;
			info.z_address = vfb->z_address;
			info.format = vfb->format;
			info.width = vfb->width;
			info.height = vfb->height;
			info.fbo = vfb->fbo;
			list.push_back(info);
		}

		return list;
	}

	// MotoGP workaround
	bool FramebufferManagerDX9::NotifyFramebufferCopy(u32 src, u32 dest, int size, bool isMemset) {
		for (size_t i = 0; i < vfbs_.size(); i++) {
			// This size fits for MotoGP. Might want to make this more flexible for other games if they do the same.
			if ((vfbs_[i]->fb_address | 0x04000000) == src && size == 512 * 272 * 2) {
				// A framebuffer matched!
				knownFramebufferCopies_.insert(std::pair<u32, u32>(src, dest));
			}
		}
		// TODO
		return false;
	}

	bool FramebufferManagerDX9::NotifyStencilUpload(u32 addr, int size, bool skipZero) {
		// TODO
		return false;
	}

	void FramebufferManagerDX9::DecimateFBOs() {
		fbo_unbind();
		currentRenderVfb_ = 0;
		bool updateVram = !(g_Config.iRenderingMode == FB_NON_BUFFERED_MODE || g_Config.iRenderingMode == FB_BUFFERED_MODE);

		for (size_t i = 0; i < vfbs_.size(); ++i) {
			VirtualFramebuffer *vfb = vfbs_[i];
			int age = frameLastFramebufUsed_ - std::max(vfb->last_frame_render, vfb->last_frame_used);

			if (updateVram && age == 0 && !vfb->memoryUpdated && vfb == displayFramebuf_) 
				ReadFramebufferToMemory(vfb);

			if (vfb == displayFramebuf_ || vfb == prevDisplayFramebuf_ || vfb == prevPrevDisplayFramebuf_) {
				continue;
			}

			if (age > FBO_OLD_AGE) {
				INFO_LOG(SCEGE, "Decimating FBO for %08x (%i x %i x %i), age %i", vfb->fb_address, vfb->width, vfb->height, vfb->format, age);
				DestroyFramebuf(vfb);
				vfbs_.erase(vfbs_.begin() + i--);
			}
		}

		// Do the same for ReadFramebuffersToMemory's VFBs
		for (size_t i = 0; i < bvfbs_.size(); ++i) {
			VirtualFramebuffer *vfb = bvfbs_[i];
			int age = frameLastFramebufUsed_ - vfb->last_frame_render;
			if (age > FBO_OLD_AGE) {
				INFO_LOG(SCEGE, "Decimating FBO for %08x (%i x %i x %i), age %i", vfb->fb_address, vfb->width, vfb->height, vfb->format, age);
				DestroyFramebuf(vfb);
				bvfbs_.erase(bvfbs_.begin() + i--);
			}
		}
	}

	void FramebufferManagerDX9::DestroyAllFBOs() {
		fbo_unbind();
		currentRenderVfb_ = 0;
		displayFramebuf_ = 0;
		prevDisplayFramebuf_ = 0;
		prevPrevDisplayFramebuf_ = 0;

		for (size_t i = 0; i < vfbs_.size(); ++i) {
			VirtualFramebuffer *vfb = vfbs_[i];
			INFO_LOG(SCEGE, "Destroying FBO for %08x : %i x %i x %i", vfb->fb_address, vfb->width, vfb->height, vfb->format);
			DestroyFramebuf(vfb);
		}
		vfbs_.clear();
	}

	void FramebufferManagerDX9::UpdateFromMemory(u32 addr, int size, bool safe) {
		addr &= ~0x40000000;
		// TODO: Could go through all FBOs, but probably not important?
		// TODO: Could also check for inner changes, but video is most important.
		if (addr == DisplayFramebufAddr() || addr == PrevDisplayFramebufAddr() || safe) {
			// TODO: Deleting the FBO is a heavy hammer solution, so let's only do it if it'd help.
			if (!Memory::IsValidAddress(displayFramebufPtr_))
				return;

			fbo_unbind();
			currentRenderVfb_ = 0;

			bool needUnbind = false;
			for (size_t i = 0; i < vfbs_.size(); ++i) {
				VirtualFramebuffer *vfb = vfbs_[i];
				if (MaskedEqual(vfb->fb_address, addr)) {
					vfb->dirtyAfterDisplay = true;
					vfb->reallyDirtyAfterDisplay = true;
					// TODO: This without the fbo_unbind() above would be better than destroying the FBO.
					// However, it doesn't seem to work for Star Ocean, at least
					if (useBufferedRendering_ && vfb->fbo) {
						fbo_bind_as_render_target(vfb->fbo);
						needUnbind = true;
						DrawPixels(Memory::GetPointer(addr | 0x04000000), vfb->format, vfb->fb_stride);
					} else {
						INFO_LOG(SCEGE, "Invalidating FBO for %08x (%i x %i x %i)", vfb->fb_address, vfb->width, vfb->height, vfb->format);
						DestroyFramebuf(vfb);
						vfbs_.erase(vfbs_.begin() + i--);
					}
				}
			}

			if (needUnbind)
				fbo_unbind();
		}
	}

	void FramebufferManagerDX9::Resized() {
		resized_ = true;
	}


	bool FramebufferManagerDX9::GetCurrentFramebuffer(GPUDebugBuffer &buffer) {
		u32 fb_address = gstate.getFrameBufRawAddress();
		int fb_stride = gstate.FrameBufStride();

		VirtualFramebuffer *vfb = currentRenderVfb_;
		if (!vfb) {
			vfb = GetVFBAt(fb_address);
		}

		if (!vfb) {
			// If there's no vfb and we're drawing there, must be memory?
			buffer = GPUDebugBuffer(Memory::GetPointer(fb_address | 0x04000000), fb_stride, 512, gstate.FrameBufFormat());
			return true;
		}

		LPDIRECT3DSURFACE9 renderTarget = nullptr;
		HRESULT hr;
		hr = pD3Ddevice->GetRenderTarget(0, &renderTarget);
		if (!renderTarget || !SUCCEEDED(hr))
			return false;

		D3DSURFACE_DESC desc;
		renderTarget->GetDesc(&desc);

		LPDIRECT3DSURFACE9 offscreen = nullptr;
		hr = pD3Ddevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &offscreen, NULL);
		if (!offscreen || !SUCCEEDED(hr)) {
			renderTarget->Release();
			return false;
		}

		bool success = false;
		hr = pD3Ddevice->GetRenderTargetData(renderTarget, offscreen);
		if (SUCCEEDED(hr)) {
			D3DLOCKED_RECT locked;
			RECT rect = {0, 0, vfb->renderWidth, vfb->renderHeight};
			hr = offscreen->LockRect(&locked, &rect, D3DLOCK_READONLY);
			if (SUCCEEDED(hr)) {
				// TODO: Handle the other formats?  We don't currently create them, I think.
				buffer.Allocate(locked.Pitch / 4, desc.Height, GPU_DBG_FORMAT_8888_BGRA, false);
				memcpy(buffer.GetData(), locked.pBits, locked.Pitch * desc.Height);
				offscreen->UnlockRect();
				success = true;
			}
		}

		offscreen->Release();
		renderTarget->Release();

		return success;
	}

	bool FramebufferManagerDX9::GetCurrentDepthbuffer(GPUDebugBuffer &buffer) {
		// TODO: Is this possible?
		return false;
	}

	bool FramebufferManagerDX9::GetCurrentStencilbuffer(GPUDebugBuffer &buffer) {
		// TODO: Is this possible?
		return false;
	}

}  // namespace DX9
