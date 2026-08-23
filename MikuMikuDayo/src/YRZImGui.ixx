//よろずDXR⇔ImGuiブリッジ
//よろずDXRとImGuiを併用する時のための、ちょっとした関数群

//ImGuiは元々使うのが楽なので、それ以上劇的に楽にはなりませんけど、よろずDXRと併用する際に引っかかる所はある程度取り除けます


//使い方

//Dear ImGuiが必要です(masterブランチまたはdockingブランチ推奨)
//1. https://github.com/ocornut/imguiをクローン x:\imgui\にクローンしたとして以下、話を進めます
//2. ソリューションエクスプローラでプロジェクトにx:\imgui\*.cpp, *.hを追加
//3. 同様に、x:\imgui\backends\imgui_impl_win32.cpp, x:\imgui\backends\imgui_impl_win32.h, imgui_impl_dx12.cpp, imgui_impl_dx12.hも追加
//4. プロジェクトのプロパティで「C/C++→全般→追加のインクルードディレクトリ」に、x:\imguiを追加

//以下は、1度やればプロジェクト作成毎にやらなくてOKです。デバッグ時の補助になります
//5. マイドキュメント/Visual Studio 2022/Visualizers/ に x:\imgui\misc\debuggers\imgui.natvis, imgui.natstepfilterをコピー


module;


#include <format>
#include <functional>
#include <unordered_map>

#include <d3dx12.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>


/****************************************************************************
* 宣言
****************************************************************************/

export module YRZImGui;

import YRZ;

export namespace YRZ {
	//ImGui用SRVディスクリプタヒープの作成
	//nDescriptors:使用したいリソースの数。imguiで使用するリソースのため少なくとも2? ImGui::Image()などで画像を張りたい場合は画像の種類分足す
	//ディクリプタヒープは全部有効なリソースで埋められてなくても特にエラーは出ないのでちょっと余計に確保しといても良いでしょう
	//この辺はImGuiのバージョンによって変わる可能性があるのでとりあえずデフォルトは16としておく
	ComPtr<ID3D12DescriptorHeap>CreateImGuiDescriptorHeap(DXR& dxr, UINT nDescriptors = 16);

	//texをImGuiから使えるように、imguiDHに登録する
	//indexはimguiDH上のインデックス(ImGuiDHAlloc.FreeIndices.back()で取得する)
	//返り値のImTextureIDをImGui::Image()などに渡すべし
	ImTextureID ImGuiTextureID(DXR& dxr, Tex2D& tex, ComPtr<ID3D12DescriptorHeap> imguiDH, int index);

	//ImGui用のテクスチャとして登録しているテクスチャのステートをpixel shader resourceに変更する
	//RaytracingShader, ComputeShaderからアクセスされたりRenderTargetになる可能性のあるテクスチャの場合は必須
	//dxr.OpenCommandList()～dxr.ExecuteCommandList()の間でImGuiRender()の前に呼び出してね
	void ImGuiTextureReadyToRender(DXR& dxr, Tex2D& tex);
	
	//GUIをバックバッファに書き込むための一連のコマンドを発行する
	//dxr.OpenCommandList()～dxr.ExecuteCommandList()の間で呼び出してね
	void ImGuiRender(DXR& dxr, ComPtr<ID3D12DescriptorHeap> imguiDH);

	//https://github.com/ocornut/imgui/blob/master/examples/example_win32_directx12/main.cpp より引用
	// Simple free list based allocator
	struct ImGuiDHAllocator
	{
		ID3D12DescriptorHeap* Heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
		D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
		D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
		UINT                        HeapHandleIncrement;
		//ImGui側で確保されたdescriptorを除いた、空きdescriptor番号リスト。リストの後ろから使うべし
		std::vector<int>            FreeIndices;	

		//初期化。CreateImGuiDescriptorHeap()からで呼ばれる
		void Init(ID3D12Device* device, ID3D12DescriptorHeap* heap)
		{
			IM_ASSERT(Heap == nullptr && FreeIndices.empty());
			Heap = heap;
			D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
			HeapType = desc.Type;
			HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
			HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
			HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
			FreeIndices.reserve((int)desc.NumDescriptors);
			for (int n = desc.NumDescriptors; n > 0; n--)
				FreeIndices.push_back(n);
		}
		void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
		{
			IM_ASSERT(FreeIndices.size() > 0);
			int idx = FreeIndices.back();
			FreeIndices.pop_back();
			out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
			out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
		}
		void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
		{
			int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
			int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
			IM_ASSERT(cpu_idx == gpu_idx);
			FreeIndices.push_back(cpu_idx);
		}
	};

