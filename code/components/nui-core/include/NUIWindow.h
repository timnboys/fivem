/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#pragma once

#include <queue>

#include <include/cef_client.h>
#include <include/cef_v8.h>

enum NUIPaintType
{
	NUIPaintTypeDummy,
	NUIPaintTypePostRender
};

#include "grcTexture.h"

class
#ifdef COMPILING_NUI_CORE
	__declspec(dllexport)
#endif
	NUIWindow : public fwRefCountable
{
private:
	CefRefPtr<CefClient> m_client;

	void(__cdecl* m_onClientCreated)(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);

	void Initialize(CefString url);

public:
	NUIWindow(bool primary, int width, int height);

private:
	bool m_primary;
	int m_width;
	int m_height;

	int m_roundedWidth;
	int m_roundedHeight;

	unsigned long m_dirtyFlag;
	RECT m_lastDirtyRect;
	CRITICAL_SECTION m_renderBufferLock;
	char* m_renderBuffer;

	std::queue<CefRect> m_dirtyRects;

	std::set<std::string> m_pollQueue;

	rage::grcTexture* m_nuiTexture;

	NUIPaintType m_paintType;

	uint64_t m_syncKey;

	ID3D11Texture2D* m_parentTexture;

	ID3D11Texture2D* m_swapTexture;

	ID3D11RenderTargetView* m_swapRtv;

	ID3D11ShaderResourceView* m_swapSrv;

	HANDLE m_lastParentHandle;

public:
	inline int		GetWidth() { return m_width; }
	inline int		GetHeight() { return m_height; }

	inline void*	GetRenderBuffer() { return m_renderBuffer; }
	inline int		GetRoundedWidth() { return m_roundedWidth; }

public:
	void			AddDirtyRect(const CefRect& rect);

	inline void		MarkRenderBufferDirty() { InterlockedIncrement(&m_dirtyFlag); }

public:
	static fwRefContainer<NUIWindow> Create(bool primary, int width, int height, CefString url);

public:
	~NUIWindow();

	void UpdateFrame();

	void Invalidate();

	void SetPaintType(NUIPaintType type);

	CefBrowser* GetBrowser();

	void SignalPoll(std::string& argument);

	void UpdateSharedResource(void* sharedHandle, uint64_t syncKey, const CefRenderHandler::RectList& rects);

	inline void SetClientContextCreated(void(__cdecl* cb)(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context))
	{
		m_onClientCreated = cb;
	}

	inline void OnClientContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
	{
		if (m_onClientCreated)
		{
			m_onClientCreated(browser, frame, context);
		}
	}

	inline rage::grcTexture* GetTexture() { return m_nuiTexture; }

	inline NUIPaintType GetPaintType() { return m_paintType; }

	inline ID3D11Texture2D* GetParentTexture()
	{
		return m_parentTexture;
	}

	inline void SetParentTexture(ID3D11Texture2D* texture)
	{
		m_parentTexture = texture;
	}
};
