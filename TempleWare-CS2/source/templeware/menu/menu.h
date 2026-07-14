#pragma once
#include <Windows.h>
#include <d3d11.h>
#include "../../../external/imgui/imgui.h"
#include "../../../external/imgui/imgui_impl_dx11.h"
#include "../../../external/imgui/imgui_impl_win32.h"


// UC CS2 weapon icon font (private-use glyphs)
extern ImFont* g_WeaponIconFont;

class Menu {
public:
	Menu();

	void init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView);
	void render();

	void toggleMenu();
	bool isOpen() { return showMenu; }
private:
	bool showMenu = false;
	int activeTab = 0;
};