	//imGuiのSRVディスクリプタアロケータ、CreateImGuiDescriptorHeapを実行すると初期化される
	YRZ::ImGuiDHAllocator ImguiDHAlloc;	

	//ImGui初期化時に必要な情報の作成
	ImGui_ImplDX12_InitInfo ImGuiInfo(DXR& dxr, ComPtr<ID3D12DescriptorHeap> imguiDH);


	// 名前から ImGuiKey を取得（大文字小文字は区別しない）
	inline ImGuiKey StringToImGuiKey(const std::string& name)
	{
		static const std::unordered_map<std::string, ImGuiKey> ImGuiKeyMap = {
			{ "none",            ImGuiKey_None         },
			{ "tab",             ImGuiKey_Tab          },
			{ "leftarrow",       ImGuiKey_LeftArrow    },
			{ "rightarrow",      ImGuiKey_RightArrow   },
			{ "uparrow",         ImGuiKey_UpArrow      },
			{ "downarrow",       ImGuiKey_DownArrow    },
			{ "pageup",          ImGuiKey_PageUp       },
			{ "pagedown",        ImGuiKey_PageDown     },
			{ "home",            ImGuiKey_Home         },
			{ "end",             ImGuiKey_End          },
			{ "insert",          ImGuiKey_Insert       },
			{ "delete",          ImGuiKey_Delete       },
			{ "backspace",       ImGuiKey_Backspace    },
			{ "space",           ImGuiKey_Space        },
			{ "enter",           ImGuiKey_Enter        },
			{ "escape",          ImGuiKey_Escape       },
			{ "keypadenter",     ImGuiKey_KeypadEnter  },

			// 数字キー
			{ "0",               ImGuiKey_0            },
			{ "1",               ImGuiKey_1            },
			{ "2",               ImGuiKey_2            },
			{ "3",               ImGuiKey_3            },
			{ "4",               ImGuiKey_4            },
			{ "5",               ImGuiKey_5            },
			{ "6",               ImGuiKey_6            },
			{ "7",               ImGuiKey_7            },
			{ "8",               ImGuiKey_8            },
			{ "9",               ImGuiKey_9            },

			// アルファベット
			{ "a",               ImGuiKey_A            },
			{ "b",               ImGuiKey_B            },
			{ "c",               ImGuiKey_C            },
			{ "d",               ImGuiKey_D            },
			{ "e",               ImGuiKey_E            },
			{ "f",               ImGuiKey_F            },
			{ "g",               ImGuiKey_G            },
			{ "h",               ImGuiKey_H            },
			{ "i",               ImGuiKey_I            },
			{ "j",               ImGuiKey_J            },
			{ "k",               ImGuiKey_K            },
			{ "l",               ImGuiKey_L            },
			{ "m",               ImGuiKey_M            },
			{ "n",               ImGuiKey_N            },
			{ "o",               ImGuiKey_O            },
			{ "p",               ImGuiKey_P            },
			{ "q",               ImGuiKey_Q            },
			{ "r",               ImGuiKey_R            },
			{ "s",               ImGuiKey_S            },
			{ "t",               ImGuiKey_T            },
			{ "u",               ImGuiKey_U            },
			{ "v",               ImGuiKey_V            },
			{ "w",               ImGuiKey_W            },
			{ "x",               ImGuiKey_X            },
			{ "y",               ImGuiKey_Y            },
			{ "z",               ImGuiKey_Z            },

			// ファンクションキー
			{ "f1",              ImGuiKey_F1           },
			{ "f2",              ImGuiKey_F2           },
			{ "f3",              ImGuiKey_F3           },
			{ "f4",              ImGuiKey_F4           },
			{ "f5",              ImGuiKey_F5           },
			{ "f6",              ImGuiKey_F6           },
			{ "f7",              ImGuiKey_F7           },
			{ "f8",              ImGuiKey_F8           },
			{ "f9",              ImGuiKey_F9           },
			{ "f10",             ImGuiKey_F10          },
			{ "f11",             ImGuiKey_F11          },
			{ "f12",             ImGuiKey_F12          },

			// ロックキー／システム
			{ "capslock",        ImGuiKey_CapsLock     },
			{ "scrolllock",      ImGuiKey_ScrollLock   },
			{ "numlock",         ImGuiKey_NumLock      },
			{ "printscreen",     ImGuiKey_PrintScreen  },
			{ "pause",           ImGuiKey_Pause        },

			// テンキー
			{ "keypad0",         ImGuiKey_Keypad0      },
			{ "keypad1",         ImGuiKey_Keypad1      },
			{ "keypad2",         ImGuiKey_Keypad2      },
			{ "keypad3",         ImGuiKey_Keypad3      },
			{ "keypad4",         ImGuiKey_Keypad4      },
			{ "keypad5",         ImGuiKey_Keypad5      },
			{ "keypad6",         ImGuiKey_Keypad6      },
			{ "keypad7",         ImGuiKey_Keypad7      },
			{ "keypad8",         ImGuiKey_Keypad8      },
			{ "keypad9",         ImGuiKey_Keypad9      },
			{ "keypaddecimal",   ImGuiKey_KeypadDecimal},
			{ "keypaddivide",    ImGuiKey_KeypadDivide },
			{ "keypadmultiply",  ImGuiKey_KeypadMultiply},
			{ "keypadsubtract",  ImGuiKey_KeypadSubtract},
			{ "keypadadd",       ImGuiKey_KeypadAdd    },
			{ "keypadequal",     ImGuiKey_KeypadEqual  },

			// モディファイアキー
			{ "leftctrl",        ImGuiKey_LeftCtrl     },
			{ "leftshift",       ImGuiKey_LeftShift    },
			{ "leftalt",         ImGuiKey_LeftAlt      },
			{ "leftsuper",       ImGuiKey_LeftSuper    },
			{ "rightctrl",       ImGuiKey_RightCtrl    },
			{ "rightshift",      ImGuiKey_RightShift   },
			{ "rightalt",        ImGuiKey_RightAlt     },
			{ "rightsuper",      ImGuiKey_RightSuper   },
			{ "menu",            ImGuiKey_Menu         },
		};

		auto it = ImGuiKeyMap.find(LowerStr(name));
		return (it != ImGuiKeyMap.end()) ? it->second : ImGuiKey_None;
	}

	inline ImGuiKey StringToImGuiKeyMod(const std::string& name)
	{
		//modキー
		static const std::unordered_map<std::string, ImGuiKey> ImGuiKeyModMap = {
			{"none", ImGuiMod_None},
			{"ctrl", ImGuiMod_Ctrl},
			{"shift", ImGuiMod_Shift},
			{"alt", ImGuiMod_Alt},
			{"win", ImGuiMod_Super},
		};

		auto it = ImGuiKeyModMap.find(LowerStr(name));
		return (it != ImGuiKeyModMap.end()) ? it->second : ImGuiMod_None;
	}

	//文字列→プレフィックス+キー
	inline ImGuiKey StringToImGuiKeyCombo(const std::string& str)
	{
		auto v = SplitStr(str, '+', true);
		int key = 0;

		if (v.empty())
			return ImGuiKey_None;

		for (int i = 0; i < v.size() - 1; i++) {
			key |= StringToImGuiKeyMod(v[i]);
		}
		key |= StringToImGuiKey(v.back());

		return ImGuiKey(key);
	}
}


//ImGui用の数学関数お好みでどうぞ
export namespace YRZ::ImMath {
	inline float Dot(const ImVec2& a, const ImVec2& b) { return a.x * b.x + a.y * b.y; }
	inline float LenSq(const ImVec2& a) { return Dot(a, a); }
	inline float Length(const ImVec2& a) { return sqrtf(LenSq(a)); }
	inline ImVec2 Normalize(const ImVec2& a) { float lensq = LenSq(a);  return (lensq == 0) ? ImVec2(0, 0) : a / sqrtf(lensq); }
	inline ImVec2 Clamp(const ImVec2& a, const ImVec2& mi, const ImVec2& ma) { return ImVec2(min(max(mi.x, a.x), ma.x), min(max(mi.y, a.y), ma.y)); }
	inline bool PointInRect(const ImVec2& p, const ImVec2& topleft, const ImVec2& bottomright) { return Clamp(p, topleft, bottomright) == p; }
}

/*
inline ImVec2& operator+=(ImVec2& lhs, const ImVec2& rhs) { lhs.x += rhs.x; lhs.y += rhs.y; return lhs; }
inline ImVec2& operator-=(ImVec2& lhs, const ImVec2& rhs) { lhs.x -= rhs.x; lhs.y -= rhs.y; return lhs; }
inline ImVec2& operator*=(ImVec2& lhs, const ImVec2& rhs) { lhs.x *= rhs.x; lhs.y *= rhs.y; return lhs; }
inline ImVec2& operator/=(ImVec2& lhs, const ImVec2& rhs) { lhs.x /= rhs.x; lhs.y /= rhs.y; return lhs; }
inline ImVec2& operator*=(ImVec2& lhs, const float rhs) { lhs.x *= rhs; lhs.y *= rhs; return lhs; }
inline ImVec2& operator/=(ImVec2& lhs, const float rhs) { lhs.x /= rhs; lhs.y /= rhs; return lhs; }
inline ImVec2 operator+(ImVec2 lhs, const ImVec2& rhs) { lhs += rhs; return lhs; }
inline ImVec2 operator-(ImVec2 lhs, const ImVec2& rhs) { lhs -= rhs; return lhs; }
inline ImVec2 operator*(ImVec2 lhs, const ImVec2& rhs) { lhs *= rhs; return lhs; }
inline ImVec2 operator/(ImVec2 lhs, const ImVec2& rhs) { lhs /= rhs; return lhs; }
inline ImVec2 operator*(ImVec2 lhs, const float rhs) { lhs *= rhs; return lhs; }
inline ImVec2 operator/(ImVec2 lhs, const float rhs) { lhs /= rhs; return lhs; }
inline bool operator==(const ImVec2& lhs, const ImVec2& rhs) { return (lhs.x == rhs.x && lhs.y==rhs.y);  }
inline bool operator!=(const ImVec2& lhs, const ImVec2& rhs) { return !(lhs == rhs); }

inline ImVec4& operator+=(ImVec4& lhs, const ImVec4& rhs) { lhs.x += rhs.x; lhs.y += rhs.y; lhs.z += rhs.z; lhs.w += rhs.w; return lhs;}
inline ImVec4& operator-=(ImVec4& lhs, const ImVec4& rhs) { lhs.x -= rhs.x; lhs.y -= rhs.y; lhs.z -= rhs.z; lhs.w -= rhs.w; return lhs; }
inline ImVec4& operator*=(ImVec4& lhs, const ImVec4& rhs) { lhs.x *= rhs.x; lhs.y *= rhs.y; lhs.z *= rhs.z; lhs.w *= rhs.w; return lhs; }
inline ImVec4& operator/=(ImVec4& lhs, const ImVec4& rhs) { lhs.x /= rhs.x; lhs.y /= rhs.y; lhs.z /= rhs.z; lhs.w /= rhs.w; return lhs; }
inline ImVec4& operator*=(ImVec4& lhs, const float rhs) { lhs.x *= rhs; lhs.y *= rhs; lhs.z *= rhs; lhs.w *= rhs; return lhs; }
inline ImVec4& operator/=(ImVec4& lhs, const float  rhs) { lhs.x /= rhs; lhs.y /= rhs; lhs.z /= rhs; lhs.w /= rhs; return lhs; }
inline ImVec4 operator+(ImVec4 lhs, const ImVec4& rhs) { lhs += rhs; return lhs; }
inline ImVec4 operator-(ImVec4 lhs, const ImVec4& rhs) { lhs -= rhs; return lhs; }
inline ImVec4 operator*(ImVec4 lhs, const ImVec4& rhs) { lhs *= rhs; return lhs; }
inline ImVec4 operator/(ImVec4 lhs, const ImVec4& rhs) { lhs /= rhs; return lhs; }
inline ImVec4 operator*(ImVec4 lhs, const float rhs) { lhs *= rhs; return lhs; }
inline ImVec4 operator/(ImVec4 lhs, const float rhs) { lhs /= rhs; return lhs; }
inline bool operator==(const ImVec4& lhs, const ImVec4& rhs) { return (lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w); }
inline bool operator!=(const ImVec4& lhs, const ImVec4& rhs) { return !(lhs == rhs); }
*/



/****************************************************************************
* 実装
****************************************************************************/

module : private;


namespace YRZ {

	ComPtr<ID3D12DescriptorHeap>CreateImGuiDescriptorHeap(DXR& dxr, UINT nDescriptors)
	{
		ComPtr<ID3D12DescriptorHeap> imguiDH;
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = nDescriptors;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(dxr.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(imguiDH.ReleaseAndGetAddressOf())), L"CreateImGuiDescriptorHeap Failed!", __FILEW__, __LINE__);
		imguiDH->SetName(L"ImGui descriptor heap created by YRZImGui");
		//descriptorアロケータの初期化
		ImguiDHAlloc.Init(dxr.Device().Get(), imguiDH.Get());
		return imguiDH;
	}

	//texをImGuiから使えるようにする
	//indexはディクスリプタヒープ上のインデックス(0はフォントで予約されているので1からスタート)
	ImTextureID ImGuiTextureID(DXR& dxr, Tex2D& tex, ComPtr<ID3D12DescriptorHeap> imguiDH, int index)
	{
		if ((tex.caps & ResCaps::srv) == ResCaps::none)
			ThrowIfFailed(E_INVALIDARG, std::format(L"ImGuiTextureID() : texture {} can't be push into SRV descriptor heap", tex.Name()), __FILEW__, __LINE__);

		auto desc = tex.desc();
		auto srv_cpu_handle = imguiDH->GetCPUDescriptorHandleForHeapStart();
		auto inclement = dxr.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		srv_cpu_handle.ptr += inclement * index;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		srvDesc.Format = desc.Format;
		//デプスステンシルテクスチャだった場合、フォーマットを挿げ替える
		if (srvDesc.Format == DXGI_FORMAT_D32_FLOAT) {
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		} else if (srvDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
			srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		} else if (srvDesc.Format == DXGI_FORMAT_D16_UNORM) {
			srvDesc.Format = DXGI_FORMAT_R16_UNORM;
		}
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		dxr.Device()->CreateShaderResourceView(tex.res.Get(), &srvDesc, srv_cpu_handle);

		auto gpu_handle = imguiDH->GetGPUDescriptorHandleForHeapStart();
		gpu_handle.ptr += inclement * index;
		return (ImTextureID)gpu_handle.ptr;
	}

	//ImGui用のテクスチャとして登録しているテクスチャのリソースステートをpixel shader resourceに遷移させる
	void ImGuiTextureReadyToRender(DXR& dxr, Tex2D& tex)
	{
		//ピクセルシェーダリソースに遷移
		if (*tex.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
			auto bar = CD3DX12_RESOURCE_BARRIER::Transition(tex.res.Get(), *tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dxr.CommandList()->ResourceBarrier(1, &bar);
		}
		*tex.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}


	void ImGuiRender(DXR& dxr, ComPtr<ID3D12DescriptorHeap> imguiDH)
	{
		//imguiのディスクリプタヒープを選択する
		ID3D12DescriptorHeap* heaps[] = { imguiDH.Get() };
		dxr.CommandList()->SetDescriptorHeaps(1, heaps);

		//バックバッファに書き込めるように準備
		auto bar = CD3DX12_RESOURCE_BARRIER::Transition(dxr.CurrentBackBuffer().Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		dxr.CommandList()->ResourceBarrier(1, &bar);

		//レンダーターゲットをバックバッファにする
		auto rtvH = dxr.BackBufferCPUHandle(dxr.BackBufferIndex());
		dxr.CommandList()->OMSetRenderTargets(1, &rtvH, true, nullptr);

		//imguiのレンダリングコマンド発行
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dxr.CommandList().Get());

		//バックバッファを表示できるように準備
		bar = CD3DX12_RESOURCE_BARRIER::Transition(dxr.CurrentBackBuffer().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		dxr.CommandList()->ResourceBarrier(1, &bar);

	}


	ImGui_ImplDX12_InitInfo ImGuiInfo(DXR& dxr, ComPtr<ID3D12DescriptorHeap> imguiDH)
	{
		ImGui_ImplDX12_InitInfo info = {};
		info.CommandQueue = dxr.CommandQueue().Get();
		info.Device = dxr.Device().Get();
		info.RTVFormat = dxr.BackBufferFormat();
		info.NumFramesInFlight = dxr.BackBufferCount();
		info.DSVFormat = DXGI_FORMAT_UNKNOWN;

		// Allocating SRV descriptors (for textures) is up to the application, so we provide callbacks.
		// The example_win32_directx12/main.cpp application include a simple free-list based allocator.
		info.SrvDescriptorHeap = imguiDH.Get();
		info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) { return ImguiDHAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
		info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) { return ImguiDHAlloc.Free(cpu_handle, gpu_handle); };

		return info;
	}

}
