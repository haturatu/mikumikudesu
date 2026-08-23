module;
//よろずFX
//DXSASを目指した何か
//MMXXIV,MMXXV,MMXXVI (c) SANDMAN presents!!
// 
// このモジュール内で定義されるクラス・構造体に格納されているオブジェクトの文字列エンコードについて
//
// std::stringの場合UTF-8 (ほとんどの文字列。JSONで読み書きする必要のある物、シェーダのソースコードなど)
// std::wstringの場合はUTF-16 LE (OS,DirectX側に渡す必要のあるファイル名や識別子など)
//
// ※SJISなどいわゆるWindows方言でのANSIエンコードは一切使いません
//   UTF-8で指定しているが受け入れ側がラテン文字と数字しか許容しないので結果的にANSIと同じ、というケースはあるかもしれない
//
// よろずDXRではstd::wstringをベースに文字列を扱うが、よろずFXではstd::stringの使用割合が高い
// JSONをcerealで扱う都合上そうなった
// 

/**
* @brief よろずFX
* @details よろずDXRのためのエフェクトフレームワーク
* @author SANDMAN(github:pennennennennennenem, X:@NenemSdmn)
*/

#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <dxcapi.h>
#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <libjsonnet++.h>


#pragma comment(lib, "winmm.lib")

export module YRZFx;

import YRZ;
import Expr;

//省略可能メンバ
#define CEREOPT(var)  try{ A(cereal::make_nvp(#var,var)); } catch(...){}

//省略可能メンバ、2引数版
#define CEREOPT2(name,var)  try{ A(cereal::make_nvp(name,var)); } catch(...){}


//ハマった話…非侵入型シリアライザの定義は、型と同じ名前空間でしなければならない
/*
template <class Archive>
void serialize(Archive& A, D3D12_STATIC_SAMPLER_DESC& desc) noexcept {
	CEREOPT2("Filter", desc.Filter)
	CEREOPT2("AddressU", desc.AddressU)
	CEREOPT2("AddressV", desc.AddressV)
	CEREOPT2("AddressW", desc.AddressW)
	CEREOPT2("MipLODBias", desc.MipLODBias)
	CEREOPT2("MaxAnisotropy", desc.MaxAnisotropy)
	CEREOPT2("ComparisonFunc", desc.ComparisonFunc)
	CEREOPT2("BorderColor", desc.BorderColor)
	CEREOPT2("MinLOD", desc.MinLOD)
	CEREOPT2("MaxLOD", desc.MaxLOD)
	CEREOPT2("ShaderRegister", desc.ShaderRegister)
	CEREOPT2("RegisterSpace", desc.RegisterSpace)
	CEREOPT2("ShaderVisibility", desc.ShaderVisibility)
}
template <class Archive>
void serialize(Archive& A, D3D12_RENDER_TARGET_BLEND_DESC& desc) {
	CEREOPT2("BlendEnable", desc.BlendEnable)
	CEREOPT2("LogicOpEnable", desc.LogicOpEnable)
	CEREOPT2("SrcBlend", desc.SrcBlend)
	CEREOPT2("DestBlend", desc.DestBlend)
	CEREOPT2("BlendOp", desc.BlendOp)
	CEREOPT2("SrcBlendAlpha", desc.SrcBlendAlpha)
	CEREOPT2("DestBlendAlpha", desc.DestBlendAlpha)
	CEREOPT2("BlendOpAlpha", desc.BlendOpAlpha)
	CEREOPT2("LogicOp", desc.LogicOp)
	CEREOPT2("RenderTargetWriteMask", desc.RenderTargetWriteMask)
}

template <class Archive>
void serialize(Archive& A, D3D12_BLEND_DESC& desc) {
	CEREOPT2("AlphaToCoverageEnable",desc.AlphaToCoverageEnable)
	CEREOPT2("IndependentBlendEnable",desc.IndependentBlendEnable)
	CEREOPT2("RenderTarget0",desc.RenderTarget[0])
	CEREOPT2("RenderTarget1",desc.RenderTarget[1])
	CEREOPT2("RenderTarget2",desc.RenderTarget[2])
	CEREOPT2("RenderTarget3",desc.RenderTarget[3])
	CEREOPT2("RenderTarget4",desc.RenderTarget[4])
	CEREOPT2("RenderTarget5",desc.RenderTarget[5])
	CEREOPT2("RenderTarget6",desc.RenderTarget[6])
	CEREOPT2("RenderTarget7",desc.RenderTarget[7])
}

template <class Archive>
void serialize(Archive& A, D3D12_RASTERIZER_DESC& desc) {
	CEREOPT2("FillMode",desc.FillMode)
	CEREOPT2("CullMode",desc.CullMode)
	CEREOPT2("FrontCounterClockwise",desc.FrontCounterClockwise)
	CEREOPT2("DepthBias",desc.DepthBias)
	CEREOPT2("DepthBiasClamp",desc.DepthBiasClamp)
	CEREOPT2("SlopeScaledDepthBias",desc.SlopeScaledDepthBias)
	CEREOPT2("DepthClipEnable",desc.DepthClipEnable)
	CEREOPT2("MultisampleEnable",desc.MultisampleEnable)
	CEREOPT2("AntialiasedLineEnable",desc.AntialiasedLineEnable)
	CEREOPT2("ForcedSampleCount",desc.ForcedSampleCount)
	CEREOPT2("ConservativeRaster",desc.ConservativeRaster)
}

template <class Archive>
void serialize(Archive& A, D3D12_DEPTH_STENCIL_DESC& desc) {
	CEREOPT2("DepthEnable",desc.DepthEnable)
	CEREOPT2("DepthWriteMask",desc.DepthWriteMask)
	CEREOPT2("DepthFunc",desc.DepthFunc)
	CEREOPT2("StencilEnable",desc.StencilEnable)
	CEREOPT2("StencilReadMask",desc.StencilReadMask)
	CEREOPT2("StencilWriteMask",desc.StencilWriteMask)
	CEREOPT2("StencilFailOp",desc.FrontFace.StencilFailOp)
	CEREOPT2("StencilDepthFailOp",desc.FrontFace.StencilDepthFailOp)
	CEREOPT2("StencilPassOp",desc.FrontFace.StencilPassOp)
	CEREOPT2("StencilFunc",desc.FrontFace.StencilFunc)
	CEREOPT2("StencilFailOp",desc.BackFace.StencilFailOp)
	CEREOPT2("StencilDepthFailOp",desc.BackFace.StencilDepthFailOp)
	CEREOPT2("StencilPassOp",desc.BackFace.StencilPassOp)
	CEREOPT2("StencilFunc",desc.BackFace.StencilFunc)
}
*/

namespace DirectX {
	template <class Archive>
	void serialize(Archive& A, XMFLOAT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMFLOAT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMFLOAT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }

	template <class Archive>
	void serialize(Archive& A, XMUINT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMUINT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMUINT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }

	template <class Archive>
	void serialize(Archive& A, XMINT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMINT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMINT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }
}



export namespace YRZ {

	using namespace DirectX;
	namespace fs = std::filesystem;

	//辞書mapと文字列strと見つからなかった場合のデフォルト値defを渡して、文字列→列挙型の変換
	//JSONに格納された文字列から各列挙型への変換に用いる
	template<typename T>
	T StringToEnumX(const std::unordered_map<std::string, T>& map, const std::string& str, T def) {
		auto s = str;
		std::transform(str.begin(), str.end(), s.begin(), [](char c) { return std::toupper(c); });
		auto it = map.find(s);
		if (str.empty()) {
			return def;
		} else if (it != map.end()) {
			return it->second;
		} else if (std::all_of(s.cbegin(), s.cend(), isdigit)) {
			int idx = std::stoi(s);
			return T(idx);
		} else {
			std::string name = typeid(T).name();
			ThrowIfFailed(E_INVALIDARG, std::format(L"{} is not valid for {}", L(str), L(name)), __FILEW__, __LINE__);
			return def;
		}

	};

	DXGI_FORMAT StringToDXGIFormat(const std::string& str) {
		static const std::unordered_map<std::string, DXGI_FORMAT> formatMap = {
			{"UNKNOWN", DXGI_FORMAT_UNKNOWN},
			{"R32G32B32A32_TYPELESS", DXGI_FORMAT_R32G32B32A32_TYPELESS},
			{"R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT},
			{"R32G32B32A32_UINT", DXGI_FORMAT_R32G32B32A32_UINT},
			{"R32G32B32A32_SINT", DXGI_FORMAT_R32G32B32A32_SINT},
			{"R32G32B32_TYPELESS", DXGI_FORMAT_R32G32B32_TYPELESS},
			{"R32G32B32_FLOAT", DXGI_FORMAT_R32G32B32_FLOAT},
			{"R32G32B32_UINT", DXGI_FORMAT_R32G32B32_UINT},
			{"R32G32B32_SINT", DXGI_FORMAT_R32G32B32_SINT},
			{"R16G16B16A16_TYPELESS", DXGI_FORMAT_R16G16B16A16_TYPELESS},
			{"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT},
			{"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM},
			{"R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT},
			{"R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM},
			{"R16G16B16A16_SINT", DXGI_FORMAT_R16G16B16A16_SINT},
			{"R32G32_TYPELESS", DXGI_FORMAT_R32G32_TYPELESS},
			{"R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT},
			{"R32G32_UINT", DXGI_FORMAT_R32G32_UINT},
			{"R32G32_SINT", DXGI_FORMAT_R32G32_SINT},
			{"R32G8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS},
			{"D32_FLOAT_S8X24_UINT", DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
			{"R32_FLOAT_X8X24_TYPELESS", DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
			{"X32_TYPELESS_G8X24_UINT", DXGI_FORMAT_X32_TYPELESS_G8X24_UINT},
			{"R10G10B10A2_TYPELESS", DXGI_FORMAT_R10G10B10A2_TYPELESS},
			{"R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM},
			{"R10G10B10A2_UINT", DXGI_FORMAT_R10G10B10A2_UINT},
			{"R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT},
			{"R8G8B8A8_TYPELESS", DXGI_FORMAT_R8G8B8A8_TYPELESS},
			{"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
			{"R8G8B8A8_UNORM_SRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
			{"R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT},
			{"R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM},
			{"R8G8B8A8_SINT", DXGI_FORMAT_R8G8B8A8_SINT},
			{"R16G16_TYPELESS", DXGI_FORMAT_R16G16_TYPELESS},
			{"R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT},
			{"R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM},
			{"R16G16_UINT", DXGI_FORMAT_R16G16_UINT},
			{"R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM},
			{"R16G16_SINT", DXGI_FORMAT_R16G16_SINT},
			{"R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS},
			{"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
			{"R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
			{"R32_UINT", DXGI_FORMAT_R32_UINT},
			{"R32_SINT", DXGI_FORMAT_R32_SINT},
			{"R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS},
			{"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT},
			{"R24_UNORM_X8_TYPELESS", DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
			{"X24_TYPELESS_G8_UINT", DXGI_FORMAT_X24_TYPELESS_G8_UINT},
			{"R8G8_TYPELESS", DXGI_FORMAT_R8G8_TYPELESS},
			{"R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM},
			{"R8G8_UINT", DXGI_FORMAT_R8G8_UINT},
			{"R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM},
			{"R8G8_SINT", DXGI_FORMAT_R8G8_SINT},
			{"R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS},
			{"R16_FLOAT", DXGI_FORMAT_R16_FLOAT},
			{"D16_UNORM", DXGI_FORMAT_D16_UNORM},
			{"R16_UNORM", DXGI_FORMAT_R16_UNORM},
			{"R16_UINT", DXGI_FORMAT_R16_UINT},
			{"R16_SNORM", DXGI_FORMAT_R16_SNORM},
			{"R16_SINT", DXGI_FORMAT_R16_SINT},
			{"R8_TYPELESS", DXGI_FORMAT_R8_TYPELESS},
			{"R8_UNORM", DXGI_FORMAT_R8_UNORM},
			{"R8_UINT", DXGI_FORMAT_R8_UINT},
			{"R8_SNORM", DXGI_FORMAT_R8_SNORM},
			{"R8_SINT", DXGI_FORMAT_R8_SINT},
			{"A8_UNORM", DXGI_FORMAT_A8_UNORM},
			{"R1_UNORM", DXGI_FORMAT_R1_UNORM},
			{"R9G9B9E5_SHAREDEXP", DXGI_FORMAT_R9G9B9E5_SHAREDEXP},
			{"R8G8_B8G8_UNORM", DXGI_FORMAT_R8G8_B8G8_UNORM},
			{"G8R8_G8B8_UNORM", DXGI_FORMAT_G8R8_G8B8_UNORM},
			{"BC1_TYPELESS", DXGI_FORMAT_BC1_TYPELESS},
			{"BC1_UNORM", DXGI_FORMAT_BC1_UNORM},
			{"BC1_UNORM_SRGB", DXGI_FORMAT_BC1_UNORM_SRGB},
			{"BC2_TYPELESS", DXGI_FORMAT_BC2_TYPELESS},
			{"BC2_UNORM", DXGI_FORMAT_BC2_UNORM},
			{"BC2_UNORM_SRGB", DXGI_FORMAT_BC2_UNORM_SRGB},
			{"BC3_TYPELESS", DXGI_FORMAT_BC3_TYPELESS},
			{"BC3_UNORM", DXGI_FORMAT_BC3_UNORM},
			{"BC3_UNORM_SRGB", DXGI_FORMAT_BC3_UNORM_SRGB},
			{"BC4_TYPELESS", DXGI_FORMAT_BC4_TYPELESS},
			{"BC4_UNORM", DXGI_FORMAT_BC4_UNORM},
			{"BC4_SNORM", DXGI_FORMAT_BC4_SNORM},
			{"BC5_TYPELESS", DXGI_FORMAT_BC5_TYPELESS},
			{"BC5_UNORM", DXGI_FORMAT_BC5_UNORM},
			{"BC5_SNORM", DXGI_FORMAT_BC5_SNORM},
			{"B5G6R5_UNORM", DXGI_FORMAT_B5G6R5_UNORM},
			{"B5G5R5A1_UNORM", DXGI_FORMAT_B5G5R5A1_UNORM},
			{"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM},
			{"B8G8R8X8_UNORM", DXGI_FORMAT_B8G8R8X8_UNORM},
			{"R10G10B10_XR_BIAS_A2_UNORM", DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM},
			{"B8G8R8A8_TYPELESS", DXGI_FORMAT_B8G8R8A8_TYPELESS},
			{"B8G8R8A8_UNORM_SRGB", DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
			{"B8G8R8X8_TYPELESS", DXGI_FORMAT_B8G8R8X8_TYPELESS},
			{"B8G8R8X8_UNORM_SRGB", DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
			{"BC6H_TYPELESS", DXGI_FORMAT_BC6H_TYPELESS},
			{"BC6H_UF16", DXGI_FORMAT_BC6H_UF16},
			{"BC6H_SF16", DXGI_FORMAT_BC6H_SF16},
			{"BC7_TYPELESS", DXGI_FORMAT_BC7_TYPELESS},
			{"BC7_UNORM", DXGI_FORMAT_BC7_UNORM},
			{"BC7_UNORM_SRGB", DXGI_FORMAT_BC7_UNORM_SRGB},
			{"AYUV", DXGI_FORMAT_AYUV},
			{"Y410", DXGI_FORMAT_Y410},
			{"Y416", DXGI_FORMAT_Y416},
			{"NV12", DXGI_FORMAT_NV12},
			{"P010", DXGI_FORMAT_P010},
			{"P016", DXGI_FORMAT_P016},
			{"OPAQUE_420", DXGI_FORMAT_420_OPAQUE},
			{"YUY2", DXGI_FORMAT_YUY2},
			{"Y210", DXGI_FORMAT_Y210},
			{"Y216", DXGI_FORMAT_Y216},
			{"NV11", DXGI_FORMAT_NV11},
			{"AI44", DXGI_FORMAT_AI44},
			{"IA44", DXGI_FORMAT_IA44},
			{"P8", DXGI_FORMAT_P8},
			{"A8P8", DXGI_FORMAT_A8P8},
			{"B4G4R4A4_UNORM", DXGI_FORMAT_B4G4R4A4_UNORM},
			{"P208", DXGI_FORMAT_P208},
			{"V208", DXGI_FORMAT_V208},
			{"V408", DXGI_FORMAT_V408},
			{"FORCE_UINT", DXGI_FORMAT_FORCE_UINT}
		};

		//DXGI_FORMAT_で始まっている場合、取り除く
		auto s = str;
		std::transform(str.begin(), str.end(), s.begin(), [](char c) { return std::toupper(c); });
		if (s.starts_with("DXGI_FORMAT_")) {
			s = s.substr(12);
		}
		return StringToEnumX(formatMap, s, DXGI_FORMAT_UNKNOWN);
	}

	std::string DXGIFormatToU8(DXGI_FORMAT fmt)
	{
		switch (fmt) {
		case DXGI_FORMAT_UNKNOWN: return "DXGI_FORMAT_UNKNOWN";
		case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "DXGI_FORMAT_R32G32B32A32_TYPELESS";
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return "DXGI_FORMAT_R32G32B32A32_FLOAT";
		case DXGI_FORMAT_R32G32B32A32_UINT: return "DXGI_FORMAT_R32G32B32A32_UINT";
		case DXGI_FORMAT_R32G32B32A32_SINT: return "DXGI_FORMAT_R32G32B32A32_SINT";
		case DXGI_FORMAT_R32G32B32_TYPELESS: return "DXGI_FORMAT_R32G32B32_TYPELESS";
		case DXGI_FORMAT_R32G32B32_FLOAT: return "DXGI_FORMAT_R32G32B32_FLOAT";
		case DXGI_FORMAT_R32G32B32_UINT: return "DXGI_FORMAT_R32G32B32_UINT";
		case DXGI_FORMAT_R32G32B32_SINT: return "DXGI_FORMAT_R32G32B32_SINT";
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "DXGI_FORMAT_R16G16B16A16_TYPELESS";
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
		case DXGI_FORMAT_R16G16B16A16_UNORM: return "DXGI_FORMAT_R16G16B16A16_UNORM";
		case DXGI_FORMAT_R16G16B16A16_UINT: return "DXGI_FORMAT_R16G16B16A16_UINT";
		case DXGI_FORMAT_R16G16B16A16_SNORM: return "DXGI_FORMAT_R16G16B16A16_SNORM";
		case DXGI_FORMAT_R16G16B16A16_SINT: return "DXGI_FORMAT_R16G16B16A16_SINT";
		case DXGI_FORMAT_R32G32_TYPELESS: return "DXGI_FORMAT_R32G32_TYPELESS";
		case DXGI_FORMAT_R32G32_FLOAT: return "DXGI_FORMAT_R32G32_FLOAT";
		case DXGI_FORMAT_R32G32_UINT: return "DXGI_FORMAT_R32G32_UINT";
		case DXGI_FORMAT_R32G32_SINT: return "DXGI_FORMAT_R32G32_SINT";
		case DXGI_FORMAT_R32G8X24_TYPELESS: return "DXGI_FORMAT_R32G8X24_TYPELESS";
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "DXGI_FORMAT_D32_FLOAT_S8X24_UINT";
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS";
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT";
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "DXGI_FORMAT_R10G10B10A2_TYPELESS";
		case DXGI_FORMAT_R10G10B10A2_UNORM: return "DXGI_FORMAT_R10G10B10A2_UNORM";
		case DXGI_FORMAT_R10G10B10A2_UINT: return "DXGI_FORMAT_R10G10B10A2_UINT";
		case DXGI_FORMAT_R11G11B10_FLOAT: return "DXGI_FORMAT_R11G11B10_FLOAT";
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "DXGI_FORMAT_R8G8B8A8_TYPELESS";
		case DXGI_FORMAT_R8G8B8A8_UNORM: return "DXGI_FORMAT_R8G8B8A8_UNORM";
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
		case DXGI_FORMAT_R8G8B8A8_UINT: return "DXGI_FORMAT_R8G8B8A8_UINT";
		case DXGI_FORMAT_R8G8B8A8_SNORM: return "DXGI_FORMAT_R8G8B8A8_SNORM";
		case DXGI_FORMAT_R8G8B8A8_SINT: return "DXGI_FORMAT_R8G8B8A8_SINT";
		case DXGI_FORMAT_R16G16_TYPELESS: return "DXGI_FORMAT_R16G16_TYPELESS";
		case DXGI_FORMAT_R16G16_FLOAT: return "DXGI_FORMAT_R16G16_FLOAT";
		case DXGI_FORMAT_R16G16_UNORM: return "DXGI_FORMAT_R16G16_UNORM";
		case DXGI_FORMAT_R16G16_UINT: return "DXGI_FORMAT_R16G16_UINT";
		case DXGI_FORMAT_R16G16_SNORM: return "DXGI_FORMAT_R16G16_SNORM";
		case DXGI_FORMAT_R16G16_SINT: return "DXGI_FORMAT_R16G16_SINT";
		case DXGI_FORMAT_R32_TYPELESS: return "DXGI_FORMAT_R32_TYPELESS";
		case DXGI_FORMAT_D32_FLOAT: return "DXGI_FORMAT_D32_FLOAT";
		case DXGI_FORMAT_R32_FLOAT: return "DXGI_FORMAT_R32_FLOAT";
		case DXGI_FORMAT_R32_UINT: return "DXGI_FORMAT_R32_UINT";
		case DXGI_FORMAT_R32_SINT: return "DXGI_FORMAT_R32_SINT";
		case DXGI_FORMAT_R24G8_TYPELESS: return "DXGI_FORMAT_R24G8_TYPELESS";
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return "DXGI_FORMAT_D24_UNORM_S8_UINT";
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "DXGI_FORMAT_R24_UNORM_X8_TYPELESS";
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return "DXGI_FORMAT_X24_TYPELESS_G8_UINT";
		case DXGI_FORMAT_R8G8_TYPELESS: return "DXGI_FORMAT_R8G8_TYPELESS";
		case DXGI_FORMAT_R8G8_UNORM: return "DXGI_FORMAT_R8G8_UNORM";
		case DXGI_FORMAT_R8G8_UINT: return "DXGI_FORMAT_R8G8_UINT";
		case DXGI_FORMAT_R8G8_SNORM: return "DXGI_FORMAT_R8G8_SNORM";
		case DXGI_FORMAT_R8G8_SINT: return "DXGI_FORMAT_R8G8_SINT";
		case DXGI_FORMAT_R16_TYPELESS: return "DXGI_FORMAT_R16_TYPELESS";
		case DXGI_FORMAT_R16_FLOAT: return "DXGI_FORMAT_R16_FLOAT";
		case DXGI_FORMAT_D16_UNORM: return "DXGI_FORMAT_D16_UNORM";
		case DXGI_FORMAT_R16_UNORM: return "DXGI_FORMAT_R16_UNORM";
		case DXGI_FORMAT_R16_UINT: return "DXGI_FORMAT_R16_UINT";
		case DXGI_FORMAT_R16_SNORM: return "DXGI_FORMAT_R16_SNORM";
		case DXGI_FORMAT_R16_SINT: return "DXGI_FORMAT_R16_SINT";
		case DXGI_FORMAT_R8_TYPELESS: return "DXGI_FORMAT_R8_TYPELESS";
		case DXGI_FORMAT_R8_UNORM: return "DXGI_FORMAT_R8_UNORM";
		case DXGI_FORMAT_R8_UINT: return "DXGI_FORMAT_R8_UINT";
		case DXGI_FORMAT_R8_SNORM: return "DXGI_FORMAT_R8_SNORM";
		case DXGI_FORMAT_R8_SINT: return "DXGI_FORMAT_R8_SINT";
		case DXGI_FORMAT_A8_UNORM: return "DXGI_FORMAT_A8_UNORM";
		case DXGI_FORMAT_R1_UNORM: return "DXGI_FORMAT_R1_UNORM";
		case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return "DXGI_FORMAT_R9G9B9E5_SHAREDEXP";
		case DXGI_FORMAT_R8G8_B8G8_UNORM: return "DXGI_FORMAT_R8G8_B8G8_UNORM";
		case DXGI_FORMAT_G8R8_G8B8_UNORM: return "DXGI_FORMAT_G8R8_G8B8_UNORM";
		case DXGI_FORMAT_BC1_TYPELESS: return "DXGI_FORMAT_BC1_TYPELESS";
		case DXGI_FORMAT_BC1_UNORM: return "DXGI_FORMAT_BC1_UNORM";
		case DXGI_FORMAT_BC1_UNORM_SRGB: return "DXGI_FORMAT_BC1_UNORM_SRGB";
		case DXGI_FORMAT_BC2_TYPELESS: return "DXGI_FORMAT_BC2_TYPELESS";
		case DXGI_FORMAT_BC2_UNORM: return "DXGI_FORMAT_BC2_UNORM";
		case DXGI_FORMAT_BC2_UNORM_SRGB: return "DXGI_FORMAT_BC2_UNORM_SRGB";
		case DXGI_FORMAT_BC3_TYPELESS: return "DXGI_FORMAT_BC3_TYPELESS";
		case DXGI_FORMAT_BC3_UNORM: return "DXGI_FORMAT_BC3_UNORM";
		case DXGI_FORMAT_BC3_UNORM_SRGB: return "DXGI_FORMAT_BC3_UNORM_SRGB";
		case DXGI_FORMAT_BC4_TYPELESS: return "DXGI_FORMAT_BC4_TYPELESS";
		case DXGI_FORMAT_BC4_UNORM: return "DXGI_FORMAT_BC4_UNORM";
		case DXGI_FORMAT_BC4_SNORM: return "DXGI_FORMAT_BC4_SNORM";
		case DXGI_FORMAT_BC5_TYPELESS: return "DXGI_FORMAT_BC5_TYPELESS";
		case DXGI_FORMAT_BC5_UNORM: return "DXGI_FORMAT_BC5_UNORM";
		case DXGI_FORMAT_BC5_SNORM: return "DXGI_FORMAT_BC5_SNORM";
		case DXGI_FORMAT_B5G6R5_UNORM: return "DXGI_FORMAT_B5G6R5_UNORM";
		case DXGI_FORMAT_B5G5R5A1_UNORM: return "DXGI_FORMAT_B5G5R5A1_UNORM";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return "DXGI_FORMAT_B8G8R8A8_UNORM";
		case DXGI_FORMAT_B8G8R8X8_UNORM: return "DXGI_FORMAT_B8G8R8X8_UNORM";
		case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM";
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "DXGI_FORMAT_B8G8R8A8_TYPELESS";
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: return "DXGI_FORMAT_B8G8R8X8_TYPELESS";
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB";
		case DXGI_FORMAT_BC6H_TYPELESS: return "DXGI_FORMAT_BC6H_TYPELESS";
		case DXGI_FORMAT_BC6H_UF16: return "DXGI_FORMAT_BC6H_UF16";
		case DXGI_FORMAT_BC6H_SF16: return "DXGI_FORMAT_BC6H_SF16";
		case DXGI_FORMAT_BC7_TYPELESS: return "DXGI_FORMAT_BC7_TYPELESS";
		case DXGI_FORMAT_BC7_UNORM: return "DXGI_FORMAT_BC7_UNORM";
		case DXGI_FORMAT_BC7_UNORM_SRGB: return "DXGI_FORMAT_BC7_UNORM_SRGB";
		case DXGI_FORMAT_AYUV: return "DXGI_FORMAT_AYUV";
		case DXGI_FORMAT_Y410: return "DXGI_FORMAT_Y410";
		case DXGI_FORMAT_Y416: return "DXGI_FORMAT_Y416";
		case DXGI_FORMAT_NV12: return "DXGI_FORMAT_NV12";
		case DXGI_FORMAT_P010: return "DXGI_FORMAT_P010";
		case DXGI_FORMAT_P016: return "DXGI_FORMAT_P016";
		case DXGI_FORMAT_420_OPAQUE: return "DXGI_FORMAT_420_OPAQUE";
		case DXGI_FORMAT_YUY2: return "DXGI_FORMAT_YUY2";
		case DXGI_FORMAT_Y210: return "DXGI_FORMAT_Y210";
		case DXGI_FORMAT_Y216: return "DXGI_FORMAT_Y216";
		case DXGI_FORMAT_NV11: return "DXGI_FORMAT_NV11";
		case DXGI_FORMAT_AI44: return "DXGI_FORMAT_AI44";
		case DXGI_FORMAT_IA44: return "DXGI_FORMAT_IA44";
		case DXGI_FORMAT_P8: return "DXGI_FORMAT_P8";
		case DXGI_FORMAT_A8P8: return "DXGI_FORMAT_A8P8";
		case DXGI_FORMAT_B4G4R4A4_UNORM: return "DXGI_FORMAT_B4G4R4A4_UNORM";
		case DXGI_FORMAT_P208: return "DXGI_FORMAT_P208";
		case DXGI_FORMAT_V208: return "DXGI_FORMAT_V208";
		case DXGI_FORMAT_V408: return "DXGI_FORMAT_V408";
		default: return "Unknown Format";
		}
	}

	std::string DXGIFormatToHLSLType(DXGI_FORMAT format) {
		static const std::unordered_map<DXGI_FORMAT, std::string> formatToHLSLType = {
			// Float formats
			{ DXGI_FORMAT_R32G32B32A32_FLOAT, "float4" },
			{ DXGI_FORMAT_R32G32B32_FLOAT,    "float3" },
			{ DXGI_FORMAT_R32G32_FLOAT,       "float2" },
			{ DXGI_FORMAT_R32_FLOAT,          "float" },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, "float4" },
			{ DXGI_FORMAT_R16G16_FLOAT,       "float2" },
			{ DXGI_FORMAT_R16_FLOAT,          "float" },

			// Unsigned int formats
			{ DXGI_FORMAT_R32G32B32A32_UINT,  "uint4" },
			{ DXGI_FORMAT_R32G32B32_UINT,     "uint3" },
			{ DXGI_FORMAT_R32G32_UINT,        "uint2" },
			{ DXGI_FORMAT_R32_UINT,           "uint" },
			{ DXGI_FORMAT_R16G16B16A16_UINT,  "uint4" },
			{ DXGI_FORMAT_R16G16_UINT,        "uint2" },
			{ DXGI_FORMAT_R16_UINT,           "uint" },
			{ DXGI_FORMAT_R8G8B8A8_UINT,      "uint4" },
			{ DXGI_FORMAT_R8G8_UINT,          "uint2" },
			{ DXGI_FORMAT_R8_UINT,            "uint" },

			// Signed int formats
			{ DXGI_FORMAT_R32G32B32A32_SINT,  "int4" },
			{ DXGI_FORMAT_R32G32B32_SINT,     "int3" },
			{ DXGI_FORMAT_R32G32_SINT,        "int2" },
			{ DXGI_FORMAT_R32_SINT,           "int" },
			{ DXGI_FORMAT_R16G16B16A16_SINT,  "int4" },
			{ DXGI_FORMAT_R16G16_SINT,        "int2" },
			{ DXGI_FORMAT_R16_SINT,           "int" },
			{ DXGI_FORMAT_R8G8B8A8_SINT,      "int4" },
			{ DXGI_FORMAT_R8G8_SINT,          "int2" },
			{ DXGI_FORMAT_R8_SINT,            "int" },

			// Normalized formats (UNORM/SNORM)
			{ DXGI_FORMAT_R8G8B8A8_UNORM,     "float4" },
			{ DXGI_FORMAT_R8G8_UNORM,         "float2" },
			{ DXGI_FORMAT_R8_UNORM,           "float" },
			{ DXGI_FORMAT_R8G8B8A8_SNORM,     "float4" },
			{ DXGI_FORMAT_R8G8_SNORM,         "float2" },
			{ DXGI_FORMAT_R8_SNORM,           "float" },
			{ DXGI_FORMAT_R16G16B16A16_UNORM, "float4" },
			{ DXGI_FORMAT_R16G16_UNORM,       "float2" },
			{ DXGI_FORMAT_R16_UNORM,          "float" },
			{ DXGI_FORMAT_R16G16B16A16_SNORM, "float4" },
			{ DXGI_FORMAT_R16G16_SNORM,       "float2" },
			{ DXGI_FORMAT_R16_SNORM,          "float" },

			// Depth formats
			{ DXGI_FORMAT_D32_FLOAT,          "float" },
			{ DXGI_FORMAT_D24_UNORM_S8_UINT,  "float" },
			{ DXGI_FORMAT_D16_UNORM,          "float" },

			// Typeless formats (not directly usable in HLSL, but mapped for completeness)
			{ DXGI_FORMAT_R32G32B32A32_TYPELESS, "float4" },
			{ DXGI_FORMAT_R32G32B32_TYPELESS,    "float3" },
			{ DXGI_FORMAT_R32G32_TYPELESS,       "float2" },
			{ DXGI_FORMAT_R32_TYPELESS,          "float" },
			{ DXGI_FORMAT_R16G16B16A16_TYPELESS, "float4" },
			{ DXGI_FORMAT_R16G16_TYPELESS,       "float2" },
			{ DXGI_FORMAT_R16_TYPELESS,          "float" },
			{ DXGI_FORMAT_R8G8B8A8_TYPELESS,     "float4" },
			{ DXGI_FORMAT_R8G8_TYPELESS,         "float2" },
			{ DXGI_FORMAT_R8_TYPELESS,           "float" },

			// Others can be added as needed
		};

		//分からない時はとりあえずデフォルトのfloat4を返す
		auto it = formatToHLSLType.find(format);
		return (it != formatToHLSLType.end()) ? it->second : "float4";
	}

	D3D12_FILTER StringToD3D12Filter(const std::string& filter)
	{
		static const std::unordered_map<std::string, D3D12_FILTER> m = {
			{"POINT", D3D12_FILTER_MIN_MAG_MIP_POINT},
			{"LINEAR", D3D12_FILTER_MIN_MAG_MIP_LINEAR},
			{"MIN_MAG_MIP_POINT", D3D12_FILTER_MIN_MAG_MIP_POINT},
			{"MIN_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR},
			{"MIN_POINT_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT},
			{"MIN_POINT_MAG_MIP_LINEAR", D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR},
			{"MIN_LINEAR_MAG_MIP_POINT", D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT},
			{"MIN_LINEAR_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
			{"MIN_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT},
			{"MIN_MAG_MIP_LINEAR", D3D12_FILTER_MIN_MAG_MIP_LINEAR},
			{"ANISOTROPIC", D3D12_FILTER_ANISOTROPIC},
			{"COMPARISON_MIN_MAG_MIP_POINT", D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT},
			{"COMPARISON_MIN_MAG_POINT_MIP_LINEAR", D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR},
			{"COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT", D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT},
			{"COMPARISON_MIN_POINT_MAG_MIP_LINEAR", D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR},
			{"COMPARISON_MIN_LINEAR_MAG_MIP_POINT", D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT},
			{"COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR", D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
			{"COMPARISON_MIN_MAG_LINEAR_MIP_POINT", D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT},
			{"COMPARISON_MIN_MAG_MIP_LINEAR", D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR},
			{"COMPARISON_ANISOTROPIC", D3D12_FILTER_COMPARISON_ANISOTROPIC},
			{"MINIMUM_MIN_MAG_MIP_POINT", D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT},
			{"MINIMUM_MIN_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR},
			{"MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT},
			{"MINIMUM_MIN_POINT_MAG_MIP_LINEAR", D3D12_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR},
			{"MINIMUM_MIN_LINEAR_MAG_MIP_POINT", D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT},
			{"MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
			{"MINIMUM_MIN_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT},
			{"MINIMUM_MIN_MAG_MIP_LINEAR", D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR},
			{"MINIMUM_ANISOTROPIC", D3D12_FILTER_MINIMUM_ANISOTROPIC},
			{"MAXIMUM_MIN_MAG_MIP_POINT", D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT},
			{"MAXIMUM_MIN_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR},
			{"MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT},
			{"MAXIMUM_MIN_POINT_MAG_MIP_LINEAR", D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR},
			{"MAXIMUM_MIN_LINEAR_MAG_MIP_POINT", D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT},
			{"MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR", D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
			{"MAXIMUM_MIN_MAG_LINEAR_MIP_POINT", D3D12_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT},
			{"MAXIMUM_MIN_MAG_MIP_LINEAR", D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR},
			{"MAXIMUM_ANISOTROPIC", D3D12_FILTER_MAXIMUM_ANISOTROPIC}
		};

		return StringToEnumX(m, filter, D3D12_FILTER_ANISOTROPIC);
	}

	D3D12_TEXTURE_ADDRESS_MODE StringToD3D12TextureAddressMode(const std::string& modeName) {
		static const std::unordered_map<std::string, D3D12_TEXTURE_ADDRESS_MODE> modeMap = {
			{"WRAP", D3D12_TEXTURE_ADDRESS_MODE_WRAP},
			{"MIRROR", D3D12_TEXTURE_ADDRESS_MODE_MIRROR},
			{"CLAMP", D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
			{"BORDER", D3D12_TEXTURE_ADDRESS_MODE_BORDER},
			{"MIRROR_ONCE", D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE}
		};

		return StringToEnumX(modeMap, modeName, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
	}

	D3D12_COMPARISON_FUNC StringToD3D12ComparisonFunc(const std::string& funcName) {
		static const std::unordered_map<std::string, D3D12_COMPARISON_FUNC> funcMap = {
			{"NEVER", D3D12_COMPARISON_FUNC_NEVER},
			{"LESS", D3D12_COMPARISON_FUNC_LESS},
			{"EQUAL", D3D12_COMPARISON_FUNC_EQUAL},
			{"LESS_EQUAL", D3D12_COMPARISON_FUNC_LESS_EQUAL},
			{"GREATER", D3D12_COMPARISON_FUNC_GREATER},
			{"NOT_EQUAL", D3D12_COMPARISON_FUNC_NOT_EQUAL},
			{"GREATER_EQUAL", D3D12_COMPARISON_FUNC_GREATER_EQUAL},
			{"ALWAYS", D3D12_COMPARISON_FUNC_ALWAYS}
		};
		return StringToEnumX(funcMap, funcName, D3D12_COMPARISON_FUNC_ALWAYS);
	}

	D3D12_BLEND StringToD3D12Blend(const std::string& blendName) {
		static const std::unordered_map<std::string, D3D12_BLEND> blendMap = {
			{"ZERO", D3D12_BLEND_ZERO},
			{"ONE", D3D12_BLEND_ONE},
			{"SRC_COLOR", D3D12_BLEND_SRC_COLOR},
			{"INV_SRC_COLOR", D3D12_BLEND_INV_SRC_COLOR},
			{"SRC_ALPHA", D3D12_BLEND_SRC_ALPHA},
			{"INV_SRC_ALPHA", D3D12_BLEND_INV_SRC_ALPHA},
			{"DEST_ALPHA", D3D12_BLEND_DEST_ALPHA},
			{"INV_DEST_ALPHA", D3D12_BLEND_INV_DEST_ALPHA},
			{"DEST_COLOR", D3D12_BLEND_DEST_COLOR},
			{"INV_DEST_COLOR", D3D12_BLEND_INV_DEST_COLOR},
			{"SRC_ALPHA_SAT", D3D12_BLEND_SRC_ALPHA_SAT},
			{"BLEND_FACTOR", D3D12_BLEND_BLEND_FACTOR},
			{"INV_BLEND_FACTOR", D3D12_BLEND_INV_BLEND_FACTOR},
			{"SRC1_COLOR", D3D12_BLEND_SRC1_COLOR},
			{"INV_SRC1_COLOR", D3D12_BLEND_INV_SRC1_COLOR},
			{"SRC1_ALPHA", D3D12_BLEND_SRC1_ALPHA},
			{"INV_SRC1_ALPHA", D3D12_BLEND_INV_SRC1_ALPHA}
		};
		return StringToEnumX(blendMap, blendName, D3D12_BLEND_ONE);
	}


	D3D12_BLEND_OP StringToD3D12BlendOp(const std::string& blendOpName) {
		static const std::unordered_map<std::string, D3D12_BLEND_OP> blendOpMap = {
			{"ADD", D3D12_BLEND_OP_ADD},
			{"SUBTRACT", D3D12_BLEND_OP_SUBTRACT},
			{"REV_SUBTRACT", D3D12_BLEND_OP_REV_SUBTRACT},
			{"MIN", D3D12_BLEND_OP_MIN},
			{"MAX", D3D12_BLEND_OP_MAX}
		};
		return StringToEnumX(blendOpMap, blendOpName, D3D12_BLEND_OP_ADD);
	}

	D3D12_LOGIC_OP StringToD3D12LogicOp(const std::string& logicOpName) {
		static const std::unordered_map<std::string, D3D12_LOGIC_OP> logicOpMap = {
			{"CLEAR", D3D12_LOGIC_OP_CLEAR},
			{"SET", D3D12_LOGIC_OP_SET},
			{"COPY", D3D12_LOGIC_OP_COPY},
			{"COPY_INVERTED", D3D12_LOGIC_OP_COPY_INVERTED},
			{"NOOP", D3D12_LOGIC_OP_NOOP},
			{"INVERT", D3D12_LOGIC_OP_INVERT},
			{"AND", D3D12_LOGIC_OP_AND},
			{"NAND", D3D12_LOGIC_OP_NAND},
			{"OR", D3D12_LOGIC_OP_OR},
			{"NOR", D3D12_LOGIC_OP_NOR},
			{"XOR", D3D12_LOGIC_OP_XOR},
			{"EQUIV", D3D12_LOGIC_OP_EQUIV},
			{"AND_REVERSE", D3D12_LOGIC_OP_AND_REVERSE},
			{"AND_INVERTED", D3D12_LOGIC_OP_AND_INVERTED},
			{"OR_REVERSE", D3D12_LOGIC_OP_OR_REVERSE},
			{"OR_INVERTED", D3D12_LOGIC_OP_OR_INVERTED}
		};
		return StringToEnumX(logicOpMap, logicOpName, D3D12_LOGIC_OP_NOOP);
	}

	D3D12_STATIC_BORDER_COLOR StringToD3D12StaticBorderColor(const std::string& colorName) {
		static const std::unordered_map<std::string, D3D12_STATIC_BORDER_COLOR> colorMap = {
			{"TRANSPARENT_BLACK", D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK},
			{"OPAQUE_BLACK", D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK},
			{"OPAQUE_WHITE", D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE}
		};
		return StringToEnumX(colorMap, colorName, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK);
	}

	D3D12_SHADER_VISIBILITY StringToD3D12ShaderVisibility(const std::string& visibilityName) {
		static const std::unordered_map<std::string, D3D12_SHADER_VISIBILITY> visibilityMap = {
			{"ALL", D3D12_SHADER_VISIBILITY_ALL},
			{"VERTEX", D3D12_SHADER_VISIBILITY_VERTEX},
			{"HULL", D3D12_SHADER_VISIBILITY_HULL},
			{"DOMAIN", D3D12_SHADER_VISIBILITY_DOMAIN},
			{"GEOMETRY", D3D12_SHADER_VISIBILITY_GEOMETRY},
			{"PIXEL", D3D12_SHADER_VISIBILITY_PIXEL}
		};
		return StringToEnumX(visibilityMap, visibilityName, D3D12_SHADER_VISIBILITY_ALL);
	}

	D3D12_COLOR_WRITE_ENABLE StringToD3D12ColorWriteEnable(const std::string& colorWriteEnableName) {
		static const std::unordered_map<std::string, D3D12_COLOR_WRITE_ENABLE> colorWriteEnableMap = {
			{"RED", D3D12_COLOR_WRITE_ENABLE_RED},
			{"GREEN", D3D12_COLOR_WRITE_ENABLE_GREEN},
			{"BLUE", D3D12_COLOR_WRITE_ENABLE_BLUE},
			{"ALPHA", D3D12_COLOR_WRITE_ENABLE_ALPHA},
			{"COLOR", (D3D12_COLOR_WRITE_ENABLE)(D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE)},
			{"ALL", D3D12_COLOR_WRITE_ENABLE_ALL}
		};
		return StringToEnumX(colorWriteEnableMap, colorWriteEnableName, D3D12_COLOR_WRITE_ENABLE_ALL);
	}

	D3D12_FILL_MODE StringToD3D12FillMode(const std::string& fillModeName) {
		static const std::unordered_map<std::string, D3D12_FILL_MODE> fillModeMap = {
			{"WIREFRAME", D3D12_FILL_MODE_WIREFRAME},
			{"SOLID", D3D12_FILL_MODE_SOLID}
		};
		return StringToEnumX(fillModeMap, fillModeName, D3D12_FILL_MODE_SOLID);
	}

	D3D12_CULL_MODE StringToD3D12CullMode(const std::string& cullModeName) {
		static const std::unordered_map<std::string, D3D12_CULL_MODE> cullModeMap = {
			{"NONE", D3D12_CULL_MODE_NONE},
			{"FRONT", D3D12_CULL_MODE_FRONT},
			{"BACK", D3D12_CULL_MODE_BACK}
		};
		return StringToEnumX(cullModeMap, cullModeName, D3D12_CULL_MODE_BACK);
	}

	D3D12_CONSERVATIVE_RASTERIZATION_MODE StringToD3D12ConservativeRasterizationMode(const std::string& modeName) {
		static const std::unordered_map<std::string, D3D12_CONSERVATIVE_RASTERIZATION_MODE> modeMap = {
			{"OFF", D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF},
			{"ON", D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON}
		};
		return StringToEnumX(modeMap, modeName, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF);
	}


	D3D12_DEPTH_WRITE_MASK StringToD3D12DepthWriteMask(const std::string& maskName) {
		static const std::unordered_map<std::string, D3D12_DEPTH_WRITE_MASK> maskMap = {
			{"ZERO", D3D12_DEPTH_WRITE_MASK_ZERO},
			{"ALL", D3D12_DEPTH_WRITE_MASK_ALL}
		};
		return StringToEnumX(maskMap, maskName, D3D12_DEPTH_WRITE_MASK_ALL);
	}

	D3D12_STENCIL_OP StringToD3D12StencilOp(const std::string& stencilOpName) {
		static const std::unordered_map<std::string, D3D12_STENCIL_OP> stencilOpMap = {
			{"KEEP", D3D12_STENCIL_OP_KEEP},
			{"ZERO", D3D12_STENCIL_OP_ZERO},
			{"REPLACE", D3D12_STENCIL_OP_REPLACE},
			{"INCR_SAT", D3D12_STENCIL_OP_INCR_SAT},
			{"DECR_SAT", D3D12_STENCIL_OP_DECR_SAT},
			{"INVERT", D3D12_STENCIL_OP_INVERT},
			{"INCR", D3D12_STENCIL_OP_INCR},
			{"DECR", D3D12_STENCIL_OP_DECR}
		};
		return StringToEnumX(stencilOpMap, stencilOpName, D3D12_STENCIL_OP_KEEP);
	}

	D3D12_PRIMITIVE_TOPOLOGY_TYPE StringToD3D12PrimitiveTopologyType(const std::string& topologyTypeName) {
		static const std::unordered_map<std::string, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topologyTypeMap = {
			{"UNDEFINED", D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED},
			{"POINT", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT},
			{"LINE", D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
			{"TRIANGLE", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
			{"PATCH", D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH}
		};
		return StringToEnumX(topologyTypeMap, topologyTypeName, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
	}

	D3D12_INPUT_CLASSIFICATION StringToD3D12InputClassification(const std::string& classificationName) {
		static const std::unordered_map<std::string, D3D12_INPUT_CLASSIFICATION> classificationMap = {
			{"PER_VERTEX_DATA", D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
			{"PER_INSTANCE_DATA", D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA}
		};
		return StringToEnumX(classificationMap, classificationName, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
	}

	std::string D3D12InputClassificationToString(D3D12_INPUT_CLASSIFICATION c) {
		switch (c) {
		case D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA:
			return "PER_VERTEX_DATA";
		case D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA:
			return "PER_INSTANCE_DATA";
		}
		return "UNKNOWN";
	}

	D3D12_HIT_GROUP_TYPE StringToD3D12HitGroupType(const std::string& s) {
		static const std::unordered_map<std::string, D3D12_HIT_GROUP_TYPE> map = {
			{"TRIANGLES", D3D12_HIT_GROUP_TYPE_TRIANGLES},
			{"PROCEDURAL_PRIMITIVE", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE}
		};
		return StringToEnumX(map, s, D3D12_HIT_GROUP_TYPE_TRIANGLES);
	}




	/* DirectXの構造体をJSONから読めるようにするためのラッパー */
	//! D3D12_RENDER_TARGET_BLEND_DESCを参照
	struct FXRendertargetBlendDesc {
		//CD3DX12_BLEND_DESC b;
		bool blendEnable = false;
		bool logicOpEnable = false;
		std::string srcBlend = "ONE";
		std::string destBlend = "ZERO";
		std::string blendOp = "ADD";
		std::string srcBlendAlpha = "ONE";
		std::string destBlendAlpha = "ZERO";
		std::string blendOpAlpha = "ADD";
		std::string logicOp = "NOOP";
		std::string renderTargetWriteMask = "ALL";
		D3D12_RENDER_TARGET_BLEND_DESC Convert()
		{
			D3D12_RENDER_TARGET_BLEND_DESC r;
			r.BlendEnable = blendEnable;
			r.LogicOpEnable = logicOpEnable;
			r.SrcBlend = StringToD3D12Blend(srcBlend);
			r.DestBlend = StringToD3D12Blend(destBlend);
			r.BlendOp = StringToD3D12BlendOp(blendOp);
			r.SrcBlendAlpha = StringToD3D12Blend(srcBlendAlpha);
			r.DestBlendAlpha = StringToD3D12Blend(destBlendAlpha);
			r.BlendOpAlpha = StringToD3D12BlendOp(blendOpAlpha);
			r.LogicOp = StringToD3D12LogicOp(logicOp);
			r.RenderTargetWriteMask = StringToD3D12ColorWriteEnable(renderTargetWriteMask);
			return r;
		}

		template <class Archive>
		void serialize(Archive& A) {
			CEREOPT(blendEnable) CEREOPT(logicOpEnable)
				CEREOPT(srcBlend) CEREOPT(destBlend) CEREOPT(blendOp)
				CEREOPT(srcBlendAlpha) CEREOPT(destBlendAlpha) CEREOPT(blendOpAlpha)
				CEREOPT(logicOp) CEREOPT(renderTargetWriteMask)
		}
		/*
		BOOL BlendEnable;
		BOOL LogicOpEnable;
		D3D12_BLEND SrcBlend;
		D3D12_BLEND DestBlend;
		D3D12_BLEND_OP BlendOp;
		D3D12_BLEND SrcBlendAlpha;
		D3D12_BLEND DestBlendAlpha;
		D3D12_BLEND_OP BlendOpAlpha;
		D3D12_LOGIC_OP LogicOp;
		UINT8 RenderTargetWriteMask;
		*/
	};

	//! D3D12_BLEND_DESCを参照
	struct FXBlendDesc {
		//CD3DX12_BLEND_DESC b;
		bool alphaToCoverageEnable = false;
		bool independentBlendEnable = false;
		FXRendertargetBlendDesc renderTarget[8];
		D3D12_BLEND_DESC Convert()
		{
			D3D12_BLEND_DESC r;
			r.AlphaToCoverageEnable = alphaToCoverageEnable;
			r.IndependentBlendEnable = independentBlendEnable;
			for (int i = 0; i < 8; i++) {
				r.RenderTarget[i] = renderTarget[i].Convert();
			}
			return r;
		}
		template <class Archive>
		void serialize(Archive& A) {
			CEREOPT(alphaToCoverageEnable) CEREOPT(independentBlendEnable)
				CEREOPT2("renderTarget0", renderTarget[0])
				CEREOPT2("renderTarget1", renderTarget[1])
				CEREOPT2("renderTarget2", renderTarget[2])
				CEREOPT2("renderTarget3", renderTarget[3])
				CEREOPT2("renderTarget4", renderTarget[4])
				CEREOPT2("renderTarget5", renderTarget[5])
				CEREOPT2("renderTarget6", renderTarget[6])
				CEREOPT2("renderTarget7", renderTarget[7])
		}

		/*
		BOOL AlphaToCoverageEnable;
		BOOL IndependentBlendEnable;
		D3D12_RENDER_TARGET_BLEND_DESC RenderTarget[8];
		*/
	};

	//! D3D12_RASTERIZER_DESCを参照
	struct FXRasterizerDesc {
		//CD3DX12_RASTERIZER_DESC r;
		std::string fillMode = "SOLID";
		std::string cullMode = "BACK";
		bool frontCounterClockwise = false;
		INT depthBias = D3D12_DEFAULT_DEPTH_BIAS;
		float depthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		float slopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		bool depthClipEnable = true;
		bool multisampleEnable = false;
		bool antialiasedLineEnable = false;
		UINT forcedSampleCount = 0;
		std::string conservativeRaster = "OFF";
		D3D12_RASTERIZER_DESC Convert()
		{
			D3D12_RASTERIZER_DESC r;
			r.FillMode = StringToD3D12FillMode(fillMode);
			r.CullMode = StringToD3D12CullMode(cullMode);
			r.FrontCounterClockwise = frontCounterClockwise;
			r.DepthBias = depthBias;
			r.DepthBiasClamp = depthBiasClamp;
			r.SlopeScaledDepthBias = slopeScaledDepthBias;
			r.DepthClipEnable = depthClipEnable;
			r.MultisampleEnable = multisampleEnable;
			r.AntialiasedLineEnable = antialiasedLineEnable;
			r.ForcedSampleCount = forcedSampleCount;
			r.ConservativeRaster = StringToD3D12ConservativeRasterizationMode(conservativeRaster);
			return r;
		}
		template <class Archive>
		void serialize(Archive& A) {
			CEREOPT(fillMode) CEREOPT(cullMode)
				CEREOPT(frontCounterClockwise) CEREOPT(depthBias) CEREOPT(depthBiasClamp)
				CEREOPT(slopeScaledDepthBias) CEREOPT(depthClipEnable) CEREOPT(multisampleEnable)
				CEREOPT(antialiasedLineEnable) CEREOPT(forcedSampleCount) CEREOPT(conservativeRaster)
		}
		/*
		D3D12_FILL_MODE FillMode;
		D3D12_CULL_MODE CullMode;
		BOOL FrontCounterClockwise;
		INT DepthBias;
		FLOAT DepthBiasClamp;
		FLOAT SlopeScaledDepthBias;
		BOOL DepthClipEnable;
		BOOL MultisampleEnable;
		BOOL AntialiasedLineEnable;
		UINT ForcedSampleCount;
		D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
		*/
	};


	//! D3D12_DEPTH_STENCILOP_DESCを参照
	struct FXDepthStencilOpDesc {
		std::string stencilFailOp = "KEEP";
		std::string stencilDepthFailOp = "KEEP";
		std::string stencilPassOp = "KEEP";
		std::string stencilFunc = "ALWAYS";
		D3D12_DEPTH_STENCILOP_DESC Convert()
		{
			D3D12_DEPTH_STENCILOP_DESC r;
			r.StencilFailOp = StringToD3D12StencilOp(stencilFailOp);
			r.StencilDepthFailOp = StringToD3D12StencilOp(stencilDepthFailOp);
			r.StencilPassOp = StringToD3D12StencilOp(stencilPassOp);
			r.StencilFunc = StringToD3D12ComparisonFunc(stencilFunc);
			return r;
		}
		template <class Archive>
		void serialize(Archive& A) {
			CEREOPT(stencilFailOp) CEREOPT(stencilDepthFailOp)
				CEREOPT(stencilPassOp) CEREOPT(stencilFunc)
		}
		/*
		D3D12_STENCIL_OP StencilFailOp;
		D3D12_STENCIL_OP StencilDepthFailOp;
		D3D12_STENCIL_OP StencilPassOp;
		D3D12_COMPARISON_FUNC StencilFunc;
		*/
	};

	//! D3D12_DEPTH_STENCIL_DESCを参照
	struct FXDepthStencilDesc {
		//CD3DX12_DEPTH_STENCIL_DESC d;
		bool depthEnable = true;
		std::string depthWriteMask = "ALL";
		std::string depthFunc = "LESS";
		bool stencilEnable = false;
		UINT8 stencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		UINT8 stencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		FXDepthStencilOpDesc frontFace;
		FXDepthStencilOpDesc backFace;
		D3D12_DEPTH_STENCIL_DESC Convert()
		{
			D3D12_DEPTH_STENCIL_DESC r;
			r.DepthEnable = depthEnable;
			r.DepthWriteMask = StringToD3D12DepthWriteMask(depthWriteMask);
			r.DepthFunc = StringToD3D12ComparisonFunc(depthFunc);
			r.StencilEnable = stencilEnable;
			r.StencilReadMask = stencilReadMask;
			r.StencilWriteMask = stencilWriteMask;
			r.FrontFace = frontFace.Convert();
			r.BackFace = backFace.Convert();
			return r;
		}
		template <class Archive>
		void serialize(Archive& A) {
			CEREOPT(depthEnable) CEREOPT(depthWriteMask) CEREOPT(depthFunc) CEREOPT(stencilEnable)
				CEREOPT(stencilReadMask) CEREOPT(stencilWriteMask) CEREOPT(frontFace) CEREOPT(backFace)
		}
		/*
		BOOL DepthEnable;
		D3D12_DEPTH_WRITE_MASK DepthWriteMask;
		D3D12_COMPARISON_FUNC DepthFunc;
		BOOL StencilEnable;
		UINT8 StencilReadMask;
		UINT8 StencilWriteMask;
		D3D12_DEPTH_STENCILOP_DESC FrontFace;
		D3D12_DEPTH_STENCILOP_DESC BackFace;
		*/
	};


	/** @brief 入力ウィジェットについての設定
	* @details
	* MikuMikuDayoの実装では、表情モーフ用のスライダーについてのみ有効。ボーンなどには適用されない\n
	* 使用例：
	* @code
	*{
	*	"name":"C_Exposure", "controllerName":"rencon.pmx", "item":"露出", "type":"float",
	*	"slider":{ "min":1.0/1024, "max":1024, "def":1, "log":true }
	*}
	* rencon.pmxの露出モーフの値をC_Exposure変数に取得する
	* スライダーからは1/1024倍～1024倍までの値を対数スケールで入力できるようにし、デフォルト値は1とする
	* @endcode
	*/
	struct FXSlider {
		//! 最小値 jsonには"min"で記述する
		float minValue = 0.0f;	
		//! 最大値 jsonには"max"で記述する
		float maxValue = 1.0f;	
		//! デフォルト値 jsonには"def"で記述する
		float defValue = 0.0f;	
		//! 対数スケールでスライダーを作る jsonには"log"で記述する
		bool logValue = false;
		//! 整数入力用スライダーにする  jsonには"int"で記述する
		bool intValue = false;	
		template<class Archive>
		void serialize(Archive& A) {
			CEREOPT2("min", minValue) CEREOPT2("max", maxValue) CEREOPT2("def", defValue) CEREOPT2("log", logValue) CEREOPT2("int", intValue)
		}
		bool operator==(const FXSlider& rhs) const { return minValue == rhs.minValue && maxValue == rhs.maxValue && defValue == rhs.defValue && logValue == rhs.logValue && intValue == rhs.intValue; }
		bool operator!=(const FXSlider& rhs) const { return !(*this == rhs); }
	};

	//! @brief コントローラの定義 HLSL側からどんな値をホストアプリケーションに要求するか？
	//! @details
	//! MikuMikuDayoでの実装では以下のように解釈される
	//! @code
	//! 例) { "name":"C_DayoNekoze", "controllerName":"dayo.pmx", "item":"猫背" }
	//!		dayo.pmx の猫背ボーンの情報をHLSL内のC_DayoNekoze変数に得る
	//! 
	//!     "type" : "bool"→ "猫背"ボーンまたはモーフがあるか？
	//!				 "float" → "猫背"モーフの値を得る
	//!				 "float3" → 猫背ボーンの位置
	//!				 "float4x4" → 猫背ボーンの姿勢を得る
	//!				 "int" → 猫背ボーンまたは猫背モーフが何番目のボーン/モーフであるかを得る。同名のボーンとモーフがある場合はモーフを優先(どちらも存在しない場合は-1)
	//! 
	//! ●特殊なitem
	//! itemが空文字の場合はモデルそのものの情報を得る
	//! "type" : "bool" → 表示状態を得る。trueで表示、falseで非表示
	//!			 "int" → コントローラのモデル番号を得る。コントローラの元になるモデル自体がアプリケーション内に存在しない場合は-1
	//! 
	//! "item" : "(exists)"の場合
	//!	"type" : "bool" → コントローラ自体が存在するかどうかを得る
	//!			 "int" → 同名のコントローラが何個存在するかを得る
	//! 
	//! ●特殊なcontrollerName
	//! "controllerName" : "(self)" の場合、エフェクトの起動元になったpmxモデルとして解釈される(デフォーマまたはポストプロセスの時に有効)
	//! 
	//! ●コントローラ配列
	//! name の末尾に[]をつけると、配列になる。[]内に要素数を書く
	//! 例) { "name":"dayo[8]", "controllerName":"dayo.pmx", "item":"ダヨー", "type":"float"}
	//! ConstantBuffer内にdayo[8]という配列変数が出来る、dayo[1]以降は同名の2台目以降のコントローラから値が入力される
	//! @endcode
	struct FXController {
		//! hlslからアクセスするための変数名
		std::string name;			
		//! ホストアプリケーション側からコントローラを検索するための名前(MikuMikuDayoの実装ではpmxファイル名)
		std::string controllerName = "(self)";	
		//! 項目名、モーフ名・ボーン名など、空文字の場合はモデル全体の状態
		std::string item;
		//! 返り値の型
		std::string type = "float";	
		//! 入力ウィジェットについての設定
		FXSlider slider;
		//! 説明文。多言語対応のためvector。MikuMikuDayoの実装では第一要素に英語、第二要素に日本語
		std::vector<std::string> desc;	
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(controllerName) CEREOPT(item) CEREOPT(type)
			CEREOPT(slider) CEREOPT(desc)
		}
		//! \cond
		//↓FX.Loadされると決定される値
		int offset = 0;				//ホストアプリケーション側で、この変数のためにコントローラ側から書き込むコンスタントバッファ上のアドレス
		int size = 0;				//変数のサイズは何バイトか？(typeで判別できるけど)
		int iModel = 0;				//QueryControllerで帰ってきたモデル・アイテム番号(毎フレーム文字列の変換・検索を行わなくて済むようにするためのデータ)
		int iItem = 0;
		//↓β3で追加。同名のコントローラが複数ある場合に配列でアクセスできるようにする
		//●●0.pmx～●●7.pmxみたいに同名のpmxファイルをいくつも拵えなくて良いようにするためのモノ
		//配列変数。同名のコントローラの2台目以降から配列に値が書き込まれるようになる
		int doppelCount = 0;		//配列変数を作る場合のカウンタ。0の場合は配列ではない
		int doppelIndex = 0;		//配列変数を作る場合のインデックス。
		//! \endcond
	};

	/*コントローラがホストアプリケーションから値を得るための仕組み
	* FXのロード時に使われるFXLoadConfigにQueryController,ControllerCallbackという2つのコールバック関数を登録できる
	* 1.FXオブジェクトが適宜(エフェクト読み込み時など)QueryControllerでコントローラ名とアイテム名に対応するモデル番号、アイテム番号を聞いてくるので、ホスト側はそれに答える
	* 2.FX::UpdateController()やFX::ResolveMatDescController()が実行されるとコントローラの値を得るためにControllerCallbackが呼ばれる
	* 　ホスト側はモデル番号・アイテム番号を元にコントローラの値を返すようにする
	*/



	//相対値からリソースのサイズを決定するためのフラグ。各々の要素数の積を求める
	enum class FXConv { one = 0, x = 1, y = 2, z = 4, xy = 3, yz = 6, xz = 5, xyz = 7 };
	FXConv operator|(FXConv L, FXConv R) { return static_cast<FXConv>(static_cast<uint64_t>(L) | static_cast<uint64_t>(R)); }
	FXConv operator&(FXConv L, FXConv R) { return static_cast<FXConv>(static_cast<uint64_t>(L) & static_cast<uint64_t>(R)); }

	FXConv StringToFXConv(const std::string& modeName) {
		static const std::unordered_map<std::string, FXConv> modeMap = {
			{"ONE", FXConv::one},
			{"X", FXConv::x},
			{"Y", FXConv::y},
			{"Z", FXConv::z},
			{"XY", FXConv::xy},
			{"YZ", FXConv::yz},
			{"XZ", FXConv::xz},
			{"XYZ", FXConv::xyz}
		};
		return StringToEnumX(modeMap, modeName, FXConv::one);
	}

	//サイズの丸め規則の指定
	enum class FXRound { trunc = 0, round = 1, ceil = 2 };
	FXRound StringToFXRound(const std::string& modeName) {
		static const std::unordered_map<std::string, FXRound> modeMap = {
			{"TRUNC", FXRound::trunc},
			{"ROUND", FXRound::round},
			{"CEIL", FXRound::ceil}
		};
		return StringToEnumX(modeMap, modeName, FXRound::trunc);
	}

	/** @brief リソースや出力先のサイズを表す構造体
	* @details
	* width,height,depthはabsolute = trueの場合のみ使われる\n
	* base, ratio, convX/Y/Z, roundingは absolute = falseの場合のみ使われる
	* @par baseに対する予約語(大文字小文字は区別される)
	* - base == "VERTEXCOUNT" の時、対象モデルの頂点数として解釈される
	* - base == "DEFAULT_RTSIZE" の時、出力先の画面の画素数として解釈される
	* - base == "TOTALMATERIALCOUNT"の時、シーン内に登録されている全モデルの材質数の合計として解釈される
	*/
	struct FXSize {
		//! true:絶対的なサイズを示す false:baseに対する相対的なサイズを示す
		bool absolute = false;
		//! リソースの次元。deminsionが0でabsoluteがfalseならばbaseと同じ次元数になる
		int32_t dimension = 0;	
		//! バッファの要素数、テクスチャの幅
		uint64_t width = 1;	
		//! テクスチャの高さ
		uint64_t height = 1;
		//! 3Dテクスチャの奥行き
		uint64_t depth = 1;
		/**
		* absolute = falseだった場合のサイズの基準になるリソース\n
		* MikuMikuDayoの実装では、空文字だった場合はエフェクトのカテゴリにより以下のように決まる\n
		* デフォーマの場合、対象モデルの頂点数(1次元)\n
		* それ以外の場合、出力画面の画素数(2次元)
		*/
		std::string base;
		//! absolute = falseだった時に使用されるbaseのサイズに対する割合
		XMFLOAT3 ratio = { 1,1,1 };	
		/**
		* 各次元の要素数はbaseのどの次元の要素数同士の積で求まるか？以下のいずれかが有効\n
		* "x" : base.width\n
		* "y" : base.height\n
		* "z" : base:depth\n
		* "xy" : base.width * base.height\n
		* "yz" : base.height * base.depth\n
		* "xz" : base.width * base.depth\n
		* "xyz" : base.width * base.height * base.depth\n
		* "one" : 1
		*/
		std::string convX = "x";	
		std::string convY = "y";
		std::string convZ = "z";
		//! 小数の丸め規則。"trunc":切捨て,  "ceil":切り上げ, "round":四捨五入
		std::string rounding = "trunc";
		
		//文字列化
		std::string String() {
			if (absolute)
				return std::format("absolute {}x{}x{}({}D)", width, height, depth, dimension);
			else
				return std::format("relative({}), ratio:{}x{}x{}, conv {}->X {}->Y {}->Z, rounding:{}", base, ratio.x, ratio.y, ratio.z, convX, convY, convZ, rounding);
		}

		FXSize() {};
		FXSize(size_t w, size_t h, size_t d, int dim) {
			dimension = dim;
			absolute = true;
			width = w;
			height = h;
			depth = d;
		}
		FXSize(const XMUINT3& s, int dim) {
			dimension = dim;
			absolute = true;
			width = s.x;
			height = s.y;
			depth = s.z;
		}
		FXSize(const Res* r) {
			absolute = true;
			switch (r->type) {
			case ResType::buf: {
				dimension = 1;
				auto pbuf = dynamic_cast<const Buf*>(r);
				width = r->desc().Width / pbuf->elemSize;
				height = 1;
				depth = 1;
				break;
			}
			case ResType::tex2D: {
				dimension = 2;
				auto ptex = dynamic_cast<const Tex2D*>(r);
				width = r->desc().Width;
				height = r->desc().Height;
				depth = 1;
				break;
			}
			case ResType::tex3D: {
				dimension = 3;
				auto p3d = dynamic_cast<const Tex3D*>(r);
				width = r->desc().Width;
				height = r->desc().Height;
				depth = r->desc().DepthOrArraySize;
				break;
			}
			default: ThrowIfFailed(E_INVALIDARG, std::format(L"invalid ResType {} for FXSize()", r->Name()), __FILEW__, __LINE__);
			}
		}

		template<class Archive>
		void serialize(Archive& A) {
			CEREOPT(absolute) CEREOPT(dimension)
				CEREOPT(width) CEREOPT(height) CEREOPT(depth)
				CEREOPT(base) CEREOPT(ratio)
				CEREOPT2("convX", convX) CEREOPT2("convY", convY) CEREOPT2("convZ", convZ)
				CEREOPT(rounding)
		}
		//サイズの決定元src から、相対サイズ属性をもつ rel を見て、絶対サイズの格納されたFXSize構造体を返す
		static FXSize convert(const FXSize& src, const FXSize& rel) {
			if (!src.absolute) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"resolving resoruce size failed! {} is not absolute sized resource", L(rel.base)), __FILEW__, __LINE__);
				return {};
			}
			if (rel.dimension > 3 || rel.dimension < 0) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"invalid dimension {}", rel.dimension), __FILEW__, __LINE__);
				return {};
			}
			if (src.dimension > 3 || src.dimension <= 0) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"invalid dimension {}", src.dimension), __FILEW__, __LINE__);
				return {};
			}
			if (rel.absolute)
				return rel;

			bool invalidDim = false;
			FXSize r = rel;
			auto X = src.width;
			auto Y = src.height;
			auto Z = src.depth;
			FXRound rou = StringToFXRound(rel.rounding);

			//次元0の場合、srcの次元に合わせる
			if (r.dimension == 0)
				r.dimension = src.dimension;

			auto convLambda = [&](auto c) {
				size_t s = 1;
				if ((int)c & 1)	s *= X;
				if ((int)c & 2)	s *= Y;
				if ((int)c & 4)	s *= Z;
				return s;
				};

			auto roundLambda = [&](auto x)->size_t {
				if (rou == FXRound::trunc) {
					return trunc(x);
				} else if (rou == FXRound::round) {
					return round(x);
				} else {
					return ceil(x);
				}
			};

			auto cx = StringToFXConv(rel.convX);
			auto cy = StringToFXConv(rel.convY);
			auto cz = StringToFXConv(rel.convZ);

			if (r.dimension == 1) {
				r.width = convLambda(cx);
				r.height = 1;
				r.depth = 1;
			} else if (r.dimension == 2) {
				r.width = convLambda(cx);
				r.height = convLambda(cy);
				r.depth = 1;
			} else {
				r.width = convLambda(cx);
				r.height = convLambda(cy);
				r.depth = convLambda(cz);
			}

			r.width = max(1ull, roundLambda(r.width * (double)r.ratio.x));
			r.height = max(1ull, roundLambda(r.height * (double)r.ratio.y));
			r.depth = max(1ull, roundLambda(r.depth * (double)r.ratio.z));

			r.absolute = true;
			return r;
		}
	};

	enum class FXShared { none = 0, source = 1, ref = 2 };	//共有関係ない / 共有される元 / 参照側

	FXShared StringToFXShared(const std::string& modeName) {
		static const std::unordered_map<std::string, FXShared> modeMap = {
			{"NONE", FXShared::none},
			{"SOURCE", FXShared::source},
			{"REF", FXShared::ref}
		};

		return StringToEnumX(modeMap, modeName, FXShared::none);
	}

	//! リソース
	struct FXRes {
		//! リソース名(シェーダからもこの名前でアクセスできる)
		std::string name;
		//! 型名 "float4"とか
		std::string type = "";
		//! 共有フラグ "none":共有しない, "source":共有源, "ref":共有参照
		std::string shared = "none";
		//! くっつけられるビューの種類。空文字でもSRVによるアクセスは可能。空文字以外では"rtv", "uav", "dsv", "rtv,uav"のどれか
		std::string view;
		// \cond
		virtual void IamPolymorphic() {};
		bool uav() const { return (view.find("UAV") != std::string::npos || view.find("uav") != std::string::npos); }
		// \endcond
	};

	//! 2Dテクスチャ。typeを省略すると型はformatから自動的に設定される
	struct FXTexture : FXRes {
		//! テクスチャのサイズ。filenameが空文字でない場合はサイズ・フォーマットは無視され、ファイルの内容から決定される
		FXSize size = {};			
		//! DXGI_FORMATを参照
		std::string format = "unknown";
		//! 読み込み元画像ファイル。空文字の場合はサイズ・フォーマットの指定に従って空のテクスチャが出来る
		std::string filename;
		//! mipチェーンを持つか？ trueの場合はサイズに合わせて一番細かいレベルまでのmipチェーンが作られる。falseの場合mipチェーンは作られない
		bool mipmap = false;	
		FXTexture() { size.dimension = 2; };
		
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(type) CEREOPT(view) CEREOPT(shared)
				CEREOPT(size) CEREOPT(format) CEREOPT(filename) CEREOPT(mipmap)
		}

		bool rtv() const { return (view.find("RTV") != std::string::npos || view.find("rtv") != std::string::npos); }
		bool dsv() const { return (view.find("DSV") != std::string::npos || view.find("dsv") != std::string::npos); }
	};

	//! 3Dテクスチャ。typeを省略すると型はformatから自動的に設定される
	struct FXTexture3D : FXTexture {
		FXTexture3D() { size.dimension = 3; };

		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(type) CEREOPT(view) CEREOPT(shared)
				CEREOPT(size) CEREOPT(format) CEREOPT(filename) CEREOPT(mipmap)
		}
	};

	//! 1次元のバッファ。構造体の配列として用いることが出来る
	struct FXBuffer : FXRes {
		std::string name;
		//! 要素ごとのバイト数、重要！省略した場合は型名から推定される
		uint64_t elemSize = 0;
		//! size.width = 要素数
		FXSize size = {};
		//! 要素を埋めるためのバイナリデータの入ったファイル名。指定が無い場合は空
		std::string filename;	
		FXBuffer() { size.dimension = 1; };
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(type) CEREOPT(view) CEREOPT(shared)
			CEREOPT(elemSize) CEREOPT(size) CEREOPT(filename)
		}
	};

	//! D3D12_STATIC_SAMPLER_DESCを参照
	struct FXSampler {
		//! HLSL側から参照する際のシンボル名
		std::string name;
		std::string filter = "ANISOTROPIC";
		std::string addressU = "WRAP", addressV = "WRAP", addressW = "WRAP";
		float mipLODBias = D3D12_DEFAULT_MIP_LOD_BIAS;
		UINT maxAnisotropy = D3D12_DEFAULT_MAX_ANISOTROPY;
		std::string comparisonFunc = "LESS_EQUAL";
		std::string borderColor = "OPAQUE_WHITE";
		float minLOD = 0;
		float maxLOD = D3D12_FLOAT32_MAX;
		std::string shaderVisibility = "ALL";
		// \cond
		UINT shaderRegister = 0;	//FX.Load()時に自動で割り振られる
		UINT registerSpace = 0;		//これもFX.Load()時に自動で割り振られる(0で固定)
		D3D12_STATIC_SAMPLER_DESC Convert()
		{
			D3D12_STATIC_SAMPLER_DESC r;
			r.Filter = StringToD3D12Filter(filter);
			r.AddressU = StringToD3D12TextureAddressMode(addressU);
			r.AddressV = StringToD3D12TextureAddressMode(addressV);
			r.AddressW = StringToD3D12TextureAddressMode(addressW);
			r.MipLODBias = mipLODBias;
			r.MaxAnisotropy = maxAnisotropy;
			r.ComparisonFunc = StringToD3D12ComparisonFunc(comparisonFunc);
			r.BorderColor = StringToD3D12StaticBorderColor(borderColor);
			r.MinLOD = minLOD;
			r.MaxLOD = maxLOD;
			r.ShaderRegister = shaderRegister;
			r.RegisterSpace = registerSpace;
			r.ShaderVisibility = StringToD3D12ShaderVisibility(shaderVisibility);
			return r;
		}
		// \endcond

		template <class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(filter) CEREOPT(addressU) CEREOPT(addressV) CEREOPT(addressW)
				CEREOPT(mipLODBias) CEREOPT(maxAnisotropy) CEREOPT(comparisonFunc) CEREOPT(borderColor)
				CEREOPT(minLOD) CEREOPT(maxLOD)
				CEREOPT(shaderVisibility)
		}

	};

	//! RTV[]に指定する要素
	struct FXRTV {
		//! テクスチャ名
		std::string name;
		//! mipチェーン付きテクスチャの場合、書き込むmipレベル
		int32_t mipSlice = 0;
		//! クリアする？
		bool clear = true;
		//! クリアする場合の値
		XMFLOAT4 value = { 0,0,0,1 };
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(mipSlice) CEREOPT(clear) CEREOPT(value)
		}
	};

	//! UAV[]に指定する要素
	struct FXUAV {
		//! リソース名
		std::string name;
		//! mipチェーン付きテクスチャの場合、書き込むmipレベル
		int32_t mipSlice = 0;
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name)); CEREOPT(mipSlice)
		}
	};

	//! DSVに指定する要素
	struct FXDSV {
		//! リソース名
		std::string name;
		//! mipチェーン付きテクスチャの場合、書き込むmipレベル
		int mipSlice = 0;
		//! クリアする？
		bool clear = true;
		//! クリアする場合の深度値
		float depth = 1.0f;
		//! クリアする場合のステンシル値
		int stencil = 0;
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(mipSlice) CEREOPT(clear) CEREOPT(depth) CEREOPT(stencil)
		}
	};

	//! ヒットグループの定義
	struct FXHitGroup {
		//! ヒットグループの種類。"triangles":三角ポリゴン, "procedural_primitive":プロシージャル定義図形
		std::string type = "triangles";
		//! closestHitシェーダのエントリポイント。空文字の場合はシェーダの割り当てなし
		std::string closestHit;
		//! anyHitシェーダのエントリポイント。空文字の場合はシェーダの割り当てなし
		std::string anyHit;
		//! intersectionシェーダのエントリポイント。空文字の場合はシェーダの割り当てなし
		std::string intersection;
		template<class Archive>
		void serialize(Archive& A) {
			CEREOPT(type)  CEREOPT(closestHit) CEREOPT(anyHit) CEREOPT(intersection)
		}
	};


	// D3D12用の構造体のままではSemnticNameがポインタなので
	// 文字列のためのメモリが解放された後にラスタライザパスを作成しようとすると
	// クラッシュしてしまう。それを防ぐためにSemanticNameをstringにした物
	//! D3D12_INPUT_ELEMENT_DESCを参照
	struct FXInputElementDesc {
		std::string semanticName;
		UINT semanticIndex = 0;
		std::string format = "UNKNOWN";
		UINT inputSlot = 0;
		UINT alignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
		std::string inputSlotClass = "PER_VERTEX_DATA";
		UINT instanceDataStepRate = 0;
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(semanticName));
			CEREOPT(semanticIndex)  CEREOPT(format) CEREOPT(inputSlot)
			CEREOPT(alignedByteOffset) CEREOPT(inputSlotClass) CEREOPT(instanceDataStepRate)
		}

		FXInputElementDesc() {};
		//descから文字列をコピーしてFXInputElementDescを作る
		FXInputElementDesc(D3D12_INPUT_ELEMENT_DESC desc) {
			semanticName = desc.SemanticName;
			semanticIndex = desc.SemanticIndex;
			format = DXGIFormatToU8(desc.Format);
			inputSlot = desc.InputSlot;
			alignedByteOffset = desc.AlignedByteOffset;
			inputSlotClass = D3D12InputClassificationToString(desc.InputSlotClass);
			instanceDataStepRate = desc.InstanceDataStepRate;
		}
		//Direct3D用の構造体へ変換
		D3D12_INPUT_ELEMENT_DESC D3D() const
		{
			D3D12_INPUT_ELEMENT_DESC r;
			r.SemanticName = semanticName.c_str();
			r.SemanticIndex = semanticIndex;
			r.Format = StringToDXGIFormat(format);
			r.InputSlot = inputSlot;
			r.AlignedByteOffset = alignedByteOffset;
			r.InputSlotClass = StringToD3D12InputClassification(inputSlotClass);
			r.InstanceDataStepRate = instanceDataStepRate;
			return r;
		}
		static std::vector<FXInputElementDesc>Convert(const std::vector<D3D12_INPUT_ELEMENT_DESC>& d3d) {
			std::vector<FXInputElementDesc> ret;
			for (auto& x : d3d) {
				ret.push_back(FXInputElementDesc(x));
			}
			return ret;
		}
		static std::vector<D3D12_INPUT_ELEMENT_DESC>Convert(const std::vector<FXInputElementDesc>& fx) {
			std::vector<D3D12_INPUT_ELEMENT_DESC> ret;
			for (auto& x : fx) {
				ret.push_back(x.D3D());
			}
			return ret;
		}
	};


	//! パスの分類
	enum class FXPassType { none = 0, raytracing = 1, postprocess = 2, rasterizer = 3, compute = 4, copy = 256, clearUAV = 257, clearRTV = 258, mipmapgen = 259 };

	FXPassType StringToFXPassType(const std::string& passtype)
	{
		static const std::unordered_map<std::string, FXPassType> passtypeMap = {
			{"NONE", FXPassType::none},
			{"RAYTRACING", FXPassType::raytracing},
			{"POSTPROCESS", FXPassType::postprocess},
			{"RASTERIZER", FXPassType::rasterizer},
			{"COMPUTE", FXPassType::compute},
			{"COPY", FXPassType::copy},
			{"CLEARUAV", FXPassType::clearUAV},
			{"CLEARRTV", FXPassType::clearRTV},
			{"MIPMAPGEN", FXPassType::mipmapgen}
		};
		return StringToEnumX(passtypeMap, passtype, FXPassType::postprocess);
	}

	//現在未使用
	const char MipmapCS[] = R"(
		Texture2D<float4> Source : register(t0);
		RWTexture2D<float4>Dest: register(u0);
		sampler samp : register(s0);

		[numthreads(8, 8, 1)]
		void CS( uint3 id : SV_DispatchThreadID )
		{
			uint W,H,nmip;
			Source.GetDimensions(0, W,H,nmip);
			float2 uv = (id.xy+0.5)/float2(W/2,H/2);
			Dest[id.xy] = Source.SampleLevel(samp,uv,0);
		}
	)";

	const char MipmapPS[] = R"(
		Texture2D<float4> Source : register(t0);
		sampler samp : register(s0);
		struct VSO { float4 pos:SV_POSITION; float2 uv:TEXCOORD0;};
		VSO VS(float4 pos:POSITION, float2 uv:TEXCOORD) { VSO o; o.pos=pos; o.uv=uv; return o;}
		float4 PS(VSO vso):SV_TARGET { return Source.SampleLevel(samp, vso.uv, 0); }
	)";

	//コピーやクリアなどのhlslコード外の機能を提供するパス、シリアライズされない
	struct FXFunctionalPass {
		FXPassType type = FXPassType::copy;
		//コピー用
		Res* copySrc = nullptr;
		Res* copyDest = nullptr;
		//以下、クリアターゲット用
		Res* clearTarget = nullptr;
		ComPtr<ID3D12DescriptorHeap>DH;	//クリアターゲットを格納するディスクリプタヒープ
		XMFLOAT4 clearValue;
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuHandle;
		D3D12_GPU_DESCRIPTOR_HANDLE m_gpuHandle;
		DXR* m_dxr = nullptr;
		//以下、mipmapgen用
		Tex2D* mipTarget = nullptr;
		std::vector<Pass>mipPass;

		FXFunctionalPass() {};
		FXFunctionalPass(DXR& dxr) { m_dxr = &dxr; }

		void Update();
		void SetupMipmapgen(Tex2D* target) {
			mipTarget = target;
			Update();
		}
	};

	//ラスタライザパスがどのモデルを描画するのか、というフラグ
	//all : 全部描く self:自分にセットされているモデルのみ other:自分以外にセットされているモデルのみ
	//buffer : エフェクト内で定義されているVB,IB
	enum class FXRasterModelTarget { all=0, self=1, other=2, buffer=3};
	FXRasterModelTarget StringToFXRasterModelTarget(const std::string& ramo)
	{
		static const std::unordered_map<std::string, FXRasterModelTarget> map = {
			{"ALL", FXRasterModelTarget::all},
			{"SELF", FXRasterModelTarget::self},
			{"OTHER", FXRasterModelTarget::other},
			{"BUFFER", FXRasterModelTarget::buffer}
		};
		return StringToEnumX(map, ramo, FXRasterModelTarget::all);
	}

	//! @brief パスを定義するための構造体
	//! @details
	//! 
	//! @par outputSizeのデフォルト値の決定法
	//! 以下の順序で決定する
	//! -# RTV[0]に指定されたリソースのサイズ
	//! -# RTVが空の場合、DSVに指定されたリソースのサイズ
	//! -# DSVも空の場合、UAV[0]に指定されたリソースのサイズ
	//! -# UAVも空の場合、エフェクトのdefaultOutputDimension, defaultOutputSize(MikuMikuDayoの実装ではエフェクトのカテゴリがデフォーマの場合は対象モデルの頂点数、それ以外の場合は出力画面の画素数)
	//! 
	//! @par numthreadsについて
	//! compute shaderにおいてスレッドグループあたりのスレッド数を指定するためのパラメータ\n
	//! YRZ_NUMTHREADSマクロが定義され、[numthreads(x,y,z)]に展開されるので、compute shaderのエントリポイントに併記すべし
	//! @code
	//! #ifdef YRZ_PASS_SamplePass
	//! YRZ_NUMTHREADS
	//! void CS(uint3 id : SV_DispatchThreadID)
	//! {
	//!		...
	//! }
	//! #endif
	//! @endcode
	//! outputSizeによって総スレッド数(スレッドグループ数*スレッド数)の下限が決定され、スレッドグループ数はoutputSize/numthreads(端数切り上げ)となる\n
	//! 指定が無い場合、MikuMikuDayoの実装ではデフォルト値として以下のように値が設定される
	//! -# outputSize.dimension == 1 ならば (1024,1,1)
	//! -# outputSize.dimension == 2 ならば (16,16,1)
	//! -# outputSize.dimension == 3 ならば (8,8,8)\n
	//! .
	//! DirectX12の仕様上、スレッドグループ数の上限が65535であるため、デフォーマの処理できる最大頂点数は65535*1024となり、この数が1つのモデルに含めることのできる頂点数の上限
	//! 
	//! @par conditionsについて
	//! パスの起動条件を記述できる。conditionsが空の場合は常時パスは起動される\n
	//! @code
	//! "conditions" : [ "OnStart" ]
	//! @endcode
	//! この場合、OnStart(再生・出力を開始直後のフレームの時1、それ以外では0)が真の時のみパスが起動される\n
	//! C++同様、真とは式を評価した結果が0以外の場合、偽とは0の場合を指す\n
	//! 使用できる変数は以下の通り
	//!	- hlsl/cb.hlsliにある、constant bufferに格納されている変数
	//!	- エフェクト内で定義されているコントローラ変数のうち、typeが"float"または"int"であるもの
	//! .
	//! \n
	//! 条件文を複数書いた場合、全ての文が真の時のみパスが起動される
	//! @code
	//! "conditions" : [ "OnStart", "OnLoad" ]
	//! @endcode
	//! この場合、OnStartが真、かつOnLoad(いずれかのエフェクトが読み込まれた直後のフレームでは1、それ以外では0)が真の時にパスが起動される\n\n
	//! conditionsには式を用いることが出来る\n
	//! @code
	//! "conditions" : [ "OnStart || OnLoad" ]
	//! @endcode
	//! この場合、OnStartが真、またはOnLoadが真の時にパスが起動される
	//! 
	struct FXPass {
		//! パス名。マクロYRZ_PASS_nameを定義してコンパイラを起動する
		std::string name;
		//! パスの種類。"postprocess", "compute", "raytracing", "rasterizer", "copy", "clearRTV", "clearUAV", "mipmapgen"のいずれか
		std::string type = "postprocess";
		//! vertexShaderのエントリポイント名(postprocess, rasterizerパス用)
		std::string vertexShader;	
		//! pixelShaderのエントリポイント名(postprocess, rasterizerパス用)
		std::string pixelShader;
		//! computeShaderのエントリポイント名(computeパス用)
		std::string computeShader;
		//! 1スレッドグループあたりのスレッド数 YRZ_NUMTHREADSマクロの内容を定義する
		XMUINT3 numthreads = { 0,0,0 };
		//! raytracingShaderのエントリポイント名
		std::string raygenShader;
		//! raytracingShaderのエントリポイント名
		std::vector<std::string> missShader;
		//! raytracingShaderのヒットグループの定義
		std::vector<FXHitGroup> hitGroup;
		//! raytracingShaderのエントリポイント名
		std::vector<std::string> callableShader;
		//! raytracingShaderのペイロードの最大サイズ[byte]
		UINT maxPayloadSize = 64;
		//! raytracingShaderの交差判定用構造体の最大サイズ[byte]
		UINT maxAttributeSize = 8;
		//! raytracingShaderの最大再帰段数
		UINT maxRecursionDepth = 4;
		//! vertexShaderのシェーダーモデル
		std::string vsTarget = "vs_6_1";
		//! pixelShaderのシェーダーモデル
		std::string psTarget = "ps_6_1";
		//! computeShaderのシェーダーモデル
		std::string csTarget = "cs_6_1";
		//! raytracingShaderのシェーダーモデル
		std::string libTarget = "lib_6_3";
		//! パスのコンパイル時に定義されるマクロ, MACRO=value の形式で=区切りでマクロを指定する
		std::vector<std::string>macros;		
		//! シェーダの出力サイズの指定
		FXSize outputSize = {};
		//! RTVでアクセスされるテクスチャ群
		std::vector<FXRTV>RTV;
		//! UAVでアクセスされるリソース群
		std::vector<FXUAV>UAV;
		//! DSVでアクセスされるテクスチャ(最大1つ)
		FXDSV DSV;
		//! 実行時の条件式。要素数が1以上の時、式がすべて真ならばパスが実行される
		std::vector<std::string> conditions;
		//! αブレンディングについての設定(rasterizerパス・postprocessパス用)
		FXBlendDesc blendDesc;
		//! プリミティブの描画方法についての設定(rasterizerパス用)
		FXRasterizerDesc rasterizerDesc;
		//! デプスステンシルバッファの使用法についての設定(rasterizerパス用)
		FXDepthStencilDesc depthStencilDesc;
		//! プリミティブの種類についての設定(rasterizerパス用) D3D12_PRIMITIVE_TOPOLOGY_TYPEを参照
		std::string primitiveTopologyType = "triangle";
		//! rasterizerパス用(現在のMikuMikuDayoの実装では未対応)
		UINT sampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		/**
		* ラスタライザのターゲットになるモデル(デフォーマ・ポストプロセス用)\n
		* "all":シーン内のモデルとエフェクト起動元に割り当てられたモデル\n
		* "other":シーン内のモデル\n
		* "self":エフェクト起動元に割り当てられたモデルのみ\n
		* "buffer":rasterVB、rasterIBで指定されるリソースによって定義されるモデル
		*/ 
		std::string rasterModelTarget = "all";
		//! rasterModelTarget=="buffer"の場合に使われるVertexBufferとなるリソース名
		std::string rasterVB;	
		//! rasterModelTarget=="buffer"の場合に使われるIndexBufferとなるリソース名
		std::string rasterIB;
		//! rasterVB, rasterIBがセットされている時はこのレイアウトを使う。空の場合はraterIBのみ参照される
		std::vector<FXInputElementDesc> layout;	

		bool IsFunctional() const { return StringToFXPassType(type) >= FXPassType::copy; }	//ファンクショナルパスか判定

		//! コピー元リソース名 copyパス用 コピー元とコピー先のリソースのサイズ・フォーマット・mipレベル数は同一でなければならない
		std::string src;
		//! コピー先リソース名 copyパス用 コピー元とコピー先のリソースのサイズ・フォーマット・mipレベル数は同一でなければならない
		std::string dest;
		//! 対象リソース名 clearRTV, clearUAV, mipmapgenパス用
		std::string target;
		//! クリアする場合に使用する値 clearRTV, clearUAVパス用
		XMFLOAT4 value = { 0,0,0,1 };
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name));
			CEREOPT(type)
				CEREOPT(vertexShader) CEREOPT(pixelShader) CEREOPT(computeShader)
				CEREOPT(raygenShader) CEREOPT(missShader) CEREOPT(hitGroup) CEREOPT(callableShader)
				CEREOPT(maxPayloadSize) CEREOPT(maxAttributeSize) CEREOPT(maxRecursionDepth)
				CEREOPT(vsTarget) CEREOPT(psTarget) CEREOPT(csTarget) CEREOPT(libTarget)
				CEREOPT(macros) CEREOPT(outputSize)
				CEREOPT(RTV) CEREOPT(UAV) CEREOPT(DSV) CEREOPT(conditions)
				CEREOPT(blendDesc) CEREOPT(rasterizerDesc) CEREOPT(depthStencilDesc)
				CEREOPT(primitiveTopologyType) CEREOPT(sampleMask)
				CEREOPT(src) CEREOPT(dest) CEREOPT(target) CEREOPT(value)
				CEREOPT(rasterModelTarget) CEREOPT(rasterVB) CEREOPT(rasterIB)
				CEREOPT(numthreads) CEREOPT(layout)
		}
	};

	class FX;

	//材質内から参照されるコントローラについての情報
	//PMXのモーフのみ対応(ボーンとかの有無については得られない)
	struct FXMatDescController {
		std::string name;	//変数名
		std::string controllerName;	//コントローラのファイル名(フォルダ名抜き、拡張子込み)
		std::string item;	//モーフ名
		int doppelIndex = 0;		//重複したコントローラの場合は何台目か

		void Setup(FX& fx, int selfIndex);	//↑の情報とエフェクトの情報から↓の情報をセットアップする, valueBufに結果を格納するバッファを入れる
		//以下、↑から導かれる情報を格納する変数
		int modelIndex = -1;	//モデル番号
		int itemIndex = -1;		//アイテム番号
		std::shared_ptr<float> value;			//値を格納するバッファ
		//現在の値をvalueに格納させる
		void Update(FX& fx);
	};


#if 0
	//MatDescの式でサポートされている関数。関数名→関数番号・引数の数 辞書
	//select(a,b,c) ... aが0以外ならb, 0ならcが返る関数
	//bit(a,b) ... (int)aのbビットの値を0か1で返す
	std::unordered_map<std::string, std::tuple<int,int>> SupportedFunctions = {
		{"sin",{1,1}}, {"cos",{2,1}}, {"tan",{3,1}}, {"asin",{4,1}},
		{"acos",{5,1}}, {"atan",{6,1}}, {"exp",{7,1}}, {"log",{8,1}},
		{"sqrt",{9,1}}, {"saturate", {10,1}}, {"frac",{11,1}}, {"floor",{12,1}},
		{"ceil",{13,1}}, {"lerp", {14,3}}, {"bit", {15,2}},
		{"hsvR", {16,3}},{"hsvG", {17,3}}, {"hsvB", {18,3}},
		{"select", {19,3}}, {"clamp", {20,3}}, {"min", {21,2}}, {"max", {22,2}},
		{"step", {23,2}}, {"smoothstep", {24,3}}, {"hash", {25,1}}, {"noise", {26,1}}
	};
	std::unordered_map<std::string, std::string> SupportedConstants = { {"pi", "3.141592653589793"}, {"e","2.718281828459045"} };

	//式の要素(オペランドと演算子)
	//charの場合は演算子, floatの場合は数値リテラル、intの場合はConstantBuffer内のオフセット(コントローラの値)
	struct FXOpOp {
		enum t { none, opr, val, locon, contofs, cbofs, fun };	//何を格納しているか？opr=演算子、val=リテラル、locon = 材質注釈内で定義されたコントローラ contofs=コントローラのオフセット, cbofs=CBのオフセット fun = 関数
		t type = none;
		char op = 0;
		float value = 0;
		int offset = 0;
		bool isint = false;	//CBに入っている値はint型である
		int func = -1;	//SupportedFunctionの中の何番の関数か?
		int funcpop = 1;	//関数だった場合何変数関数か？
		//以下、locon用
		int iController = -1;	//コントローラモデル番号
		int iItem = -1;	//モーフ番号
		int selfIndex = -1;	//(self)に該当するモデル番号(マテリアルが割り当てられているモデルの番号)

		//文字列をパース。成功したらtrue
		bool Parse(const std::string& s, const FX& fx, float defval, const std::vector<FXMatDescController>& ctrls, int selfIdx);
		
		//関数番号を文字列から得る(どの関数にも該当しない場合0が帰る)
		static int FuncIndex(const std::string& s) {
			if (SupportedFunctions.contains(s)) {
				auto [idx, num] = SupportedFunctions[s];
				return idx;
			}
			return 0;
		}
		//関数の実行(idxで指定される番号で実行する関数を決める)
		static float Func(int idx, float v1, float v2 = 0, float v3 = 0);
	};

	//式プロセッサ…MatDescのvalues[]の1要素を担当する。式を評価して値を格納する
	struct FXExpr {
		int ofs = 0;			//値配列内でのオフセット
		bool empty = true;		//空の式か？
		bool isConst = true;	//定数式か？
		float value = 0;		//定数式の場合はその値(定数式でない場合はデフォルト値が入る)
		std::vector<FXOpOp> expr;	//入力された式を逆ポーランド記法のトークン列に分解した物
		bool Eval(const FX& fx, float& ret);	//式を評価して値を得る。成功すればtrue
		//strから式を作る
		//selfIndex : マテリアルを割り当てているモデル番号
		//edict : この値用の列挙子辞書
		bool Parse(const std::string& str, const FX& fx, int _ofs, const std::vector<FXMatDescController>& ctrls, int selfIndex, const std::unordered_map<std::string, float>* edict);
	};
#endif

	struct FXExpr {
		Expr::Expr e;
		int ofs = 0;	//値配列内でのオフセット
	};


	//エフェクトに対して各材質ごとに注釈を入れるためのデータ。FXファイルの中には含まれない
	// ホストアプリケーションのUIのエフェクトダイアログ的な物から選択・セットされる
	// シリアライズについてはホストアプリケーションに委ねる
	// 同じエフェクトに対するvalues,texturesそれぞれの要素数は全ての材質注釈で同じ(templateで規定される)
	// exprsの要素数は材質注釈ごとに異なる(必要な式の分だけ確保される)
	struct FXMatDesc {
		std::string sourceFile;					//格納されていたファイル名
		std::vector<float> values;				//値配列
		std::vector<std::string>textures;		//使用する追加テクスチャ。使用しない場合は空文字が入る。フラグを格納する都合上、1つの材質注釈当たり最大64枚まで
		std::vector<std::string>textures3D;		//↑の3D版
		std::vector<FXExpr> exprs;				//コントローラの値を含んだ式(values[exprs[i].ofs] = exprs[i].Eval(...)として毎フレームの値を更新できる)
		std::vector<FXMatDescController> controllers;
	};

	//↑を取りまとめるためのモノ。1エフェクトにつき最大1つ設定される
	// モデルの増減・エフェクトの読み込み時に構築・更新される
	//! 材質注釈についての設定
	struct FXMatDescStorage {
		/** hlslから材質注釈へアクセスするための構造体名のプレフィクス\n
		* MikuMikuDayoではMaterialウィンドウのタブに追加される名前\n
		* 空文字の時は材質注釈のないエフェクトと見なされる
		*/
		std::string name;
		//! 材質注釈のテンプレートファイル。エフェクトファイルの存在するパスを基準とする相対パス
		std::string matDescTemplate;
		//! デフォルトの材質注釈ファイル。エフェクトファイルの存在するパスを基準とする相対パス
		std::string defaultFile;

		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(name), cereal::make_nvp("template", matDescTemplate), CEREAL_NVP(defaultFile));
		}

		//! \cond
		//以下はFXファイルには格納されない
		std::vector<std::vector<FXMatDesc>> items;
		
		//総材質数を返す
		int Count() const {
			int r = 0;
			for (auto v : items)
				r += v.size();
			return r;
		}
		//! \endcond
	};

	//ソースファイル名だけのバックアップ
	struct FXMatDescBackup {
		std::vector<std::vector<std::string>>sourceFiles;
		void Backup(const FXMatDescStorage& md);
		template<class Archive>
		void serialize(Archive& A) {
			A(CEREAL_NVP(sourceFiles));
		}
	};

	/*シェーダからmatDescにどうやってアクセスするか？

	①物体IDと材質番号の取得
	  ※ポストプロセスからはこれからシェーディングされるパッチを作る物体と材質は何か？という情報を得るためにはそういうマップを作っておけば良い
	  ※レイトレーシングからはそのまま拾える。BLASに振られているIDと、facebufferに格納された材質番号をそのまま使う

	②hlsl側に作られるオブジェクトによりアクセス。レジスタ割り当てはFX::Load時に決める
	　レジスタ自体はモデルの増減では変化せず、バッファの内容だけ更新される
	　エフェクト本体のhlslには以下のStructuredBufferまたはTexture2Dが作られる
	  インデックス取得用のStructuredBuffer<uint>が作られる

	 <name>_idx[モデル数]
	　<name>_idx[モデル番号]で各モデルの材質0番に対する以下2つの配列内のモデル毎の先頭番地imoが得られる
	  imo+材質番号で注目材質の情報を格納する番地imatを得て、

	  ival = imat*values.size()
	  itex = imat*textures.size() として各value, textureへアクセスするための番地を得る

	  以下の2つの配列から、値、またはテクスチャへアクセスできる
	 a. StructuredBuffer <name>_value[ival]<<name>Value>	... <name>Valueという構造体が作られる
	 b. Texture2D <name>_texture[itex]<float4>

	 ※例えば0:albedo、1:normal、2:roughnessと3つの「サブインデックス」が割り振られたテクスチャが使われる事を想定した場合
	 　必ずしも全材質に3つのマップが用意されている訳では無いので、テクスチャには有効なテクスチャと無効なテクスチャができる
	  <name>_texture配列には有効・無効に関わらずオブジェクト自体は格納されているが、有効・無効のどちらかをシェーダから判定する必要がある
	  そのための情報が↓

	4.<name>_tflg[総材質数]<uint64_t>
	<name>_tflag[imat]でビットbが1の時、サブインデックスbのテクスチャが格納されているという事を示す(最大64枚まで可)
	例として、先の0～2のサブインデックスのテクスチャ群を想定する場合
	 0000 ... どのマップも無効
	 0001 ... albedoマップのみ格納されている
	 0111 ... albedo,normal,roughness全て格納されている

	 ③材質注釈テンプレートと材質注釈ファイル
	  以上のように、配列内のアドレスを直接扱うのはエフェクトのバージョンアップに伴う互換性などに乏しくなるので、
	  アドレスに名前を付けてアクセスできる仕組みを用意してある(後述の「材質注釈パーサ」の項を参照)
	  材質注釈ファイルを使う事を前提として、hlslには <name>Struct構造体と Get<name>Struct関数が生成される

	  材質注釈テンプレートに値番号と値変数名、テクスチャ番号とテクスチャ変数名の対応付けを記述する

	  例："f.4:value; _T0:AlbedoMap;"

	  この場合、以下のような<name>Value,Texture構造体と Get<name>Value(),Texture()関数がfxファイルのコンパイル前にhlsl内に生成される

		struct ShaderToiToiValue {
		  float4 value;
		};
		struct ShaderToiToiTexture {
		  bool hasAlbedoMap;			//AlbedoMapが有効ならtrueが入る
		  Texture2D<float4> AlbedoMap;
		};

		ShaderToiToiValue GetShaderToiToiValue(uint ID, uint subID)
		{
		  ShaderToiToiValue m = (ShaderToiToiValue)0;
		  uint imat = ShaderToiToi_idx[ID] + subID;
		  return ShaderToiToi_value[imat];
		}
		ShaderToiToiTexture GetShaderToiToiTexture(uint ID, uint subID)
		{
		  ShaderToiToiTexture m = (ShaderToiToiTexture)0;
		  uint imat = ShaderToiToi_idx[ID] + subID;
		  uint tidx = imat * 1;
		  uint64_t tflg = ShaderToiToi_tflg[imat];
		  m.value = ShaderToiToi_value[vidx + 0].xyzw;
		  m.hasAlbedoMap = tflg & 1;
		  m.AlbedoMap = ShaderToiToi_texture[tidx + 0];
		  return m;
		}

		Get<name>Value(), Get<name>Texture() 関数に例えばモデル番号、材質番号などを指定して呼ぶことで、材質注釈データを得られる

	  個々の材質注釈データはホストアプリケーションから設定される
	  設定方法は
	  1.FX.Load時にFX.matDescs.defaultfileに指定された材質注釈ファイルが読み込まれる
	  2.

	 */


	 //材質注釈パーサ
	 //ParseTemplateでテンプレートを読み込んだ後に、Parseで各材質注釈ファイルの内容をvaluesとtexturesに入れる
	 //材質注釈で格納されるvaluesとtexturesそのままでは扱いづらいのと、アップデートに弱いので
	 //専用の材質注釈ファイル(MatDesc)を用いる　以下、文例
	 //MatDescで具体的な値を指定し、MatDescTemplateでどんな変数名でvalues[]とtextures[]のどこをアクセスするのか指定する

	 /* MatDescTemplate, MatDescの文法と文例

	 ●文法・語彙についての概説
	 1. ;または\nで行を区切る
	 2. ダブルクォート"～"で囲われた範囲外の半角スペース、タブなどの白文字は無視される
	 3. 行は : で左辺と右辺に区切られ、必ず左辺と右辺を1つずつ持つ
	 4. #から始まる行はコメントと見なされる
	 5. 文字のエンコードはUTF-8(BOMなし)のみ認める
	 6. シンボルの大文字と小文字は区別される

	 ●MatDescTemplate文例

	 #値テンプレートの定義
	 #左辺は 型.要素数 ... 右辺の変数を表す型を表現する。f.はfloat型 i.はint型 現状では、この2つのみサポート。その後は要素数を1～4で指定
	 #_から始まる変数名は特殊変数のためのキーワードとして予約されており、半角数字から始まる変数名はテクスチャレジスタ番号の指定のため予約されており使用禁止
	 f.3 : Albedo
	 f.1 : Flag
	 f.2 : IOR
	 f.2 : Roughness

	 #デフォルト値。テンプレート内にデフォルト値の記述が無い値の場合、デフォルト値は0と見なされる
	 #記述法はMatDescと同じ
	 IOR : 1.5,0.1

	 #テクスチャテンプレートの定義
	 #何番目のレジスタに割り当てられるテクスチャに、何という名前が割り当てられるか？
	 #値とカブらない名前にしなければならない
	 #命名規則は値と同じ
	 #_Tの後のテクスチャレジスタ番号は0～63の範囲にしなければならない(1つのMatDescで参照されるテクスチャは最大64枚)
	 _T0 : AlbedoMap
	 _T1 : NormalMap
	 _T2 : EmissiveMap
	 _T3 : RoughnessMap

	 #テクスチャ番号の後にmを付けると読み込まれた後に自動的にmipmapチェーンを作成する
	 _T4m : SubAlbedoMap

	 #デフォルト値。ダブルクォートは必須ではないが、半角スペースを含むファイル名の場合はダブルクォートで囲う必要がある
	 #相対パスの基準はエフェクトファイルのパス
	 #テンプレート内にデフォルトの記述が無いテクスチャの場合、デフォルトテクスチャは""(空文字、そのテクスチャは使用しない)と見なされる
	 _TNormalMap : "normalnormal.png"

	 #おすすめ材質キーワード。材質名に特定のキーワードが含まれると「recommends」ボタンを押した時に指定の材質注釈ファイルが設定される
	 #キーワード列は,で区切る
	 #2つ以上の異なるおすすめ材質にマッチする材質名を持つような材質については、先に現れた(若い行で定義された)おすすめ材質が選択される
	 #_Rで左辺が始まる場合はおすすめ材質の設定とみなされる。半角スペースを含むファイル名の場合はテクスチャ同様ダブルクォートで囲うと指定できる
	 #相対パスの基準はエフェクトファイルのパス
	 _Rmetal.txt : 金,鉄,metal,gold
	 _Rskin.txt : 肌,skin
	 _Remission.txt : 灯,light

	 #列挙子の定義	_E パラメータ名 : シンボル1[=定数1], シンボル2[=定数2] ... 
	 _E Category : default=0, glass=1, metal=2, sss=3
	 各パラメータについて、定数と置き換えられるシンボルを定義できる。この場合
	 Category : metal
	 と書くとCategory : 2 が指定された時と同じになる。
	_E AlbedoEstimation : mmd, diffuse, ambient
	定数部分を省略して↑のように記述した場合、mmd=0, diffuse=1, ambient=2と出現順に0,1,2...と値が割り振られる

	 */

	 /*
	 ●MatDesc文例  ... { }はいらない。書いちゃダメ。

	 Albedo : 0.1,0.5,1.0
	 Roughness : 1.0, 0.5
	 Flag : 5

	 _TAlbedoMap : "rgb.png"
	 _TNormalMap : "norma.jpg"
	 _TEmissiveMap : "emis.hdr"

	 ※相対パスの基準は個々のMatDescファイルのパス(エフェクトのパスではない)

	 MatDescではマクロ、コントローラの定義も行うことが出来るほか、数値に式を使うことが出来る
	 1.マクロの例 ... _Mで開始。lumというマクロは0.5に置換される
	 _M lum : 0.5
	 引数付きマクロ
	 _M variable(x) : (500*(x))
	 Light : variable(10) と書くと、Light : (500*10)と同じとみなされる。これは定数式として5000に変換される
	 ※マクロは文中に直接展開されるので項を書く場合は括弧で囲う方が良い

	 2.式の例 ... 四則演算、関数(sin,saturateなど)の使用、括弧による優先順位の指定が可能
	 Albedo : 0.1*lum,0.5+lum,1.0/lum

	 3.コントローラの定義の例 ... _Cで開始。dayoconという変数にdayo.pmxの"ダヨ"モーフの値が代入される(表情モーフのみ対応)
	 _C dayocon : dayo.pmx,ダヨ

	 #このようにすればLightパラメータをdayo.pmxの"ダヨ"モーフをコントローラとして制御できる
	 Light: dayocon*100



	 */
		
	 //材質注釈テンプレート読み込み補助用、変数1つについての情報
	struct FXMatDescVar {
		int order = -1;		//構造体内の順序(unordered_mapだと順不同になるので)
		int offset = -1;	//構造体内の位置(0スタート、1でfloat1個分)
		int type = 0;		//0:float 1:int
		int nComp = -1;		//要素数
		XMFLOAT4 defval = { 0,0,0,0 };  //デフォルト値

		static std::string TypeStr(int t) {
			if (t == 1) {
				return "int";
			}
			return "float";
		}
	};



	//材質注釈パーサ
	struct FXMatDescParser {
	private:
		FX* m_fx;	//このパーサを呼び出しているエフェクトオブジェクト
		//文字列操作補助ルーチン
		std::vector<std::string>split(const std::string& s, char tok);	//tokでsを分解してベクタに返す
		std::string deleteWhite(const std::string& str);	//白文字の除去
		std::vector<std::vector<std::string>> getLR(const std::string& str);	// : で左辺と右辺に分解
		void parseval(
			const std::vector<std::string>& l, std::vector<float>& values, std::vector<FXExpr>* exprs,
			std::vector<FXMatDescController>* ctrls, bool isTemplate = false, int selfIndex = -1);	//values配列を解釈、式があればexprsに追加
		std::string delDoubleQuote(const std::string& str);	//ファイル名の両端の " をあれば除去
	public:
		//ParseTemplateによってセットされる変数群
		int nVal = 0;   //valuesの要素数
		int nTex = 0;
		int nTex3D = 0;
		std::unordered_map<std::string, FXMatDescVar> varmap;	//変数名→変数情報マップ
		std::unordered_map<std::string, int> texmap;	//テクスチャ名→テクスチャ番号マップ
		std::unordered_set<int> mip;			//この集合の中にテクスチャ番号が入っている場合、ミップマップ有効
		std::unordered_map<std::string, int> texmap3D;	//3Dテクスチャ名→3Dテクスチャ番号マップ
		std::unordered_set<int> mip3D;			//この集合の中に3Dテクスチャ番号が入っている場合、ミップマップ有効
		std::vector<float> defaultVal;			//変数のデフォルト値
		std::vector<int> valTypes;				//↑の各要素の型(0:float, 1:int)
		std::vector<std::string> defaultTex;	//変数のデフォルト値
		std::vector<std::string> defaultTex3D;	//変数のデフォルト値
		std::unordered_map<std::string, std::string> recommends;	//キーワード→材質注釈ファイル
		std::unordered_map<std::string, std::unordered_map<std::string, float>> enumdict;	//各パラメータの列挙子

		//strに格納された材質注釈テンプレートを読み込む
		//fxpathはデフォルトテクスチャの基準パス(読み込み元のエフェクトの格納されたディレクトリ)
		void ParseTemplate(const std::string& str, const fs::path& fxpath, FX* fx);
		FXMatDesc Parse(const std::string& str, int selfIndex);	//材質注釈の読み込み
		std::string Generate(const std::vector<float>& values, const std::vector<std::string>& textures);	//変数・テクスチャの状態から逆に材質注釈テキストを生成する
	};




	enum class FXCategory { none = -1, postprocess = 0, deform = 1, render = 2 };

	FXCategory StringToFXCategory(const std::string& category) {
		static const std::unordered_map<std::string, FXCategory> categoryMap = {
			{"NONE", FXCategory::none},
			{"POSTPROCESS", FXCategory::postprocess},
			{"DEFORM", FXCategory::deform},
			{"RENDER", FXCategory::render}
		};
		return StringToEnumX(categoryMap, category, FXCategory::none);
	}

	/**
	 * @struct FXTech
	 * @brief ★★★エフェクト全体の定義★★★
	 */
	struct FXTech {
		//! エフェクトのカテゴリ。"postprocess", "deform", "render"のいずれか
		std::string category = "postprocess";
		//! コントローラ
		std::vector<FXController> controllers;
		//! サンプラー
		std::vector<FXSampler> samplers;
		//! テクスチャ
		std::vector<FXTexture> textures;
		//! 3Dテクスチャ
		std::vector<FXTexture3D> textures3D;
		//! バッファ
		std::vector<FXBuffer> buffers;
		//! パス 定義された順で実行される
		std::vector<FXPass> passes;
		//! HLSLのコードに直接出力される文字列群
		std::vector<std::string> code;	
		//! 材質注釈
		FXMatDescStorage matDescs;
		//! シェーダ内のグローバル変数を格納する「暗黙のコンスタントバッファ」のサイズ[byte]
		UINT globalVarSize = 1024;
		/** ホストアプリケーションに伝える情報\n
		* MikuMikuDayoが対応しているのは以下の通り\n
		* "skyboxsampler" skyboxが読み込まれた際にサンプリング用のskywalkerオブジェクトを用意する\n
		* "skyboxprefilter" skyboxが読み込まれた際、下位mipレベルにprefiltered specular envmapを格納する
		*/
		std::vector<std::string>memos;

		template<class Archive>
		void serialize(Archive& A, std::uint32_t const version) {
			CEREOPT(category) CEREOPT(controllers) CEREOPT(samplers) CEREOPT(textures) CEREOPT(textures3D) CEREOPT(buffers) CEREOPT(passes) CEREOPT(matDescs)
				CEREOPT(globalVarSize) CEREOPT(code) CEREOPT(memos)
		}
		//以下はLoad時に決定する(ホストアプリケーション側で設定されるLoadConfigに依存)
		//! \cond
		bool defaultOutputDependency = true;			//画面のリサイズが行われた時にdefaultOutputSizeは変更されるか？
		int defaultOutputDimension = 2;					//出力先のデフォルトの次元数(デフォーマの場合は1がセットされる)
		XMUINT3 defaultOutputSize = { 1280, 720, 1 };	//テクスチャや出力先のデフォルトのサイズ, デフォーマの場合はconfig.deformOutputCountからセットされる
		//! \endcond
	};


	//コンパイル済みシェーダキャッシュのうち1アイテム
	struct FXShaderCacheItem {
		bool loaded = false;	//ディスクからcompiledが読み出されてセットされました、フラグ
		Shader compiled;
		fs::file_time_type timestamp;
	};

	//コンパイル済みシェーダキャッシュ
	class FXShaderCache {
	private:
		fs::path m_path;
		std::unordered_map<size_t, FXShaderCacheItem> m_cache;

		//ハマった話 … 
		// なぜかCompileShaderMemory時にCLSID_DxcUtilsに0が入り、DxcCreateInstanceが失敗するようになってしまった
		// その時に、m_IHを定義しないようにして、下の&m_IHにnullptrをセットしてインクルードを失敗させたあと、またm_IHを定義するようになしたらなぜかOKになった
		// CustomIncludeHandlerの内部で定義されるpDxCUtilsが実はいけない？？ともかく、インクルードハンドラの寿命は短い方が無難らしい
		//CustomIncludeHandler m_IH;	//←これやめてコンパイルが必要な時に都度生成・破棄するようにした

		//キャッシュに登録されているシェーダの事を思い出す
		void RecallItems();

	public:
		//ファイルへのキャッシュは使わず、起動後にコンパイルされたシェーダのみキャッシュする
		FXShaderCache() {};
		//cachepath下にキャッシュファイルを保管し、ファイルを使ったキャッシュを有効化する
		FXShaderCache(fs::path cachepath) { m_path = cachepath; RecallItems(); }

		// メモリ上のpShaderSource とサイズ sizeInBytesを指定して必要があればコンパイルする
		// 更新日の基準を取るためにメモリの元になったファイルsourcefileを指定する
		Shader Load(DXR& dxr, void* pShaderSource, size_t sizeInBytes, const std::wstring& includePath,
			const std::wstring& sourcefile, const std::wstring& entrypoint, const std::wstring& target, const std::vector<std::wstring>& options);

		//メモリキャッシュのみ消去。キャッシュファイルはそのまま
		void Flush() { m_cache.clear(); }
	};


	//材質注釈用テクスチャキャッシュ
	template <class T>
	struct FXTexCache {
	private:
		DXR* m_dxr;
		T m_dmyTex;
		std::unordered_map<fs::path, T> mipcache;	//mipmap付きテクスチャ用キャッシュ
		std::unordered_map<fs::path, T> cache;		//mipmapなしテクスチャ用キャッシュ
	public:
		void Init(DXR& dxr, T dmyTex) { m_dxr = &dxr; m_dmyTex = dmyTex; }
		
		//キャッシュからテクスチャを得る
		// テクスチャ読み込み時にエラーが出た場合、ダミーテクスチャが返り、okにfalseが入る
		T Get(const fs::path path, bool mip, bool& ok) {
			ok = true;
			
			std::unordered_map<fs::path, T>& c = mip ? mipcache : cache;

			if (!c.contains(path)) {
				T tex;
				try {
					if constexpr (std::is_same<T,Tex2D>::value)
						tex = m_dxr->CreateTex2D(path.wstring().c_str(), ColorSpace::linear, D3D12_HEAP_FLAG_NONE, mip);
					else if constexpr (std::is_same<T,Tex3D>::value)
						tex = m_dxr->CreateTex3D(path.wstring().c_str(), ColorSpace::linear, D3D12_HEAP_FLAG_NONE, mip);
				} catch (std::exception ex) {
					ok = false;
					return m_dmyTex;
				}
				c[path] = tex;
			}
			return c[path];
		}
		
		void Clear() { cache.clear(); mipcache.clear(); }
	};

	class FX;	//前方参照

	//読み込み時にパスを作る際に用いるパラメータ
	struct FXLoadConfig {
		//各シェーダのコンパイルターゲット
		std::vector<std::wstring> compileOption;	//コンパイル時のオプション
		UINT SRVspace = 0;				//SRVに入れる物を作るスペース番号
		UINT SRVspace3DTex = 1;			//SRVに入れる物を作るスペース番号(MatDescの3DTexture用)
		Pass* definedPass = nullptr;	//定義済みリソースへの参照を格納したパス。このパスから参照されるリソースにjsonに記述されたリソースが追加されてhlslから利用できる
		RTVItem defaultRT = {};			//デフォルトのレンダーターゲットの指定。texがnullptrの時、バックバッファと見なされる
		DSVItem defaultZBuf = {};		//デフォルトのデプスステンシルバッファ。texがnullptrの時、デプスステンシルバッファは使わない
		//以下、ラスタライザ専用
		std::vector<Buf*> rasterVB;				//ラスタライザパスに渡す頂点バッファ
		std::vector<Buf*> rasterIB;				//ラスタライザパスに渡すインデックスバッファ
		std::vector<FXInputElementDesc>	layout;	//頂点レイアウトの情報
		//描画順:i番目にレンダリングされるVB,IBの番号がrasterOrder[i]に書かれている
		//ラスタライザを用いる場合はこの値が空のままだと何も描画されないので注意！
		//モデルの表示/非表示の状態に合わせて毎フレーム書き換えて使う
		std::vector<int> rasterOrder;			
		//以下、コンピュートパス用
		XMUINT3 defaultNumthreads[3] = { {1024,1,1}, {16,16,1}, {8,8,8} };	//出力の次元数ごとのデフォルトnumthreads
		//以下、デフォーマ専用
		size_t deformOutputCount = 0;	//要素数のデフォルト値
		int deformIndex = 0;	//デフォームの対象になるモデル番号…rootConst[2]にセットされる
		int deformOrder = 0;	//何番目のデフォーム処理か？…rootConst[3]にセットされる
		//以下、材質注釈用
		std::vector<UINT>materialCount;	//各モデルの材質数。読み込み前には材質注釈について何もわからない時に使う
		FXMatDescBackup mdb;			//材質注釈復元用。プロジェクトのロード時、再コンパイル時など、読み込み前にエフェクトの材質ファイルが何か分かっている場合に使う
		std::vector<FX*>deformers;		//各モデルを処理するデフォーマ。デフォルトデフォーマの場合はnullptrを指定する

		//MatDescからCBの中身にアクセスするための情報
		char* pCB;	//CBの中身へのポインタ
		std::unordered_map<std::string, std::pair<int,char>> CBPointers;	//変数名→<オフセット、型(0:float 1:int)>辞書
		//コントローラ名とコントローラアイテム名、型名からそれぞれのインデックスを得るためのコールバック
		//selfIDはコントローラ名(self)に対応するモデル番号、0未満が指定された場合(self)に該当するモデル無しとする
		//doppelIndexは重複したコントローラの幾つ目のコントローラか？
		std::function<void(int selfIndex, int doppelIndex, const std::string& controllerName, const std::string& item, const std::string& type, int& modelIndex, int& itemIndex)>ControllerQuery;
		//コントローラの値を得るためのコールバック
		std::function<void(void* value, int iModel, int iItem, const std::string& type)>ControllerCallback;	
		//構造体のサイズを得るためのコールバック
		std::function < UINT(const std::string& type)> ElemSizeQuery;
		int selfIndex = -1;	//(self)の識別用。(self)はモデル番号の何番か？
		//matdescから参照できるscreen.bmpなど予約された名前に対するリソース
		std::unordered_map<std::string, YRZ::Res*>reservedMDRes;
	};

	//共有リソースについての情報を格納する。シリアライズ・デシリアライズの必要はない
	//共有されるリソースそのものの事を「共有源」、共有されるリソースへの参照を「共有参照」と呼ぶ
	//共有源を定義するエフェクトを「共有元」、共有参照を定義するエフェクトを「共有先」と呼ぶ
	// 
	//共有リソースについてのルール
	//1.共有源に書き込みを行う事が出来るのは共有元のみ。このため共有先において共有参照はsrvにしか入らない
	//2.共有参照では、サイズの指定、内容を充填するためのファイル名の指定はいずれも無視される
	//3.共有参照をリソースのサイズの参照元(size.base)に指定することはできない
	struct FXSharedItem {
		std::string name;
		ResType type;
		int index;
	};

	//共有情報タグ
	//共有参照によるリソースが各パスオブジェクトのSRVのどこに格納されているかを示す
	//リソースの更新時に共有元が更新された場合に追従するために用いる
	struct FXShareTag {
		std::string name;
		UINT reg = 0, space = 0;
	};

	//パス起動条件を解析した物
	//コントローラ変数またコンスタントバッファ上のスカラー変数と、オペランドとの比較式を欠ける
	struct FXPassCond {
		bool always = true;	//無条件に実行するならtrue
		enum { tBool, tInt, tFloat  };	//CB上に置かれた定数の型
		//演算子
		//bool型の場合はopIsまたはOpNegateのみ可
		enum { opEqual, opNeq, opLess, opGreater, opLeq, opGeq, opIs, opNegate };	
		int type = tBool;
		int op = opIs;
		int operandI = 0;	//比較対象になる値
		float operandF = 0;	
		int ControllerCBoffset = -1;	//コントローラの有無を格納したCB上のオフセット
		int CBoffset = -1;				//コンスタントバッファ上のオフセット
	};

	const char* JSONStartSign = "[YRZFX]";
	const char* JSONEndSign = "[HLSL]";

	class FXWatcher;	//前方参照

	//1個のjson+hlslファイルで表現される一連のマルチパスレンダリングを実行するクラス
	//内部的にはFXTech構造体にデシリアライズされた情報を一旦格納し、そこからよろずDXR用の各バッファ、テクスチャを生成し、hlslコードの追加を行う
	class FX {
	private:
		std::wstring m_filename;	//最後にLoadされたエフェクトファイル名
		fs::path m_path;			//エフェクトファイルの格納されているディレクトリ。ここからの相対パスとして全てのファイルI/Oを行う
		DXR* m_dxr;
		Buf m_dmyBuf;				//uavとsrv同時アクセスを防ぐ時などに使う空のバッファ
		Tex2D m_dmyTex;				//空のテクスチャ
		Tex3D m_dmyTex3D;			//空のテクスチャ
		FXWatcher* m_watcher;		//このエフェクトを格納しているFXWatcher
		FXShaderCache* m_cache;
		FXTexCache<Tex2D> m_texCache;	//材質注釈用テクスチャキャッシュ
		FXTexCache<Tex3D> m_texCache3D;	//材質注釈用テクスチャキャッシュ
		FXTech m_tech;			//読み込み元ソース
		std::unordered_map<std::string, int>texturemap, bufmap, texture3Dmap;	//リソース名→テクスチャ番号マップ

		const int matDescTexChunkSize = 256;	//材質注釈テクスチャ何種類ごとにrootsigとPSOを作り直すか？
		const int matDescTex3DChunkSize = 64;
		int m_MDTexChunkSize = matDescTexChunkSize;		//現在の材質注釈テクスチャ用のディスクリプタの数
		int m_MDTex3DChunkSize = matDescTex3DChunkSize;

		//出力先のリサイズがそれぞれのリソースのサイズに影響するか？
		std::vector<bool>texture3DSizeDependency, textureSizeDependency, bufferSizeDependency, outputSizeDependency;
		//リサイズ用情報、各サイズの元の値(相対値なら相対値のまま)
		std::vector<FXSize>texture3DSize, textureSize, bufferSize, originalOutputSize;
		//jsonヘッダの読み込み filenameは読み込み元のファイル名
		template <class T>
		static std::string ReadHeader(const std::string& filename, std::istream& ifs, const std::string& rootname, T& info);

	public:
		fs::path BasePath() const { return m_path; }	//エフェクトファイルの格納されているディレクトリ
		fs::path filename() const { return m_filename; }
		FXCategory category = FXCategory::none;
		FXLoadConfig usedConfig;		//最後にLoadされた時のパラメータ
		std::string compiledHLSL;		//コンパイラに渡されたhlslソース

		Buf& DmyBuf() { return m_dmyBuf; }
		Tex2D& DmyTex() { return m_dmyTex; }
		Tex3D& DmyTex3D() { return m_dmyTex3D; }

		//コントローラ
		std::vector<FXController>controllers;
		CB globalCB;		//グローバル変数を格納するためのCB
		CB controllerCB;	//コントローラの値を格納するためのCB

		//リソース
		std::vector<D3D12_STATIC_SAMPLER_DESC>samplers;
		std::vector<Tex2D>textures;
		std::vector<Tex3D>textures3D;
		std::vector<Buf>buffers;
		std::unordered_map<std::string, Res*>resMap;	//名前から割り当て済みリソースを検索するマップ

		Expr::Compiler conditionCompiler;	//パス起動条件用コンパイラ
		Expr::Compiler mdCompiler;			//材質注釈用コンパイラ

		//パスごとの情報
		std::vector<FXSize>outputSize;		//各パスの出力サイズ(絶対値に変換済みとする)
		std::vector<std::string>passNames;	//各パスの名前
		std::vector<Pass>passes;
		std::vector<FXFunctionalPass>funcPasses;
		std::vector<FXPassType>passOrder;			//FXPassType::copy未満ならpassesから、以上ならfuncPassesから実行する、という意味のオーダリングテーブル
		std::vector<std::vector<FXShareTag>>shareTags;	//パスが用いる共有参照についての情報、i番目のパスのj番目の共有参照はshareTags[i][j]で読み書きする
		std::vector<FXRasterModelTarget>passRamos;	//ラスタライザの場合の描画対象モデル
		std::vector<Buf*>passVB, passIB;	//ラスタライザかつramo==bufferだった場合の描画対象
		std::vector<std::vector<Expr::Expr>>passCond;	//パスの起動条件
		std::vector<std::vector<FXInputElementDesc>>customLayout;	//カスタムレイアウトを使うラスタライザパスだった場合のレイアウト

		//共有
		std::vector<FXSharedItem> sharedSrc;	//このエフェクトが定義しているsharedリソース
		std::vector<FXSharedItem> sharedRef;	//このエフェクトが参照しているsharedリソース

		//材質注釈
		FXMatDescStorage matDescs;				//材質注釈の配列 ※モデルの増減の都度更新する必要がある ※ソースの再コンパイルの際はwatcherにバックアップされる
		Buf table_idx;							//材質注釈を検索するためのテーブル
		Buf matDescValues, matDescValuesCPU;	//材質注釈に含まれる値バッファとCPU側バッファの組み
		std::uint8_t* pMatDescValusCPU = nullptr;	//↑をMapして得たポインタ
		std::vector<Tex2D>matDescTextures;		//材質注釈に含まれるテクスチャ配列
		std::vector<Tex3D>matDescTextures3D;	//材質注釈に含まれる3Dテクスチャ配列
		std::vector<Tex2D>matDescUniqTex;		//材質注釈に含まれるテクスチャ群からユニークな要素を抽出した配列
		std::vector<Tex3D>matDescUniqTex3D;		//材質注釈に含まれる3Dテクスチャ群からユニークな要素を抽出した配列
		Buf table_tex, table_tex3D;				//ユニークな材質注釈テクスチャを検索するためのテーブル
		std::vector<int> matDescTexReg;			//各パス(functionalPass除く)における材質注釈のSRV上でのインデックス

		FXMatDescParser parser;				//材質注釈のテキストから具体的なmatDescs.itemを生成するオブジェクト
		bool hasMatDesc() const { return !matDescs.name.empty(); };	//このエフェクトには材質注釈が必要か？

		//レンダラ固有情報
		std::vector<std::string>memos;

		//メソッド群table_tflg
		void Clear(bool keepMatDesc, bool keepPass);	//保持している情報を全部クリア

		//全パスを順次実行。CommandListが開いている状態で実行する
		void Render();
		void Update();	//全パスの更新(リサイズなとで参照しているオブジェクトが上書き更新された場合に使う。レジスタ数に変化がある時は作り直さないといけない)
		void Resize(int width, int height);		//出力先のリサイズに伴うバッファの再作成を促す

		//filenameで指定されるエフェクトの読み込み
		// updateSharedRef : 読み込まれたエフェクトに共有源があり、その共有先がwatcherに含まれる時、共有先もリロードする
		// restoreMatDesc : trueの時、config.mdbにバックアップされた材質注釈を復元する。falseの時は材質注釈はテンプレートのデフォルト値で初期化される
		// updateShader : リソースが更新されたがルートシグネチャには影響がないのでシェーダの読み直しはしなくて良い場合はfalse
		void Load(DXR& dxr, const std::wstring& filename, FXShaderCache& cache, FXWatcher* watcher, const FXLoadConfig& config = {},
			bool updateSharedRef = true, bool restoreMatDesc = true, bool updateShader = true);
		
		//definedPassの更新に伴ってエフェクト内のパスをアップデートする
		//definedPassの中で参照されるリソースの数が変わってもエフェクトで使用するレジスタ番号は動かない事を前提にしている
		//材質注釈についてはこの関数が呼ばれた時点での状態をusedConfigにバックアップ・復元する
		void UpdateDefinedPass();

		//材質注釈ファイルを開いて文字列に入れる、ファイル名が空文字だった場合はデフォルトの材質注釈ファイルを読み込む
		std::string ReadDescFile(const std::string& filename);

		//全材質注釈のセット
		//desc[モデル番号][材質番号]にそれぞれの材質注釈を入れる(Load時に呼ばれる)
		//読み込みに失敗してデフォルトに置き換えられた材質がある場合、帰り値に「モデル番号・材質番号」の格納されたベクタが返る
		//この関数自体は例外を投げない
		std::vector <std::tuple<int,int>> SetMatDescAll(const std::vector<std::vector<std::string>>& descfile);

		//モデル番号][材質番号]の注釈を変更する
		//値のみ、又はテクスチャのみ更新したい場合はsetValue, setTextureをそれぞれ設定する
		//内容を連続して更新する途中の場合はpassUpdate = falseにするとパスの更新をしない(trueにした場合もテクスチャの更新が無い場合はパスの更新は必要が無いのでしない)
		void SetMatDesc(const std::string& descfile, uint32_t iModel, uint32_t iMat, bool setValue = true, bool setTexture = true, bool passUpdate = true);

		//設定済みの材質注釈(3D)テクスチャを一意なテクスチャ配列にする
		void MatDescUniqueTextures();

		//↑に続いて各パスのSRVにテクスチャを入れる
		// パス毎に個別に行いたい場合はiPassにパス番号(ファンクショナルパスを除いて何番目のパスか、0スタート)を入れる
		// デフォルトでは全パスを実行
		void SetupMatDescSRV(int iPass = -1);

		//コントローラの値を得てコントローラ用CBを更新する
		void UpdateController();

		//Matdesc内にコントローラの値をベースにした式が含まれる場合、その評価を行ってMatDescを再アップロードする
		//CommandListが開いている状態で実行する
		void ResolveMatDescController();

		//MatDesc内の共有テクスチャ・デフォーマ内定義テクスチャの更新をする
		void UpdateMatDescSharedTexture();

		//テスト用、hlslファイルにfx構造体から生成されるjsonをくっつけてfxfileへ出力
		void SaveCombined(const std::wstring& fxfile, const std::wstring& hlsl, const FXTech& fx);
		
		//共有リソースの定義元を検索する、見つからない場合はnullptrを返す
		Res* FindSharedSource(std::string name);

		//FXFileに格納されているカテゴリを知る
		static FXCategory CategoryFromFile(const std::wstring& filename) {
			std::ifstream ifs(filename, std::ios::binary);
			if (!ifs) { return FXCategory::none; }
			FXTech tech;
			ReadHeader(u8(filename), ifs, "fx", tech);
			return StringToFXCategory(tech.category);
		}

		//バッファの1要素のサイズを得る
		UINT GetElemSize(const FXBuffer& buf);

		//pathに格納されたfxファイルにrapidJSONでパースしてエラーを報告させる
		static std::string ReportJSONError(const fs::path& path);
		
		//passの出力サイズを決定する
		FXSize ResolveOutputSize(const FXPass& pass);

		//Techの読み出し用
		const FXTech& tech() const { return m_tech; };

		//dimensionとnumから実際に設定されるnumthreads数を計算する
		XMUINT3 CalcNumthreads(const XMUINT3& num, int dimension) {
			auto numthreads = (num.x == 0 && num.y == 0 && num.z == 0) ? usedConfig.defaultNumthreads[dimension - 1] : num;
			numthreads.x = max(1, numthreads.x);
			numthreads.y = max(1, numthreads.y);
			numthreads.z = max(1, numthreads.z);
			return numthreads;
		}

	};



	//FXファイル監視
	struct FXWatchedItem {
		int id;
		FXCategory category = FXCategory::none;	//なる予定のエフェクトカテゴリ
		fs::file_time_type time;//ソースの最終更新時刻
		//コンパイルの成功・失敗に応じて書き換えられるFXオブジェクト
		//読み込もうとしたがコンパイルなどが失敗した場合やレンダリングに失敗した場合はnullptrが入り、エフェクトファイルの更新を待つ
		std::unique_ptr<FX> fx = nullptr;	
		fs::path path;				//監視対象ファイル
		FXLoadConfig config;		//読み込み時のオプション
	};

	class FXWatcher {
		DXR* dxr;
		FXShaderCache* cache;
		DWORD lastPolled = 0;
		//コンパイルする。成功したらtrue、失敗したらfalse、例外は起こさない
		//updateは起動後の初コンパイル時はfalse, ソースの更新に伴う再コンパイルはtrue
		bool Compile(FXWatchedItem& w, bool update);
	public:
		std::vector<FXWatchedItem>watchList;	//監視対象アイテム
		DWORD pollInterval = 1000;	//ファイル更新チェックの間隔[ms]
		FXWatcher(DXR& _dxr, FXShaderCache& _cache) { dxr = &_dxr; cache = &_cache; };

		//エフェクトオブジェクトに割り当てるidを指定して監視を始めるファイルをパスで指定する
		//監視開始の際に読み込みを行い、fxオブジェクトを作成する(Get()で取得できる)
		//読み込みに失敗した場合はfxオブジェクトにnullptrが入るだけで例外は起こさない
		//delayedLoading 監視の開始はするがすぐにはコンパイルしない
		void StartWatch(int id, FXCategory cat, const fs::path& path, const FXLoadConfig& config, bool delayedLoading = false);
		void DelayedLoad(int id);	//delayedLoadingされたエフェクトをコンパイルする

		//監視対象から外す。無効なidが指定されても例外は起こさない
		void EndWatch(int id);

		//pollInterval[ms]間隔でファイルの更新をチェックし、更新されていたら読み込みなおす
		//どれかのエフェクトの更新があった場合、返り値はtrue
		bool Poll();

		//監視しているエフェクト群に対するdefinedPassが更新された時の処理
		void UpdateDefinedPass();

		//共有リソースの解決, 共有先エフェクトについて、パスに格納されている共有参照オブジェクトを更新する
		void ResolveSharedResources();

		//共有元が更新されるので共有リソースを一旦使用停止する
		//namesには使用停止する共有源リソース名
		void StopSharedResources(const std::unordered_set<std::string>& names);

		//監視中のアイテムへの参照を返す
		//idが無効な場合は特に例外など出さずにnullptrを返す
		FXWatchedItem* Find(int id) {
			for (auto&& w : watchList)
				if (w.id == id)
					return &w;
			return nullptr;
		}

		//コンパイルされたFXオブジェクトへの参照を返す
		//コンパイルエラーなどが原因で作られていない場合やidが無効な場合は特に例外など出さずにnullptrを返す
		FX* Get(int id) {
			auto w = Find(id);
			if (w)
				return w->fx.get();
			else
				return nullptr;
		}

	};
}

module : private;

namespace {
	//Json部分に開始記号・終了記号を付けてofsに書き入れる
	template <class T>
	void WriteHeader(std::ostream& ofs, const std::string& rootname, const T& info, const std::string& startSign = "[YRZFX]", const std::string& endSign = "[HLSL]")
	{

		ofs << startSign << "\n";
		{
			cereal::JSONOutputArchive archive(ofs);
			archive(cereal::make_nvp(rootname, info));
		}
		ofs << "\n" << endSign << "\n";

	}

}

namespace YRZ {


	void FXFunctionalPass::Update()
	{
		if (type == FXPassType::clearUAV || type == FXPassType::clearRTV) {
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.NumDescriptors = 1;
			heapDesc.Type = type == FXPassType::clearUAV ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			heapDesc.Flags = type == FXPassType::clearUAV ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

			// ディスクリプタヒープの作成
			ThrowIfFailed(m_dxr->Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(DH.ReleaseAndGetAddressOf())), L"CreateDescriptorHeap @ FXFunctionalPass::Update() failed", __FILEW__, __LINE__);
			m_cpuHandle = DH->GetCPUDescriptorHandleForHeapStart();

			auto desc = clearTarget->desc();
			if (type == FXPassType::clearUAV) {
				// UAV
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = clearTarget->UAVDesc();
				m_dxr->Device()->CreateUnorderedAccessView(clearTarget->res.Get(), nullptr, &uavDesc, m_cpuHandle);
				// GPUディスクリプタハンドル(UAVの場合のみ使う)
				m_gpuHandle = DH->GetGPUDescriptorHandleForHeapStart();
			} else {
				//RTV
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = clearTarget->RTVDesc();
				m_dxr->Device()->CreateRenderTargetView(clearTarget->res.Get(), &rtvDesc, m_cpuHandle);
			}
		} else if (type == FXPassType::mipmapgen) {

			mipPass.clear();
			int nmip = mipTarget->desc().MipLevels;
			//Shader cs = m_dxr->CompileShaderMemory(MipmapCS, sizeof(MipmapCS), nullptr, L"mipmapCS", L"CS", L"cs_6_1");
			Shader vs = m_dxr->CompileShaderMemory(MipmapPS, sizeof(MipmapPS), nullptr, L"mipmapPS", L"VS", L"vs_6_1");
			Shader ps = m_dxr->CompileShaderMemory(MipmapPS, sizeof(MipmapPS), nullptr, L"mipmapPS", L"PS", L"ps_6_1");

			//1レベル下のmipに縮小するシェーダをセット
			for (int i = 0; i < nmip - 1; i++) {
				Pass p(m_dxr);
				SRVItem si = {};
				si.res = mipTarget;
				si.mipLevels = 1;
				si.mostDetailedMip = i;
				p.SRV[0].push_back(si);
				p.RTV.push_back({ mipTarget,{}, i + 1 });
				p.Samplers.push_back(CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT));
				p.PostProcessPass(vs, ps);
				mipPass.emplace_back(p);
			}
		}
	}

#if 0
	//1個の演算子または数値リテラルまたはコントローラ名と思われる文字列をパースして各々に分類する
	bool FXOpOp::Parse(const std::string& str, const FX& fx, float defval, const std::vector<FXMatDescController>& ctrls, int selfIdx)
	{
		selfIndex = selfIdx;
		std::string s = str;

		if (s == "+" || s == "-" || s == "/" || s == "*") {
			//2項演算子である
			type = opr;
			op = s[0];
		} else if (s[0] == '@'){
			//関数である
			type = fun;
			auto f = s.substr(1);
			if (SupportedFunctions.contains(f)) {
				auto [idx, num] = SupportedFunctions[f];
				func = idx;
				funcpop = num;
			} else {
				func = 0;
			}
		} else {
			auto cit = std::find_if(ctrls.begin(), ctrls.end(), [&](auto& o) {return o.name == s; });
			auto it = std::find_if(fx.controllers.begin(), fx.controllers.end(), [&](auto& o) {return o.name == s; });
			if (cit != ctrls.end()) {
				//材質注釈内で定義されたコントローラ由来の変数である
				type = locon;
				if (fx.usedConfig.ControllerQuery)
					fx.usedConfig.ControllerQuery(selfIdx, cit->doppelIndex, cit->controllerName, cit->item, "float", iController, iItem);
			} else if (it != fx.controllers.end()) {
				//コントローラ由来の変数である
				type = contofs;
				offset = it->offset;
			} else if (fx.usedConfig.CBPointers.contains(s)) {
				//定数バッファ由来の変数である
				type = cbofs;
				auto it = fx.usedConfig.CBPointers.find(s);
				offset = it->second.first;
				isint = it->second.second;
			} else {
				//数値に変換できるなら数値リテラル
				type = val;
				try {
					value = std::stof(s);
				} catch (...) {
					value = defval;	//特定の数値でもコントローラの値でもない場合はデフォルト値をセットしとくけど変換ミスを伝える
					return false;
				}
			}
		}
		return true;
	}

	float FXOpOp::Func(int idx, float v1, float v2, float v3)
	{
		auto sat = [](float f) { return max(0, min(1, f)); };
		auto frac = [](float f) { return f - floorf(f); };
		auto lerp = [](float a, float b, float c) { return a * (1 - c) + b * c; };
		auto hsvR = [&](float h, float s, float v) { return (((sat(abs(frac(h) * 2 - 1) * 3 - 1) - 1) * sat(s) + 1) * v); };
		auto hsvG = [&](float h, float s, float v) { return hsvR(h + 2 / 3.0f, s, v); };
		auto hsvB = [&](float h, float s, float v) { return hsvR(h + 1 / 3.0f, s, v); };
		auto uvec3 = [&](const XMUINT3& v, auto f) { return XMUINT3(f(v.x), f(v.y), f(v.z)); };
		//pcg3d
		auto hash = [&](float x) {
			auto v = XMUINT3(x, 114, 514);
			v = uvec3(v, [&](auto x) {return x * 1664525u + 1013904223u; });
			v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
			v = uvec3(v, [&](auto x) { return x ^ (x >> 16u); });
			v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
			return (float)((double)v.x / 4294967296);
			};
		auto noise = [&](float x) {
			float f = frac(x);
			float i = x - f;
			float u = f * f * f * (f * (f * 6 - 15) + 10);
			float n = lerp(hash(i), hash(i + 1), u);
			return n;
			};

		switch (idx) {
		case 1: return sinf(v1); break;
		case 2: return cosf(v1); break;
		case 3: return tanf(v1); break;
		case 4: return asinf(v1); break;
		case 5: return acosf(v1); break;
		case 6: return atanf(v1); break;
		case 7: return expf(v1); break;
		case 8: return logf(v1); break;
		case 9: return sqrtf(v1); break;
		case 10: return sat(v1); break;
		case 11: return frac(v1); break;
		case 12: return floorf(v1); break;
		case 13: return ceilf(v1); break;
		case 14: return lerp(v1, v2, v3); break;
		case 15: return ((int)v1 >> (int)v2) & 1; break;
		case 16: return hsvR(v1, v2, v3); break;
		case 17: return hsvG(v1, v2, v3); break;
		case 18: return hsvB(v1, v2, v3); break;
		case 19: return v1 ? v2 : v3;
		case 20: return min(max(v1, v2), v3); break;
		case 21: return min(v1, v2); break;
		case 22: return max(v1, v2); break;
		case 23: return (v1 <= v2) ? 0 : 1; break;
		case 24: {
			if (v1 == v2) { return v1 <= v3 ? 0 : 1; }
			float u = (v3 - v1) / (v2 - v1);
			float x = max(0.0f, min(u, 1.0f));
			return x * x * (3.0f - 2.0f * x);
		} break;
		case 25: return hash(v1); break;
		case 26: return noise(v1); break;
		}
		return v1;
	}

	//数値に使える文字かどうかを判定(1e+3 みたいなのはダメ)
	bool isNumber(char c)
	{
		return ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-');
	}

	//シンボルに使える文字かどうかを判定
	bool isSymbol(char c)
	{
		return std::isalnum(unsigned char(c)) || c == '_';
	}

	//文字列をトークン列に変換
	std::vector<std::string> tokenize(const std::string& expression) {
		std::vector<std::string> tokens;
		std::regex token_pattern(R"((\-\d*\.\d+|\d*\.\d+|\-\d+|\d+|[+\-*/(),]|[a-zA-Z_]\w*))");
		auto tokens_begin = std::sregex_iterator(expression.begin(), expression.end(), token_pattern);
		auto tokens_end = std::sregex_iterator();

		for (std::sregex_iterator i = tokens_begin; i != tokens_end; ++i) {
			auto s = i->str();
			if (SupportedConstants.contains(s))
				s = SupportedConstants[s];
			tokens.push_back(s);
		}

		return tokens;
	}


	// tokenizeで切り出してトークン列にした物を逆ポーランド記法に従って並べ替える
	// 関数名と思われる物は@を先頭につけ、括弧は優先順位の解決の上、除去される
	std::vector<std::string> toRPN(const std::vector<std::string>& tokens) {
		std::vector<std::string> output;
		std::stack<std::string> operators;
		std::map<std::string, int> precedence = { {"+", 1}, {"-", 1}, {"*", 2}, {"/", 2} };
		std::map<std::string, int> associativity = { {"+", 0}, {"-", 0}, {"*", 0}, {"/", 0} }; // 0 for left, 1 for right

		for (size_t i = 0; i < tokens.size(); ++i) {
			const std::string& token = tokens[i];

			// 数値または変数の場合
			if (std::regex_match(token, std::regex(R"(\-?\d*\.\d+|\-?\d+|[a-zA-Z_]\w*)")) && SupportedFunctions.find(token) == SupportedFunctions.end()) {
				output.push_back(token);
			}
			// 左括弧の場合
			else if (token == "(") {
				operators.push(token);
			}
			// 右括弧の場合
			else if (token == ")") {
				while (!operators.empty() && operators.top() != "(") {
					output.push_back(operators.top());
					operators.pop();
				}
				if (!operators.empty() && operators.top() == "(") {
					operators.pop();
				} else {
					throw std::runtime_error("Mismatched parentheses");
				}
				// 関数を処理する
				if (!operators.empty() && SupportedFunctions.find(operators.top().substr(1)) != SupportedFunctions.end()) {
					output.push_back(operators.top());
					operators.pop();
				}
			}
			// 関数の場合
			else if (SupportedFunctions.find(token) != SupportedFunctions.end()) {
				operators.push("@" + token); // 関数トークンを追加
			}
			// カンマの場合（関数の引数区切り）
			else if (token == ",") {
				while (!operators.empty() && operators.top() != "(") {
					output.push_back(operators.top());
					operators.pop();
				}
			}
			// 演算子の場合
			else if (precedence.find(token) != precedence.end()) {
				while (!operators.empty() && operators.top() != "(" &&
					((associativity[token] == 0 && precedence[token] <= precedence[operators.top()]) ||
						(associativity[token] == 1 && precedence[token] < precedence[operators.top()]))) {
					output.push_back(operators.top());
					operators.pop();
				}
				operators.push(token);
			}
		}

		while (!operators.empty()) {
			if (operators.top() == "(" || operators.top() == ")") {
				throw std::runtime_error("Mismatched parentheses");
			}
			output.push_back(operators.top());
			operators.pop();
		}

		return output;
	}

	//後置記法トークン列が定数式か？
	bool isConstantExpression(const std::vector<std::string>& postfix) {
		for (const auto& token : postfix) {
			if (!(isNumber(token[0]) || token[0]=='@') && token != " + " && token != " - " && token != " * " && token != " / ") {
				return false;
			}
		}
		return true;
	}

	//定数式の後置記法トークン列を評価する
	float evaluateConstantRPN(const std::vector<std::string>& postfix, bool& success) {
		std::stack<float> stack;
		success = false;

		for (const auto& token : postfix) {
			if (isNumber(token[0])) {
				try {
					stack.push(std::stod(token));
				} catch (...) {
					return 0;
				}
			} else if (token == "+" || token == "-" || token == "*" || token == "/") {
				if (stack.size() < 2) {
					return 0;
				}
				float b = stack.top();
				stack.pop();
				float a = stack.top();
				stack.pop();
				if (token == "+") {
					stack.push(a + b);
				} else if (token == "-") {
					stack.push(a - b);
				} else if (token == "*") {
					stack.push(a * b);
				} else if (token == "/") {
					stack.push(a / b);
				}
			} else if (token[0] == '@') {
				auto f = token.substr(1);
				if (SupportedFunctions.contains(f)) {
					auto [idx, num] = SupportedFunctions[f];
					float a[3] = { 0,0,0 };
					for (int i = num-1; i >= 0; i--) {
						a[i] = stack.top();
						stack.pop();
					}
					stack.push(FXOpOp::Func(idx, a[0],a[1],a[2]));
				} else {
					return 0;
				}
			} else {
				return 0;
			}
		}

		if (stack.size() != 1) {
			return 0;
		}

		success = true;
		return stack.top();
	}


	//str:入力、エラーがなければtrue
	bool FXExpr::Parse(const std::string& str, const FX& fx, int _ofs, const std::vector<FXMatDescController>& ctrls, int selfIndex, const std::unordered_map<std::string,float>* edict)
	{
		*this = {};
		ofs = _ofs;
		std::vector<std::string>postfix;

		//逆ポーランド記法に変換。入力にエラーがあるなら帰る
		try {
			auto tokens = tokenize(str);
			postfix = toRPN(tokens);
		} catch (...) {
			return false;
		}

		//列挙子を置換
		if (edict) {
			for (auto&& t : postfix) {
				if (edict->contains(t))
					t = std::to_string(edict->at(t));
			}
		}

		//定数式だったら定数に変換
		if (isConstantExpression(postfix)) {
			bool ok;
			value = (float)evaluateConstantRPN(postfix, ok);
			empty = !ok;
			return ok;
		}

		//オペランドと演算子を逆ポーランド記法順にexprに入れる
		for (auto&& e : postfix) {
			FXOpOp op;
			if (!op.Parse(e, fx, 0, ctrls, selfIndex))
				return false;
			expr.push_back(op);
		}
		empty = false;
		isConst = false;
		return true;
	}

	//CBの値から式を評価して値をretに返す。成功したらtrue
	bool FXExpr::Eval(const FX& fx, float& ret)
	{
		ret = value;

		if (empty)
			return false;

		if (isConst)
			return true;

		auto* pCB = static_cast<char*>(fx.controllerCB.pData);
		std::stack<float> stack;

		for (const auto& token : expr) {
			if (token.type == FXOpOp::val) {
				stack.push(token.value);
			} else if (token.type == FXOpOp::opr) {
				if (stack.size() < 2) {
					return false;
				}
				float b = stack.top();
				stack.pop();
				float a = stack.top();
				stack.pop();
				if (token.op == '+') {
					stack.push(a + b);
				} else if (token.op == '-') {
					stack.push(a - b);
				} else if (token.op == '*') {
					stack.push(a * b);
				} else if (token.op == '/') {
					stack.push(a / b);
				}
			} else if (token.type == FXOpOp::fun) {
				float a[3] = { 0,0,0 };	//今の所3変数関数までサポート
				for (int i = token.funcpop - 1; i >= 0; i--) {
					if (stack.size() < 1)
						return false;
					a[i] = stack.top();
					stack.pop();
				}
				stack.push(FXOpOp::Func(token.func, a[0],a[1],a[2]));
			} else if (token.type == FXOpOp::locon) {
				if (fx.usedConfig.ControllerCallback) {
					float c;
					fx.usedConfig.ControllerCallback(&c, token.iController, token.iItem, "float");
					stack.push(c);
				}
			} else if (token.type == FXOpOp::contofs) {
				float c = *reinterpret_cast<float*>(pCB + token.offset);
				stack.push(c);
			} else {
				float c;
				if (token.isint) {
					int i = *reinterpret_cast<int*>(fx.usedConfig.pCB + token.offset);
					c = (float)i;
				} else {
					c = *reinterpret_cast<float*>(fx.usedConfig.pCB + token.offset);
				}
				stack.push(c);
			}
		}

		if (stack.size() != 1) {
			return false;
		}

		ret = stack.top();
		return true;
	}
#endif


	//mdのバックアップをとる
	void FXMatDescBackup::Backup(const FXMatDescStorage& md) {
		if (!md.name.empty()) {
			sourceFiles.clear();
			auto m = md.items.size();
			sourceFiles.resize(m);
			for (size_t i = 0; i < m; i++) {
				auto n = md.items[i].size();
				sourceFiles[i].resize(n);
				for (size_t j = 0; j < n; j++) {
					sourceFiles[i][j] = md.items[i][j].sourceFile;
				}
			}
		}
	}

	//sをtokで区切ってベクタに入れる
	//ただし括弧の中、ダブルクォートの間にあるtokは無視する
	std::vector<std::string> FXMatDescParser::split(const std::string& str, char tok) {
		std::vector<std::string> result;
		std::string temp;
		bool in_dq = false;
		int in_parentheses = 0;

		for (char ch : str) {
			if (ch == '"') {
				in_dq = !in_dq;
			}
			if (ch == '(') {
				in_parentheses++;
			}
			if (ch == ')') {
				in_parentheses--;
			}

			if (ch == tok && !in_dq && !in_parentheses) {
				result.push_back(temp);
				temp.clear();
			} else {
				temp += ch;
			}
		}
		if (!temp.empty()) {
			result.push_back(temp);
		}

		return result;
	}

	std::string FXMatDescParser::deleteWhite(const std::string& str) {
		std::string ret;
		bool inDQ = false;	//"～"の間か？

		for (auto c : str) {
			if (c == '"') {
				inDQ = !inDQ;
			}
			if (c > 0) {	//cが0x80以降でisspaceに投げるとエラーがでるので
				if (std::isspace(c) && !inDQ)
					continue;
			}
			ret.push_back(c);
		}
		return ret;
	}


	//文字列を整形して : 区切りのベクタにする
	std::vector<std::vector<std::string>> FXMatDescParser::getLR(const std::string& str) {
		if (str.empty())
			return {};

		//;と\nで区切ってlinesに入れる
		std::vector<std::string> lines;
		
		std::string buf;
		for (auto c : str) {
			if (c == ';' || c == '\n') {
				if (!buf.empty())
					lines.push_back(buf);
				buf = "";
			} else {
				buf += c;
			}
		}
		if (!buf.empty())
			lines.push_back(buf);

		//各行の#以降を除去
		for (auto&& l : lines) {
			auto idx = l.find('#');
			if (idx != std::string::npos)
				l = l.substr(0,idx);
		}


		//白文字の除去
		for (auto&& s : lines)
			s = deleteWhite(s);

		//:で左右を区切る。右辺 : 左辺 で区切れる物だけ処理対象にする
		std::vector<std::vector<std::string>> LR;
		for (auto&& s : lines) {
			if (!s.empty()) {
				//ドライブ名がテクスチャファイル名に入っている可能性があるので全部のコロンで分解ではなくコロン1か所だけで分解する
				auto idx = s.find(":");
				if (idx == std::string::npos) {
					//コロンが無い
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc --\n {} \n no colon in the line (or forgot semicolon)", L(s)), __FILEW__, __LINE__);
					return {};
				}
				std::string L = s.substr(0, idx);
				std::string R = s.substr(idx + 1);
				LR.push_back({ L,R });
			}
		}
		return LR;
	}

	//LRの値の指定のある行をパースして、右辺のfloatX型の値を得る
	//isTemplate : 材質注釈テンプレートの場合はtrue, 普通の材質注釈ファイルの場合はfalse
	void FXMatDescParser::parseval(
		const std::vector<std::string>& l, std::vector<float>& values, std::vector<FXExpr>* exprs, std::vector<FXMatDescController>*ctrls, bool isTemplate, int selfIndex)
	{
		std::wstring tempstr = isTemplate ? L"template" : L"";

		//コントローラやマクロ定義についての行ならこの関数では扱わない
		if (l[0].starts_with("_C") || l[0].starts_with("_M"))
			return;

		//まだ定義されていない変数が参照された
		if (!varmap.contains(l[0])) {
			if (!l[0].starts_with("_T") && !l[0].starts_with("_V"))
				YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc {} --\n {} : {} \n variable {} not found", tempstr,
					L(l[0]), L(l[1]), L(l[0])), __FILEW__, __LINE__);
			return;
		}

		auto vm = varmap[l[0]];

		//,で区切る
		auto csv = split(l[1], ',');
		if (csv.size() != vm.nComp) {
			//要素数が違う
			YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc {} --\n {} : {} \n component numbers not match", tempstr,
				L(l[0]), L(l[1])), __FILEW__, __LINE__);
			return;
		}

		for (int i = 0; auto & c : csv) {
			bool ok;
			FXExpr e;

			//列挙子を定数辞書に入れる
			if (enumdict.contains(l[0])) {
				auto savedConst = m_fx->mdCompiler.constants;
				for (auto const& [key, val] : enumdict[l[0]]) {
					m_fx->mdCompiler.constants.insert_or_assign(key, val);
				}
				ok = e.e.Compile(m_fx->mdCompiler, c);
				m_fx->mdCompiler.constants = savedConst;
			} else {
				ok = e.e.Compile(m_fx->mdCompiler, c);
			}

			if (!ok) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc {} --\n {} : {} \n {}", tempstr,
					L(l[0]), L(l[1]), L(m_fx->mdCompiler.error)), __FILEW__, __LINE__);
				return;
			}

			//初期値のセット
			float constVal = e.e.IsConst() ? e.e.ConstValue() : 0.0f;
			if (vm.type == 0) {
				//値配列にセット
				values[vm.offset+i] = constVal;
			} else if (vm.type == 1) {
				//int型でセット
				int j = (int)constVal;
				int* p = reinterpret_cast<int*>(&values[vm.offset+i]);
				*p = j;
			}

			//式だったら式リストに追加
			if (!e.e.IsConst()) {
				if (isTemplate) {
					ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc {} --\n {} : {} \n expressions cannot be used in templates", tempstr,
						L(l[0]), L(l[1])), __FILEW__, __LINE__);
					return;
				} else if (exprs) {
					e.ofs = vm.offset + i;
					exprs->push_back(e);
				}
			}

			i++;
		}

	}

	//ダブルクォートを取り除く
	std::string FXMatDescParser::delDoubleQuote(const std::string& str) {
		int start = 0, length = str.size();
		
		if (length == 0)
			return "";

		if (str[0] == '"')
			start = 1;

		auto idx = str.rfind('"');
		if (idx != 0 && idx != std::string::npos)
			length = idx - start;
		return str.substr(start, length);
	}

	//文字列の中の最初の整数文字列をintに変換
	int toru_stoi(const std::string& str) {
		std::string num_str;
		for (char c : str) {
			if (std::isdigit(c)) {
				num_str += c;
			} else if (!num_str.empty()) {
				break;
			}
		}
		return std::stoi(num_str);
	}

	void FXMatDescParser::ParseTemplate(const std::string& str, const fs::path& fxpath, FX* fx) {
		//一旦全部クリア
		*this = {};
		m_fx = fx;

		auto LR = getLR(str);
		defaultVal = {};
		defaultTex = {};

		//template評価
		for (auto&& l : LR) {

			auto textureLambda = [&](auto& texmap, auto& mip, auto& defaultTex, auto& nTex) {//_Tから始まっているならテクスチャについての行
				if (l[0][2] >= '0' && l[0][2] <= '9') {
					//テンプレートに登録
					int ireg = toru_stoi(l[0]);   //レジスタ番号を得る
					//命名被りチェック
					if (texmap.contains(l[1])) {
						YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n symbol {} already used",
							L(l[0]), L(l[1]), L(l[1])), __FILEW__, __LINE__);
						return;
					}
					//レジスタ番号チェック
					if (ireg < 0 || ireg >= 64) {
						YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n invalid texture register {}, (must be 0-64)",
							L(l[0]), L(l[1]), ireg), __FILEW__, __LINE__);
						return;
					} else if (ireg >= nTex) {
						nTex = ireg + 1;
						defaultTex.resize(nTex);
					}
					texmap[l[1]] = ireg;

					//末尾にmがあるならミップマップ対応
					if (l[0].back() == 'm')
						mip.insert(ireg);
				} else {
					//デフォルト値
					auto ss = l[0].substr(2);
					if (!texmap.contains(ss)) {
						YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n texture {} is not found in template",
							L(l[0]), L(l[1]), L(ss)), __FILEW__, __LINE__);
						return;
					}
					//デフォルトテクスチャの指定が空文字ならそのまま、そうでないならエフェクトファイルからのパスをつなげて絶対パスにする
					//個々のmatdescはmatdescからの相対パスになるが、デフォルト設定のパスはエフェクトファイルからの相対パスになるので
					auto deftex = delDoubleQuote(l[1]);
					if (deftex.empty())
						defaultTex[texmap[ss]] = "";
					else {
						fs::path dp = fxpath / (char8_t*)deftex.c_str();
						defaultTex[texmap[ss]] = (char*)dp.u8string().c_str();
					}
				}
				};

			if (l[0].starts_with("_T")) {
				textureLambda(texmap, mip, defaultTex, nTex);
			} else if (l[0].starts_with("_V")) {
				textureLambda(texmap3D, mip3D, defaultTex3D, nTex3D);
			} else if (l[0].starts_with("_R")) {
				//_Rからはじまってるならおすすめ設定キーワード
				auto fname = delDoubleQuote(l[0].substr(2));
				auto recs = split(l[1], ',');
				for (auto&& r : recs) {
					recommends[r] = fname;
				}
			} else if (l[0].starts_with("_E")) {
				//_Eから始まっているなら列挙子の定義
				//例文
				// _E Catergory : default, glass, metal → defaultに0、glassに1、metalに2が割り当てられる
				// _E Catergory : default=0, glass=1, metal=4 → defaultに0、glassに1、metalに4が割り当てられる
				// 列挙子が置換される定数はfloat型で管理されるので、int型に入れる場合は7桁未満の整数を指定すべし
				auto ss = l[0].substr(2);
				auto enums = split(l[1], ',');
				std::unordered_map<std::string, float>item;
				for (auto&& e : enums) {
					auto f = split(e, '=');
					if (f.size() > 2 || f.empty()) {
						YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n invalid enumerator definition",
							L(l[0]), L(l[1])), __FILEW__, __LINE__);
						return;
					}
					if (f.size() == 1) {
						item[f[0]] = item.size();
					}
					if (f.size() == 2) {
						item[f[0]] = std::stof(f[1]);
					}
				}
				enumdict[ss] = item;
			} else if (l[0].find(".") != std::string::npos) {
				//左辺に.が含まれるなら_value構造体のメンバ定義
				FXMatDescVar mv = {};
				auto v = split(l[0], '.');
				if (v.size() != 2) {
					YRZ::ThrowIfFailed(E_INVALIDARG,
						std::format(L"invalid matDesc template --\n {}:{} \n invalid variable definition", L(l[0]), L(l[1])), __FILEW__, __LINE__);
					return;
				}
				//型チェック(今はfとi以外ダメ)
				auto type = v[0];
				if (v[0] == "i") {
					mv.type = 1;
				} else if (v[0] == "f") {
					mv.type = 0;
				} else {
					YRZ::ThrowIfFailed(E_INVALIDARG,
						std::format(L"invalid matDesc template --\n {}:{} \n invalid variable type (must be f. or i.)", L(l[0]), L(l[1])), __FILEW__, __LINE__);
					return;
				}
				mv.order = varmap.size();
				mv.offset = nVal;
				mv.nComp = std::stoi(v[1]);
				nVal += mv.nComp;
				defaultVal.resize(nVal);
				for (int i = 0; i < mv.nComp; i++) {
					valTypes.push_back(mv.type);
				}
				//要素数チェック
				if (mv.nComp < 0 || mv.nComp > 4) {
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n invalid number of components (must be 1-4)", L(l[0]), L(l[1])), __FILEW__, __LINE__);
					return;
				}
				//命名被りチェック
				if (varmap.contains(l[1])) {
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n {} : {} \n symbol {} already used",
						L(l[0]), L(l[1]), L(l[1])), __FILEW__, __LINE__);
					return;
				}
				varmap[l[1]] = mv;
			} else {
				//左辺には.が含まれないならデフォルトパラメータ
				parseval(l, defaultVal, nullptr, nullptr, true);
			}
		}

		//テクスチャと変数で被ってる名前がないかチェック
		for (auto&& [name, itex] : texmap) {
			if (varmap.contains(name)) {
				YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n symbol {} is included in both textures and variables", L(name)), __FILEW__, __LINE__);
				return;
			}
		}
		for (auto&& [name, itex] : texmap3D) {
			if (varmap.contains(name)) {
				YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n symbol {} is included in both textures3D and variables", L(name)), __FILEW__, __LINE__);
				return;
			}
		}

		//変数が一個もない
		if (varmap.empty()) {
			YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc template --\n no variables defined"), __FILEW__, __LINE__);
			return;
		}

	}

	//マクロ
	struct FXMatDescMacro {
		std::string symbol;	//マクロの名前(展開前のシンボル)
		std::string arg;	//マクロの引数(metaの中にargが含まれていれば置換される)
		std::string meta;	//argの含まれたメタ文字列
	};

	// マクロの引数内でネストされた関数呼び出しを正しく処理するためのヘルパー関数
	std::string extractArgs(const std::string& str, size_t& pos) {
		std::string args;
		int level = 0;
		while (pos < str.size()) {
			char c = str[pos];
			if (c == '(') {
				level++;
			} else if (c == ')') {
				level--;
				if (level == 0) {
					args += c;
					pos++;
					break;
				}
			}
			args += c;
			pos++;
		}
		return args;
	}

	//引数付きマクロ展開関数
	std::string ExpandMacro(const FXMatDescMacro& macro, const std::string& input) {
		std::regex macro_regex(R"(\b(\w+)\((.*)\))");
		std::smatch match;
		std::string result = input;

		if (std::regex_search(result, match, macro_regex)) {
			std::string macro_name = match[1].str();
			std::string macro_args_str = match[2].str();
			size_t pos = 0;
			std::string expanded_args;

			// 引数の処理
			while (pos < macro_args_str.size()) {
				if (macro_args_str[pos] == '(') {
					expanded_args += extractArgs(macro_args_str, pos);
				} else {
					expanded_args += macro_args_str[pos];
					pos++;
				}
			}

			if (macro_name == macro.symbol) {
				std::string macro_body = macro.meta;
				std::string expanded_macro = std::regex_replace(macro_body, std::regex("\\b" + macro.arg + "\\b"), expanded_args);
				result.replace(match.position(0), match.length(0), expanded_macro);
			} else {
				result.replace(match.position(0), match.length(0), macro_name + "(" + expanded_args + ")");
			}
		}

		return result;
	}

	//MatDescコントローラのセットアップ
	void FXMatDescController::Setup(FX& fx, int selfIndex)
	{
		fx.usedConfig.ControllerQuery(selfIndex, doppelIndex, controllerName, item, "float", modelIndex, itemIndex);
		value = std::make_shared<float>(0);
		/*
		std::string msg = std::format("sidx:{}, didx:{} ctrlname:{}, item:{} -> midx:{} iidx:{}", selfIndex, doppelIndex, controllerName, item, modelIndex, itemIndex);
		DEB(L"setup controller : {}", L(msg));
		*/
	}

	void FXMatDescController::Update(FX& fx)
	{
		fx.usedConfig.ControllerCallback(value.get(), modelIndex, itemIndex, "float");
		/*
		std::string msg = std::format("{} : midx:{} iidx:{} value:{}", item, modelIndex, itemIndex, *value);
		DEB(L"update controller : {}", L(msg));
		*/
	}

	//strに書かれたマテリアルをパースしてvalues配列とtex配列とexpr配列とcontroller配列に変換する
	FXMatDesc FXMatDescParser::Parse(const std::string& str, int selfIndex)
	{
		auto LR = getLR(str);

		//材質読み込み前のコンパイラの浮動小数辞書を保存(コントローラがあるとその材質固有の変数が増えるので)
		auto savedFval = m_fx->mdCompiler.fvars;

		std::vector<float> val = defaultVal;
		std::vector<std::string> tex = defaultTex;
		std::vector<std::string> tex3D = defaultTex3D;
		std::vector<FXExpr> expr;
		std::vector<FXMatDescController> controllers;
		std::vector<FXMatDescMacro> macros;
		for (auto&& l : LR) {
			//定義済みのマクロで置換。これにより、マクロから定義済みの(自分より前に定義された)マクロを参照できる
			for (auto& m : macros) {
				if (m.arg.empty()) {
					//引数無しマクロの場合
					std::regex macro_regex("\\b" + m.symbol + "\\b");
					std::string replacement = m.meta;
					l[1] = std::regex_replace(l[1], macro_regex, replacement);
				} else {
					//引数付きマクロの場合
					l[1] = ExpandMacro(m, l[1]);
				}
			}

			parseval(l, val, &expr, &controllers, false, selfIndex);

			if (l[0].starts_with("_T")) {
				//テクスチャ
				auto ss = l[0].substr(2);
				if (!texmap.contains(ss)) {
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc : {} = {} \n texture {} is not found in the template", L(l[0]), L(l[1]), L(ss)), __FILEW__, __LINE__);
					return {};
				}
				tex[texmap[ss]] = delDoubleQuote(l[1]);
			} else if (l[0].starts_with("_V")) {
				//3Dテクスチャ
				auto ss = l[0].substr(2);
				if (!texmap3D.contains(ss)) {
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc : {} = {} \n texture3D {} is not found in the template", L(l[0]), L(l[1]), L(ss)), __FILEW__, __LINE__);
					return {};
				}
				tex3D[texmap3D[ss]] = delDoubleQuote(l[1]);
			} else if (l[0].starts_with("_C")) {
				//コントローラ
				auto ss = l[0].substr(2);
				auto lr = split(l[1], ',');
				if (lr.size() < 2) {
					YRZ::ThrowIfFailed(E_INVALIDARG, std::format(L"invalid matDesc : {} = {} \n invalid controller variable definition", L(l[0]), L(l[1])), __FILEW__, __LINE__);
					return {};
				}
				FXMatDescController ctrl = {};
				ctrl.name = ss;
				ctrl.controllerName = delDoubleQuote(lr[0]);
				ctrl.item = delDoubleQuote(lr[1]);
				if (lr.size() > 2)
					ctrl.doppelIndex = std::stoi(lr[2]);
				ctrl.Setup(*m_fx, selfIndex);
				controllers.push_back(ctrl);
				//TODO : コンパイラに追加…しているが、関数を抜けるとこのcontrollersは消えてしまう。controllersを永続させたい
				m_fx->mdCompiler.fvars.insert_or_assign(ctrl.name, ctrl.value.get());
			} else if (l[0].starts_with("_M")) {
				//マクロ定義
				auto ss = l[0].substr(2);
				//マクロ引数がある
				std::string arg;
				int a1 = ss.find('(');
				int a2 = ss.rfind(')');
				if (a1 != std::string::npos && a2 != std::string::npos && a2 > a1 + 1) {
					arg = ss.substr(a1 + 1, a2-a1-1);
					ss = ss.substr(0, a1);
				}
				macros.push_back({ss, arg, l[1]});
			}
		}

		//コンパイラの状態を復元
		m_fx->mdCompiler.fvars = savedFval;

		return { "", val, tex, tex3D, expr, controllers };
	}

	//既に読み込まれているテンプレートに従って、values, texturesの組みから材質注釈テキストを作る
	std::string FXMatDescParser::Generate(const std::vector<float>& values, const std::vector<std::string>& textures) {
		std::stringstream ss;
		for (auto&& [name, var] : varmap) {
			ss << name << " : ";
			float v[4] = {};
			memcpy_s(v, sizeof(v), &values[var.offset], sizeof(float) * var.nComp);
			for (int i = 0; i < var.nComp; i++) {
				if (var.type == 1) {
					int32_t a;
					memcpy_s(&a, sizeof(a), &v[i], sizeof(float));
					ss << a;
				} else {
					ss << v[i];
				}
				if (i < var.nComp-1)
					ss << ",";
				else
					ss << "\n";
			}
		}
		for (auto&& [name, itex] : texmap) {
			ss << "_T" << name << " : " << textures[itex] << "\n";
		}
		return ss.str();
	}

	void FXShaderCache::RecallItems()
	{
		for (auto& entry : fs::directory_iterator(m_path)) {
			if (entry.is_regular_file()) {
				auto str = entry.path().stem().wstring();
				size_t hash = wcstoull(str.c_str(), nullptr, 16);
				//とりあえず「そういうコンパイル済みシェーダがある」という事だけ思い出して、ディスクからはまだ読み込まない
				m_cache[hash] = { false, {}, entry.last_write_time() };
			}
		}
	}

	Shader FXShaderCache::Load(DXR& dxr, void* pShaderSource, size_t sizeInBytes, const std::wstring& includePath,
		const std::wstring& sourcefile, const std::wstring& entrypoint, const std::wstring& target, const std::vector<std::wstring>& options)
	{

		auto key = sourcefile + L":" + entrypoint + L":" + target;
		for (auto& o : options) {
			key += L":";
			key += o;
		}
		auto hash = std::hash<std::wstring>()(key);
		std::wstring hashstr = std::format(L"{:x}", hash) + L"." + entrypoint;	//キャッシュファイル名。拡張子にエントリポイント名を付けるのは視認性のため

		bool compile = true;	//キャッシュにないのでコンパイルせよフラグ

		//キャッシュにある場合
		if (m_cache.contains(hash)) {
			auto& item = m_cache[hash];
			auto ftime = fs::last_write_time(sourcefile);
			if (ftime <= item.timestamp)
				compile = false;
		}

		if (compile) {
			//コンパイルしてキャッシュに入れる
			IncludeHandler IH(dxr);
			IH.InitialPath = includePath;
			auto& item = m_cache[hash];
			item.compiled = dxr.CompileShaderMemory(pShaderSource, sizeInBytes, &IH, sourcefile.c_str(), entrypoint.c_str(), target.c_str(), options);
			item.timestamp = fs::last_write_time(sourcefile);
			item.loaded = true;
			//ディスクキャッシュ有効の場合、ファイルへ保存
			if (!m_path.empty()) {
				item.compiled.SaveToFile((m_path / hashstr).wstring().c_str());
				//ソースも保存
				fs::path srcpath = m_path / (fs::path(sourcefile).filename().wstring() + L".source.hlsl");
				std::ofstream ofs(srcpath, std::ios::binary);
				ofs.write((char*)pShaderSource, sizeInBytes);
			}
			return item.compiled;
		} else {
			//キャッシュから読み出す
			if (m_cache[hash].loaded)
				return m_cache[hash].compiled;
			else {
				//キャッシュにはある事が分かっているがディスクから読んでない場合はディスクから読む
				std::wstring fname = std::format(L"{:x}", hash);
				m_cache[hash].compiled = Shader((m_path / hashstr).wstring().c_str(), entrypoint.c_str());
				m_cache[hash].loaded = true;
				return m_cache[hash].compiled;
			}
		}
	}



	std::vector<Expr::Expr> ParsePassCond(FX& fx, const std::vector<std::string>& cond)
	{
		std::vector<Expr::Expr> v;
		for (auto&& c : cond) {
			Expr::Expr expr;
			if (!expr.Compile(fx.conditionCompiler, c)) {
				ThrowIfFailed(E_INVALIDARG, L"conditon error : " + L(fx.conditionCompiler.error + " : " + c), __FILEW__, __LINE__);
			}
			v.push_back(expr);
		}
		return v;
	}

	bool PassCond(FX& fx, const std::vector<Expr::Expr>& conds)
	{
		for (auto&& c : conds) {
			if (c.Eval() == 0.0f)
				return false;
		}
		return true;
	}


	/* 以下、古いPassCond
	//パス起動条件を解析して、CB上のインデックスを返す。コントローラが無い場合は-1を返す
	FXPassCond ParsePassCond1(FX& fx, const std::string& cond)
	{
		FXPassCond pc = {};

		if (cond.empty()) {
			return pc;
		}

		pc.always = false;

		//配列の場合は添え字をdoppelIndexとする
		int didx = 0;
		auto idx = cond.find('[');
		std::string var = cond;
		if (idx == std::string::npos) {
			didx = 0;
		} else {
			var = var.substr(0, idx);
			auto idx2 = cond.find(']');
			if (idx2 == std::string::npos || idx2 <= idx) {
				didx = std::atoi(cond.substr(idx + 1).c_str());
			} else {
				didx = std::atoi(cond.substr(idx + 1, idx2 - idx - 1).c_str());
			}
		}

		//ホワイトスペース削除
		var.erase(std::remove_if(var.begin(), var.end(), ::isspace), var.end());
		if (var.empty()) {
			ThrowIfFailed(E_INVALIDARG, L"invalid pass condition " + L(cond), __FILEW__, __LINE__);
			return {};
		}

		//演算子があるか？
		if (var[0] == '!') {
			pc.op = FXPassCond::opNegate;
			var = var.substr(1);
		}
		if (var.empty()) {
			ThrowIfFailed(E_INVALIDARG, L"invalid pass condition " + L(cond), __FILEW__, __LINE__);
			return {};
		}

		std::string operand;
		auto opLambda = [&](const std::string& op, int aop) {
			//すでにマッチした演算子があるなら帰る
			if (pc.op != FXPassCond::opIs && pc.op != FXPassCond::opNegate)
				return;
			//演算子の前後で変数名とオペランドとして分ける
			auto o = var.find(op);
			if (o != std::string::npos) {
				pc.op = aop;
				operand = var.substr(o + op.size());
				var = var.substr(0, o);
				if (var.empty() || operand.empty()) {
					ThrowIfFailed(E_INVALIDARG, L"invalid pass condition " + L(cond), __FILEW__, __LINE__);
				}
			}
		};

		opLambda("==", FXPassCond::opEqual);
		opLambda("!=", FXPassCond::opNeq);
		opLambda("<=", FXPassCond::opLeq);
		opLambda("<", FXPassCond::opLess);
		opLambda(">=", FXPassCond::opGeq);
		opLambda(">", FXPassCond::opGreater);

		auto pcLambda = [&](const std::string& t) {
			if (t == "bool") {
				pc.type = FXPassCond::tBool;
				if (pc.op != FXPassCond::opIs && pc.op != FXPassCond::opNegate) {
					ThrowIfFailed(E_INVALIDARG, L"invalid pass condition " + L(cond), __FILEW__, __LINE__);
				}
			} else if (t == "int") {
				pc.type = FXPassCond::tInt;
				if (!operand.empty())
					pc.operandI = std::atoi(operand.c_str());
			} else if (t == "float") {
				pc.type = FXPassCond::tFloat;
				if (!operand.empty())
					pc.operandF = std::atoi(operand.c_str());
			}
		};

		//コントローラ変数群を探索
		for (auto&& c : fx.controllers) {
			if (c.name == var && c.doppelIndex == didx) {
				pc.ControllerCBoffset = c.offset;
				pcLambda(c.type);
				return pc;
			}
		}
		//無かったらコンスタントバッファを探索
		if (fx.usedConfig.CBPointers.contains(var)) {
			auto& c = fx.usedConfig.CBPointers[var];
			pc.CBoffset = c.first;
			std::string t = (c.second == 0) ? "float" : "int";
			pcLambda(t);
			return pc;
		}

		
		ThrowIfFailed(E_INVALIDARG, L"invalid pass condition " + L(cond), __FILEW__, __LINE__);
		return {};
	}
	std::vector<FXPassCond> ParsePassCond(FX& fx, const std::vector<std::string>& cond)
	{
		std::vector<FXPassCond> v;
		for (auto&& c : cond) {
			v.push_back(ParsePassCond1(fx, c));
		}
		return v;
	}

	//パス起動条件を満たしているかチェック
	bool PassCond1(const FX& fx, const FXPassCond& cond)
	{
		if (cond.always)
			return true;

		int value;
		if (cond.CBoffset >= 0) {
			value = *reinterpret_cast<int*>(fx.usedConfig.pCB + cond.CBoffset);
		} else {
			value = *reinterpret_cast<int*>((char*)fx.controllerCB.pData + cond.ControllerCBoffset);
		}

		if (cond.type == FXPassCond::tBool) {
			if (cond.op == FXPassCond::opNegate) {
				value = (value!=0);
			}
		} else if (cond.type == FXPassCond::tFloat) {
			float f;
			memcpy(&f, &value, 4);
			switch (cond.op) {
				case FXPassCond::opIs:value = (f != 0.0f); break;
				case FXPassCond::opNegate:value = (f == 0.0f); break;
				case FXPassCond::opEqual:value = (f == cond.operandF); break;
				case FXPassCond::opNeq:value = (f != cond.operandF); break;
				case FXPassCond::opLess:value = (f < cond.operandF); break;
				case FXPassCond::opLeq:value = (f <= cond.operandF); break;
				case FXPassCond::opGreater:value = (f > cond.operandF); break;
				case FXPassCond::opGeq:value = (f >= cond.operandF); break;
			}
		} else if (cond.type == FXPassCond::tInt) {
			switch (cond.op) {
				case FXPassCond::opIs:value = (value != 0); break;
				case FXPassCond::opNegate:value = (value == 0); break;
				case FXPassCond::opEqual:value = (value == cond.operandI); break;
				case FXPassCond::opNeq:value = (value != cond.operandI); break;
				case FXPassCond::opLess:value = (value < cond.operandI); break;
				case FXPassCond::opLeq:value = (value <= cond.operandI); break;
				case FXPassCond::opGreater:value = (value > cond.operandI); break;
				case FXPassCond::opGeq:value = (value >= cond.operandI); break;
			}
		}
		return value;
	}

	//パスの起動条件を全部満たしているか？
	bool PassCond(FX& fx, const std::vector<FXPassCond>& conds)
	{
		for (auto&& c : conds) {
			if (!PassCond1(fx, c))
				return false;
		}
		return true;
	}
	*/

	void FX::Render()
	{
		int iPass = 0;
		int iFunctional = 0;
		for (int i = 0;  auto && order : passOrder) {
			if (order < FXPassType::copy) {
				auto& pass = passes[iPass];
				//条件チェック
				if (!PassCond(*this, passCond[i])) {
					iPass++;
					i++;
					continue;
				}

				//デフォーマならRootConstの設定
				if (category == FXCategory::deform) {
					pass.SetRootConst(2, usedConfig.deformIndex);
					pass.SetRootConst(3, usedConfig.deformOrder);
				}

				//デバッグ用
				//auto [err, desc] = pass.Check();
				//CopyTextToClipboard(desc);
				auto& s = outputSize[i];
				if (pass.type() == PassType::compute) {
					pass.Compute(s.width, s.height, s.depth);
				} else {
					if (pass.type() == PassType::rasterizer) {
						auto ramo = passRamos[i];
						if (ramo == FXRasterModelTarget::all) {
							pass.OrderedRender(s.width, s.height, s.depth, usedConfig.rasterOrder);
						} else if (ramo == FXRasterModelTarget::self) {
							pass.OrderedRender(s.width, s.height, s.depth, {usedConfig.selfIndex});
						} else if (ramo == FXRasterModelTarget::other) {
							auto o = usedConfig.rasterOrder;
							std::erase_if(o, [&](int i) { return i == usedConfig.selfIndex; });
							pass.OrderedRender(s.width, s.height, s.depth, o);
						} else if (ramo == FXRasterModelTarget::buffer) {
							pass.OrderedRender(s.width, s.height, s.depth, { 0 });
						}
					} else
						pass.Render(s.width, s.height, s.depth);
				}
				iPass++;
			} else {
				//条件チェック
				if (!PassCond(*this, passCond[i])) {
					iFunctional++;
					i++;
					continue;
				}
				auto& fpass = funcPasses[iFunctional];
				switch (fpass.type) {
				case FXPassType::copy:
					m_dxr->CopyResource(*fpass.copyDest, *fpass.copySrc);
					break;
				case FXPassType::clearUAV: {
					//DHのセット
					ID3D12DescriptorHeap* heaps[] = { fpass.DH.Get() };
					m_dxr->CommandList()->SetDescriptorHeaps(1, heaps);

					//必要ならUAVに遷移
					std::vector<D3D12_RESOURCE_BARRIER> bar;
					m_dxr->AddBarrier(fpass.clearTarget, bar, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					if (!bar.empty())
						m_dxr->CommandList()->ResourceBarrier(bar.size(), bar.data());

					//クリア
					m_dxr->CommandList()->ClearUnorderedAccessViewFloat(fpass.m_gpuHandle, fpass.m_cpuHandle, fpass.clearTarget->res.Get(), (float*)&fpass.clearValue, 0, nullptr);

					//UAVバリア
					bar = { CD3DX12_RESOURCE_BARRIER::UAV(fpass.clearTarget->res.Get()) };
					m_dxr->CommandList()->ResourceBarrier(bar.size(), bar.data());
					break;
				}
				case FXPassType::clearRTV: {
					//必要ならRTVに遷移
					std::vector<D3D12_RESOURCE_BARRIER> bar;
					m_dxr->AddBarrier(fpass.clearTarget, bar, D3D12_RESOURCE_STATE_RENDER_TARGET);
					if (!bar.empty())
						m_dxr->CommandList()->ResourceBarrier(bar.size(), bar.data());

					//RTV設定
					m_dxr->CommandList()->OMSetRenderTargets(1, &fpass.m_cpuHandle, true, nullptr);

					//クリア
					m_dxr->CommandList()->ClearRenderTargetView(fpass.m_cpuHandle, (float*)&fpass.clearValue, 0, nullptr);

					break;
				}
				case FXPassType::mipmapgen: {
					/*
						m_dxr->ExecuteCommandList();
						graphic = false;
						auto desc = fpass.mipTarget->desc();
						int nmip = desc.MipLevels;
						int w = desc.Width;
						int h = desc.Height;
						m_dxr->OpenCommandListCS();
						for (int i = 0; i < nmip-1; i++) {
							w = max(1,w>>1);
							h = max(1,h>>1);
							fpass.mipPass[i].Compute(w, h, 1, true);
						}
					}
					*/
					auto desc = fpass.mipTarget->desc();
					int nmip = desc.MipLevels;
					int w = desc.Width;
					int h = desc.Height;
					for (int i = 0; i < nmip - 1; i++) {
						w = max(1, w >> 1);
						h = max(1, h >> 1);
						fpass.mipPass[i].Render(w, h, 1, true);
					}
				}
				}
				iFunctional++;
			}
			i++;
		}

	}

	void FX::Update()
	{
		if (hasMatDesc()) {
			MatDescUniqueTextures();
			SetupMatDescSRV();
		}

		for (auto&& pass : passes) {
			pass.Update();
		}
		for (auto&& fpass : funcPasses) {
			fpass.Update();
		}
	}

	//読み込みの終わったFXとFXPassの状態から出力サイズを決定する
	FXSize FX::ResolveOutputSize(const FXPass& pass)
	{
		const auto& o = pass.outputSize;

		//絶対サイズで指定されていればそのまま
		if (o.absolute)
			return o;

		FXSize abs = {};
		//リソースを検索、あればそのリソースのサイズを基準にする
		auto resLambda = [&](const std::string& name) {
			if (resMap.contains(name)) {
				FXSize s = FXSize(resMap[name]);
				abs = FXSize::convert(s, o);
			} else if (name == "DEFAULT_RTSIZE") {
				auto desc = usedConfig.defaultRT.tex->desc();
				auto s = FXSize( desc.Width, desc.Height, 1, 2);
				abs = FXSize::convert(s, o);
			} else if (name == "VERTEXCOUNT" && category == FXCategory::deform) {
				abs = FXSize::convert(FXSize(usedConfig.deformOutputCount, 1,1, 1), o);
			} else {
				std::string msg = std::format("{}@{} can't find outputSize.base {}", pass.name, (char*)filename().filename().u8string().c_str(), name);
				ThrowIfFailed(E_INVALIDARG, L(msg), __FILEW__, __LINE__);
			}
		};

		//baseが空の場合は RTV[0]->DSV->UAV[0]の順で検索、どれも無い場合、エフェクトのデフォルト出力サイズを基準にする
		if (!o.base.empty()) {
			resLambda(o.base);
		} else if (!pass.RTV.empty()) {
			resLambda(pass.RTV[0].name);
		} else if (!pass.DSV.name.empty()) {
			resLambda(pass.DSV.name);
		} else if (!pass.UAV.empty()) {
			resLambda(pass.UAV[0].name);
		} else {
			abs = FXSize::convert(FXSize(m_tech.defaultOutputSize.x, m_tech.defaultOutputSize.y, m_tech.defaultOutputSize.z, m_tech.defaultOutputDimension), o);
		}

		return abs;
	}

	void FX::Resize(int width, int height)
	{
		FXSize src((uint32_t)width,(uint32_t)height,1, 2);	//baseに何も指定が無い場合のサイズ
		if (category != FXCategory::deform) {
			m_tech.defaultOutputSize = { (UINT)width, (UINT)height, 1 };
		}
		FXSize abs;

		for (int i = 0; i < textures.size(); i++) {
			if (textureSizeDependency[i]) {
				abs = FXSize::convert(src, textureSize[i]);
				bool uav = ((textures[i].caps & ResCaps::uav) != ResCaps::none);
				bool dsv = ((textures[i].caps & ResCaps::dsv) != ResCaps::none);
				auto desc = textures[i].desc();
				int miplevels = (desc.MipLevels == 1) ? 1 : 0;
				auto texname = textures[i].Name();
				auto texflag = D3D12_RESOURCE_FLAG_NONE;
				textures[i] = m_dxr->CreateTex2D(abs.width, abs.height, desc.Format, miplevels, D3D12_HEAP_FLAG_NONE, desc.Flags);
				textures[i].SetName(texname.c_str());
			}
		}
		for (int i = 0; i < buffers.size(); i++) {
			if (bufferSizeDependency[i]) {
				abs = FXSize::convert(src, bufferSize[i]);
				bool uav = ((buffers[i].caps & ResCaps::uav) != ResCaps::none);
				auto bufname = buffers[i].Name();
				if (uav)
					buffers[i] = m_dxr->CreateRWBuf(buffers[i].elemSize, abs.width);
				else
					buffers[i] = m_dxr->CreateBuf(nullptr, buffers[i].elemSize, abs.width);
				buffers[i].SetName(bufname.c_str());
			}
		}


		for (int i = 0; i < outputSize.size(); i++) {
			//リソースを検索してそのサイズからabsを作る
			auto resLambda = [&](const std::string& name) {
				FXSize s = src;
				if (resMap.contains(name)) {
					s = FXSize(resMap[name]);
					abs = FXSize::convert(s, originalOutputSize[i]);
				}
			};

			if (outputSizeDependency[i]) {
				outputSize[i] = ResolveOutputSize(m_tech.passes[i]);
				
				//DEB(L"{} ratio : {},{},{}", passes[i].name, o.ratio.x, o.ratio.y, o.ratio.z);
				//DEB(L"{} size resolved : {},{},{}", passes[i].name, abs.width, abs.height, abs.depth);
			}
		}

		Update();
	}

	void FX::Clear(bool keepMatDesc, bool keepPass)
	{
		memos.clear();
		controllerCB = {};
		globalCB = {};
		controllers.clear();
		samplers.clear();
		textures.clear();
		textures3D.clear();
		buffers.clear();
		passNames.clear();
		passOrder.clear();
		passRamos.clear();
		customLayout.clear();
		passVB.clear();
		passIB.clear();
		funcPasses.clear();
		passCond.clear();
		if (!keepPass) {
			passes.clear();
		}

		if (!keepMatDesc) {
			matDescs = {};
			table_idx = {};
			matDescValues = {};
			matDescTextures = {};
			matDescTextures3D = {};
			parser = {};
			m_texCache = {};
			m_texCache3D = {};
		}

		outputSize = {};
		outputSizeDependency = {};
		textureSizeDependency = {};
		texture3DSizeDependency = {};
		bufferSizeDependency = {};
		originalOutputSize = {};
		textureSize = {};
		texture3DSize = {};
		bufferSize = {};

		m_dmyBuf = {};
		m_dmyTex = {};
		m_dmyTex3D = {};

		sharedSrc = {};
		sharedRef = {};
		shareTags = {};

		texturemap = {};
		texture3Dmap = {};
		bufmap = {};

		matDescTexReg.clear();

		compiledHLSL = "";
	}



	//ifsからテキストを読み取って、ssにJSON部を入れる
	//よろずFXファイルがちゃんと入ってればtrueを返す
	//predにはJSON部が始まる前のテキストが格納される
	bool ExtractJSONPart(const std::string& filename, std::istream& ifs, std::stringstream& ss, std::string& pred)
	{
		std::vector<char> line(4096);
		int iLine = 0;

		//[YRZFX]を探す
		while (1) {
			ifs.getline(line.data(), line.size());
			std::string l = line.data();

			//見つかった
			if (l.find(JSONStartSign) != std::string::npos)
				break;

			if (ifs.eof()) {
				return false;
			}
			pred += l + "\n";
			iLine++;
		}

		//[YRZFX]の後の改行の真下からjson開始
		auto pos1 = ifs.tellg();

		//[HLSL]という文字列の直前まででjson終了。[HLSL]の次の行からソースコードの続きが始まる
		//json内には[HLSL]という文字列は含まれない事を想定している
		auto pos2 = ifs.tellg();
		while (!ifs.eof()) {
			ifs.getline(line.data(), line.size());
			std::string l = line.data();
			iLine++;
			if (l.find(JSONEndSign) != std::string::npos) {
				pos2 = ifs.tellg();
				break;
			}
		}
		if (ifs.eof()) {
			return false;
		}

		//[YRZFX]行の直後に戻って、[HLSL]の直前まで切り出す
		ifs.seekg(pos1);

		std::vector<char> buffer(pos2 - pos1);
		ifs.read(buffer.data(), buffer.size());

		// tellgで行を読む前の位置を取ろうとしたがバッファリングのせいかアテにならないので
		// 読んだ後の位置から前に戻って[HLSL]の[の1つ手前の位置を探す
		auto idx = buffer.size() - 1;
		while (1) {
			if (buffer[idx] == JSONEndSign[0])
				break;
			idx--;
		}
		idx--;

		//[の手前までの文字をssに流しこむ
		ss.write(buffer.data(), idx);

		//ifsの読み取り位置をBinaryDayoの改行の直後にする
		ifs.seekg(pos2);

		return true;
	}


	//ifsに格納されているjson部分を切り出してinfoに入れる、返り値はstartSign以前の行の内容
	template <class T>
	std::string FX::ReadHeader(const std::string& filename, std::istream& ifs, const std::string& rootname, T& info)
	{
		std::string pred;
		std::stringstream ss;

		if (!ExtractJSONPart(filename, ifs, ss, pred)) {
			YRZ::ThrowIfFailed(E_FAIL, L"not YRZFx file", __FILEW__, __LINE__);
		}

		//jsonnetでjsonに変換する
		jsonnet::Jsonnet jsonnetEngine;
		jsonnetEngine.init();
		std::string jsonOutput;
		bool ok = jsonnetEngine.evaluateSnippet(fs::path(filename).generic_string(), ss.str(), &jsonOutput);	//Jsonnetではパスの区切り文字は\ではなく/なので
		if (!ok) {
			ThrowIfFailed(E_FAIL, L"Jsonnet evaluation failed: " + L(jsonnetEngine.lastError()), __FILEW__, __LINE__);
		}
		ss.str(jsonOutput);


		//cerealでデシリアライズする
		cereal::JSONInputArchive archive(ss);
		archive(cereal::make_nvp(rootname, info));

		return pred;
	}

	//rapidjsonより引用
	inline const std::string GetParseErrorEn(int parseErrorCode) {
		switch (parseErrorCode) {
		case 0:	return "No error.";
		case 1:	return "The document is empty.";
		case 2:	return "The document root must not be followed by other values.";
		case 3:	return "Invalid value.";
		case 4: return "Missing a name for object member.";
		case 5: return "Missing a colon after a name of object member.";
		case 6: return "Missing a comma or '}' after an object member.";
		case 7: return "Missing a comma or ']' after an array element.";
		case 8: return "Incorrect hex digit after \\u escape in string.";
		case 9: return "The surrogate pair in string is invalid.";
		case 10:return "Invalid escape character in string.";
		case 11:return "Missing a closing quotation mark in string.";
		case 12:return "Invalid encoding in string.";
		case 13:return "Number too big to be stored in double.";
		case 14:return "Miss fraction part in number.";
		case 15:return "Miss exponent in number.";
		case 16:return "Terminate parsing due to Handler error.";
		case 17:return "Unspecific syntax error.";
		default:return "Unknown error.";
		}
	}


	//JSON部分のエラーを文字列で返す
	std::string FX::ReportJSONError(const fs::path& path)
	{
		std::ifstream ifs(path,std::ios::binary);
		
		if (!ifs) {
			return "can't open " + path.string();
		}

		std::string pred;
		std::stringstream ss;
		const std::string startSign = "[YRZFX]";
		const std::string endSign = "[HLSL]";

		if (!ExtractJSONPart((char*)path.u8string().c_str(), ifs, ss, pred)) {
			return "not YRZFx file";
		}

		//rapidJSONに食わせる
		rapidjson::Document doc;
		doc.Parse(ss.str().c_str());
		if (doc.HasParseError()) {
			std::string err;
			err += "json error\n";
			err += GetParseErrorEn(doc.GetParseError());
			err += " at offset ";
			auto o = doc.GetErrorOffset();
			err += std::to_string(o);
			err += "\n";
			std::string str = ss.str();
			//エラーのある行を切り出す
			auto b = str.rfind("\n", o);
			if (b == std::string::npos)
				b = 0;
			auto e = str.substr(b + 1).find("\n");
			if (e == std::string::npos)
				err += str.substr(b);
			else
				err += str.substr(b, e+1);
			err += "\n";
			return err;
		}

		return "";
	}

	inline UINT Align(size_t size, UINT align) {
		return UINT(size + align - 1) & ~(align - 1);
	}

	//テクスチャリソースを作る際のフラグの作成
	auto MakeTexFlag = [](DXR* dxr, const auto& tex) {
		auto flag = D3D12_RESOURCE_FLAG_NONE;
		auto supflag = D3D12_FORMAT_SUPPORT1_NONE;
		if (tex.dsv()) {
			flag |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			supflag |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
		}
		if (tex.rtv()) {
			flag |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			supflag |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
		}
		if (tex.uav()) {
			flag |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			supflag |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
		}

		return std::tuple<D3D12_RESOURCE_FLAGS,D3D12_FORMAT_SUPPORT1>(flag, supflag);
	};

	//テクスチャリソースを作る際のフラグがサポートされている組み合わせなのかチェック
	auto CheckTexFlag = [&](DXR* dxr, const auto& tex, const auto supflag) {
		//フォーマットのチェック
		D3D12_FEATURE_DATA_FORMAT_SUPPORT sup = {};
		sup.Format = StringToDXGIFormat(tex.format);
		sup.Support1 = D3D12_FORMAT_SUPPORT1_NONE;
		sup.Support2 = D3D12_FORMAT_SUPPORT2_NONE;
		ThrowIfFailed(dxr->Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sup, sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)),
			std::format(L"unsupported texture format:{}", L(tex.format)),
			__FILEW__, __LINE__);

		//このフォーマットがサポートされているのは分かったが、ビューの割り当ては出来るのか？
		if (supflag && (sup.Support1 & supflag) == 0) {
			ThrowIfFailed(E_INVALIDARG, std::format(L"{} : unsupported combination of texture format({}) and view({})", L(tex.name), L(tex.format), L(tex.view)), __FILEW__, __LINE__);
		}
	};


	void FX::Load(DXR& dxr, const std::wstring& filename, FXShaderCache& cache, FXWatcher* watcher, const FXLoadConfig& param, bool updateSharedRef, bool restoreMatDesc, bool updateShader)
	{
		/* FXTech構造体の読み込み */

		std::wstring fxname = fs::path(filename).filename().wstring();
		LOG(L"Load FX : {}", fxname);

		Clear(false, !updateShader);	//読み込み前に以前の情報を全クリア
	
		m_dxr = &dxr;
		m_filename = filename;
		m_cache = &cache;
		m_watcher = watcher;
		usedConfig = param;
		m_path = fs::path(filename).parent_path();

		std::ifstream ifs(filename, std::ios::binary);
		if (!ifs) {
			ThrowIfFailed(E_FAIL, std::format(L"can't open file {}", filename), __FILEW__, __LINE__);
			return;
		}

		FXTech fx;
		auto predecessor = ReadHeader(u8(filename), ifs, "fx", fx);

		//HLSLソース書き込みストリームの用意
		std::stringstream ss;
		ss << predecessor << "\n";	//[YRZFZ]以前の内容を流し込む

		//ダミーリソース(切れた共有参照や、割り当ての無い材質注釈テクスチャ用)
		m_dmyBuf = m_dxr->CreateBuf(nullptr, 256, 1);
		m_dmyBuf.SetName(std::format(L"dmyBuf@{}", fxname).c_str());
		m_dmyTex = m_dxr->CreateTex2D(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
		m_dmyBuf.SetName(std::format(L"dmyTex@{}", fxname).c_str());
		m_dmyTex3D = m_dxr->CreateTex3D(8, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
		m_dmyTex3D.SetName(std::format(L"dmyTex3D@{}", fxname).c_str());

		//このエフェクト専用のテクスチャキャッシュ(材質注釈用)
		m_texCache.Init(*m_dxr, m_dmyTex);
		m_texCache3D.Init(*m_dxr, m_dmyTex3D);


		/*パス名のコピー */
		passNames.resize(fx.passes.size());
		for (int i = 0; i < fx.passes.size(); i++)
			passNames[i] = fx.passes[i].name;

		/* 共有タグ格納領域の確保 */
		shareTags.resize(fx.passes.size());

		/* 全パスで共通の情報の取得とHLSLへの書き込み */
		category = StringToFXCategory(fx.category);
		memos = fx.memos;

		//デフォルト出力先サイズの取得。
		switch (category) {
		//ポストプロセスとレンダラではデフォルト出力先は2Dテクスチャでなければならない
		case FXCategory::postprocess:
		case FXCategory::render:
			fx.defaultOutputDependency = true;
			fx.defaultOutputDimension = 2;
			if (param.defaultRT.tex != nullptr) {
				auto desc = param.defaultRT.tex->desc();
				fx.defaultOutputSize = { (uint32_t)desc.Width, (uint32_t)desc.Height, 1 };
			} else {
				fx.defaultOutputSize = { (uint32_t)dxr.Width(), (uint32_t)dxr.Height(), 1 };
			}
			break;
		//デフォーマではデフォルト出力先は1Dバッファ
		case FXCategory::deform:
			fx.defaultOutputDependency = false;
			fx.defaultOutputDimension = 1;
			fx.defaultOutputSize = { (UINT)param.deformOutputCount, 1,1 };
			break;
		}


		//書き込み開始レジスタ番号
		int treg = 0, ureg = 0, breg = 0;
		if (param.definedPass != nullptr) {
			treg = param.definedPass->SRV.size() <= param.SRVspace ? 0 : param.definedPass->SRV[param.SRVspace].size();
			ureg = param.definedPass->UAV.size();
			breg = param.definedPass->CBV.size();
		}

		//コントローラー用CBの定義
		if (!fx.controllers.empty()) {
			for (int i = 0; auto && item:fx.controllers) {
				size_t idx = item.name.find('[');
				if (item.name.back() == ']' && idx != std::string::npos) {
					//配列コントローラなら展開する
					int n = std::stoi(item.name.substr(idx+1));
					if (n <= 0) {
						ThrowIfFailed(E_INVALIDARG, std::format(L"The number of elements in the array {} is 0 or less, which is not allowed.", L(item.name)), __FILEW__, __LINE__);
					}
					//同名でdoppelIndexの違うn個のコントローラ変数ができる
					auto name = item.name.substr(0, idx);
					for (int j = 0; j < n; j++) {
						auto cont = item;
						cont.name = name;
						cont.doppelCount = n;
						cont.doppelIndex = j;
						controllers.push_back(cont);
					}
				} else {
					//そうでないならそのままコピー
					controllers.push_back(item);
				}
				i++;
			}

			ss << "cbuffer YRZFX_ControllerCB : register(b" << breg << ") {\n";
			int offset = 0;
			for (int i = 0; auto && item : controllers) {

				//item.typeに有ったsizeを格納するラムダ
				auto sizeLambda = [&](const std::string& type) {
					if (type == "bool" || type == "int") {
						return 4;
					} else if (type == "float") {
						return 4;
					} else if (type == "float2") {
						return 8;
					} else if (type == "float3") {
						return 12;
					} else if (type == "float4") {
						return 16;
					} else if (type == "float4x4") {
						return 64;
					} else {
						ThrowIfFailed(E_INVALIDARG, std::format(L"invalid type {} specified for controller {} ", L(type), L(item.controllerName)), __FILEW__, __LINE__);
						return 0;
					}
				};

				//パッキングルールについて、現状で分かっている事
				//- メンバのサイズは必ず4の倍数バイトである
				//- 16バイト境界について
				//  - 8バイト以上のメンバの場合、16バイト境界を跨ぐように配置が出来ない
				//  - 配列変数は全て16バイト境界の先頭に置かれる

				int size = sizeLambda(item.type);
				if (item.doppelIndex <= 0 && item.doppelCount <= 0) {
					//配列の要素ではない
					//オフセットが16の倍数では無い可能性を考える
					int candy = Align(offset, 16);	//候補地としては16の倍数バイトの場所
					if (offset != candy)	//元々オフセットが16の倍数バイトならそのままでいい
						if (offset + size > candy)
							offset = candy;	//16の倍数の場所をまたいでいるならオフセットを次の16の倍数の場所まで持っていく

					item.offset = offset;
					item.size = size;
					ss << "    " << item.type << " " << item.name << ";\n";
					offset += size;
				} else if (item.doppelIndex == 0) {
					//配列の先頭、配列の場合、各要素は16バイトアラインされる
					offset = Align(offset, 16);
					item.offset = offset;
					item.size = size;
					ss << "    " << item.type << " " << item.name << "[" << item.doppelCount << "];\n";
					offset += size;
				} else {
					//配列の続き
					offset = Align(offset, 16);
					item.offset = offset;
					item.size = size;
					offset += size;
				}
				//ホストアプリケーションにモデル名・アイテム名・型を渡して、モデル検索キー・アイテム検索キーを返してもらう
				param.ControllerQuery(param.selfIndex, item.doppelIndex, item.controllerName, item.item, item.type, item.iModel, item.iItem);
			}
			ss << "};\n";


			//controllers = fx.controllers;
			controllerCB = dxr.CreateCB(nullptr, offset);	//最後のアイテムのオフセット+サイズ, 256バイトアラインはcreateCBでやってくれる
			controllerCB.SetName(L"controllerCB");
		}
		breg++;

		//グローバル変数格納用CBの定義、これについてはhlslのソースには何も書かない
		globalCB = dxr.CreateCB(nullptr, fx.globalVarSize);
		globalCB.SetName(L"globalCB");

		//ここで読み込み元情報を保存(ロード時に決定する情報が定まったので)
		m_tech = fx;

		//サンプラー
		UINT sreg = 0;	//サンプラーのシェーダレジスタ
		for (auto&& presamp : param.definedPass->Samplers) {
			if (presamp.RegisterSpace == 0)
				sreg = max(sreg, presamp.ShaderRegister+1);
		}
		for (auto&& item : fx.samplers) {
			samplers.push_back(item.Convert());
			samplers.back().ShaderRegister = sreg;
			samplers.back().RegisterSpace = 0;
			ss << "sampler " << item.name << " : register(s" << sreg << ", space0);\n";
			sreg++;
		}

		//size.baseの予約ワード
		const std::set<std::string> reservedSizeBase = {
			"TOTALMATERIALCOUNT"
		};

		std::unordered_set<int> resolvedtex;			//サイズ解決済みのテクスチャの番号
		std::unordered_set<int> resolvedtex3D;			//サイズ解決済みのテクスチャの番号
		std::unordered_set<int> resolvedbuf;			//サイズ解決済みのテクスチャの番号
		std::unordered_map<std::string, bool> depBuf, depTex, depTex3D;	//リソース名→dependencyについての辞書

		//リサイズ時のための情報 〇〇Dependencyは画面のリサイズに伴って各サイズの変更も必要かどうか？を示す
		textureSizeDependency.resize(fx.textures.size(), false);
		texture3DSizeDependency.resize(fx.textures3D.size(), false);
		bufferSizeDependency.resize(fx.buffers.size(), false);
		outputSizeDependency.resize(fx.passes.size(), false);
		textureSize.resize(fx.textures.size());
		texture3DSize.resize(fx.textures3D.size());
		bufferSize.resize(fx.buffers.size());
		originalOutputSize.resize(fx.passes.size());
		auto findBaseLambda = [&](const auto& name, const auto& s, const auto& dict, bool* found = nullptr) {
			//baseが有効か、有効ならdependencyを返す
			bool dep = false;
			if (s.base.empty()) {
				dep = true;
			} else if (reservedSizeBase.contains(s.base)) {
				//size.baseが予約語だった場合は解決元は見つかったとして帰る
				if (found)
					*found = true;
			} else {
				if (dict.contains(s.base)) {
					dep = dict.at(s.base);
					if (found)
						*found = true;
				} else if (found == nullptr) {
					ThrowIfFailed(E_INVALIDARG, std::format(L"{}'s size.base {} is not defined", L(name), L(s.base)), __FILEW__, __LINE__);
				}
			}
			return dep;
		};

		//dep**からリソースを探してdependencyを返す
		auto findDepLambda = [&](const std::string& name, const std::string& passname) {
			if (depTex.contains(name)) {
				return depTex[name];
			} else if (depTex3D.contains(name)) {
				return depTex3D[name];
			} else if (depBuf.contains(name)) {
				return depBuf[name];
			} else if (name == "DEFAULT_RTSIZE") {
				return true;
			} else {
				ThrowIfFailed(E_INVALIDARG, std::format(L"pass {} : resource {} not found", L(passname), L(name)), __FILEW__, __LINE__);
				return false;
			}
		};

		auto findOutputBaseLambda = [&](const FXPass& p) {
			if (!p.outputSize.base.empty()) {
				return findDepLambda(p.outputSize.base, p.name);
			} else if (!p.RTV.empty()) {
				return findDepLambda(p.RTV[0].name, p.name);
			} else if (!p.DSV.name.empty()) {
				return findDepLambda(p.DSV.name, p.name);
			} else if (!p.UAV.empty()) {
				return findDepLambda(p.UAV[0].name, p.name);
			} else {
				return fx.defaultOutputDependency;
			}
		};

		for (int i = 0; i < fx.textures.size(); i++) {
			textureSize[i] = fx.textures[i].size;
			bool dep = findBaseLambda(fx.textures[i].name, fx.textures[i].size, depTex);
			textureSizeDependency[i] = (!textureSize[i].absolute && dep && fx.textures[i].filename.empty());
			depTex[fx.textures[i].name] = textureSizeDependency[i];
		}
		for (int i = 0; i < fx.textures3D.size(); i++) {
			texture3DSize[i] = fx.textures3D[i].size;
			bool dep = findBaseLambda(fx.textures3D[i].name, fx.textures3D[i].size, depTex3D);
			texture3DSizeDependency[i] = (!texture3DSize[i].absolute && dep && fx.textures3D[i].filename.empty());
			depTex3D[fx.textures3D[i].name] = texture3DSizeDependency[i];
		}
		for (int i = 0; i < fx.buffers.size(); i++) {
			bufferSize[i] = fx.buffers[i].size;
			bool dep = findBaseLambda(fx.buffers[i].name, fx.buffers[i].size, depBuf);
			bufferSizeDependency[i] = (!bufferSize[i].absolute && dep);
			depBuf[fx.buffers[i].name] = bufferSizeDependency[i];
		}
		for (int i = 0; i < fx.passes.size(); i++) {
			originalOutputSize[i] = fx.passes[i].outputSize;
			bool dep = findOutputBaseLambda(fx.passes[i]);
			outputSizeDependency[i] = (!originalOutputSize[i].absolute && dep);
		}

		textures.resize(fx.textures.size());
		textures3D.resize(fx.textures3D.size());
		buffers.resize(fx.buffers.size());


		/* リソースの作成 */

		//サイズの決定したテクスチャを作成
		//pending : 呼び出しもとでテクスチャオブジェクトを作る場合はtrue
		auto createTextureLambda = [&](FXTexture &item, int idx) {
			Tex2D ret;
			if (StringToFXShared(item.shared) == FXShared::ref) {
				//共有リソースへの参照の場合、参照元を探す
				auto p = FindSharedSource(item.name);
				if (p && p->type == ResType::tex2D) {
					ret = *dynamic_cast<Tex2D*>(p);	//Comポインタのコピーが起こるので共有元が解放されても内容は破棄されない
				} else {
					ret = m_dmyTex;
				}
			} else {
				auto [flags,supFlags] = MakeTexFlag(m_dxr, item);
				if (item.filename.empty()) {
					//空のテクスチャを作成
					int mip = item.mipmap ? 0 : 1;
					ret = dxr.CreateTex2D(item.size.width, item.size.height, StringToDXGIFormat(item.format), mip, D3D12_HEAP_FLAG_NONE, flags);
				} else {
					//ファイルから読み込む
					ret = dxr.CreateTex2D((m_path / L(item.filename)).wstring().c_str(), ColorSpace::linear, D3D12_HEAP_FLAG_NONE, item.mipmap, flags);
					item.format = DXGIFormatToU8(ret.desc().Format);	//↓でフォーマットとビューの組み合わせをチェックするためにitem.formatだけセットしとく
				}
				CheckTexFlag(m_dxr, item, supFlags);
				ret.SetName(L(item.name).c_str());
			}
			textures[idx] = ret;
			resMap[item.name] = &textures[idx];

			FXSize size;
			auto desc = ret.desc();
			size.absolute = true;	size.dimension = 2;
			size.width = desc.Width;	size.height = desc.Height;	size.depth = 1;
			item.size = size;
			item.format = DXGIFormatToU8(ret.desc().Format);
			if (item.type.empty())
				item.type = DXGIFormatToHLSLType(ret.desc().Format);


			//共有属性がある場合はリストに追加
			if (StringToFXShared(item.shared) == FXShared::source) {
				sharedSrc.push_back({item.name, ResType::tex2D, idx});
			} else if (StringToFXShared(item.shared) == FXShared::ref) {
				sharedRef.push_back({ item.name, ResType::tex2D, idx});
			}
		};

		auto createTexture3DLambda = [&](FXTexture3D& item, int idx) {
			Tex3D ret;
			if (StringToFXShared(item.shared) == FXShared::ref) {
				//共有リソースへの参照の場合、参照元を探す
				auto p = FindSharedSource(item.name);
				if (p && p->type == ResType::tex3D) {
					ret = *dynamic_cast<Tex3D*>(p);	//Comポインタのコピーが起こるので共有元が解放されても内容は破棄されない
				} else {
					ret = m_dmyTex3D;
				}
			} else {
				auto [flags, supFlags] = MakeTexFlag(m_dxr, item);
				//3Dテクスチャはdsvに指定できないのでここでチェック
				if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
					ThrowIfFailed(E_INVALIDARG, std::format(L"texture3D {} can't be use with dsv", L(item.name)), __FILEW__, __LINE__);
				}
				if (item.filename.empty()) {
					//空のテクスチャを作成
					int mip = item.mipmap ? 0 : 1;
					ret = dxr.CreateTex3D(item.size.width, item.size.height, item.size.depth, StringToDXGIFormat(item.format), mip, D3D12_HEAP_FLAG_NONE, flags);
				} else {
					//ファイルから読み込む
					ret = dxr.CreateTex3D((m_path / L(item.filename)).wstring().c_str(), ColorSpace::linear, D3D12_HEAP_FLAG_NONE, item.mipmap, flags);
					item.format = DXGIFormatToU8(ret.desc().Format);
				}
				CheckTexFlag(m_dxr, item, supFlags);
				ret.SetName(L(item.name).c_str());
			}
			textures3D[idx] = ret;
			resMap[item.name] = &textures3D[idx];

			FXSize size;
			auto desc = ret.desc();
			size.absolute = true;	size.dimension = 3;
			size.width = desc.Width;	size.height = desc.Height;	size.depth = desc.DepthOrArraySize;
			item.size = size;
			item.format = DXGIFormatToU8(ret.desc().Format);
			if (item.type.empty())
				item.type = DXGIFormatToHLSLType(ret.desc().Format);

			//共有属性がある場合はリストに追加
			if (StringToFXShared(item.shared) == FXShared::source) {
				sharedSrc.push_back({ item.name, ResType::tex3D, idx });
			} else if (StringToFXShared(item.shared) == FXShared::ref) {
				sharedRef.push_back({ item.name, ResType::tex3D, idx });
			}
		};

		auto createBufferLambda = [&](FXBuffer& item, int idx) {
			if (item.type.empty()) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"{} : buffer {} type not specified", m_filename, L(item.name)), __FILEW__, __LINE__);
			}
			Buf buf;
			if (StringToFXShared(item.shared) == FXShared::ref) {
				//共有リソースへの参照なら、参照元を探し、SRVに割り当てる
				auto p = FindSharedSource(item.name);
				if (p && p->type == ResType::buf) {
					buf = *dynamic_cast<Buf*>(p);
				} else {
					buf = m_dmyBuf;
				}
				auto desc = buf.desc();
				item.elemSize = buf.elemSize;
				item.size = {};
				item.size.dimension = 1;
				item.size.absolute = true;
				item.size.width = desc.Width / item.elemSize;
			} else {
				int elemSize = GetElemSize(item);
				if (item.uav()) {
					buf = dxr.CreateRWBuf(elemSize, item.size.width);
				} else {
					buf = dxr.CreateBuf(nullptr, elemSize, item.size.width);
				}
				if (!item.filename.empty()) {
					auto wstr = L(item.filename);
					std::ifstream ifs(m_path / wstr, std::ios::binary);
					if (!ifs) {
						ThrowIfFailed(E_FAIL, std::format(L"can't open {}", wstr), __FILEW__, __LINE__);
					}
					std::vector<char>buffer(elemSize * item.size.width);
					ifs.read(buffer.data(), buffer.size());
					ifs.close();
					dxr.Upload(buf, buffer.data());
				}
				buf.SetName(L(item.name).c_str());
			}
			buffers[idx] = buf;
			resMap[item.name] = &buffers[idx];

			//共有属性がある場合はリストに追加
			if (StringToFXShared(item.shared) == FXShared::source) {
				sharedSrc.push_back({ item.name, ResType::buf, idx });
			} else if (StringToFXShared(item.shared) == FXShared::ref) {
				sharedRef.push_back({ item.name, ResType::buf, idx });
			}
		};

		//jsonからリソースを準備する
		//まずはサイズが確定しているリソースだけ作成し、相対サイズのリソースについては●●mapに入れて作成は後回し
		for (int i = 0;  auto && item : fx.textures) {
			bool cond1 = !item.filename.empty() && (StringToFXShared(item.shared) != FXShared::ref);
			bool cond2 = item.size.absolute || (StringToFXShared(item.shared) == FXShared::ref);
			if (cond1 || cond2) {
				createTextureLambda(item, i);
				resolvedtex.insert(i);
			}
			texturemap[item.name] = i;
			i++;
		}
		for (int i = 0; auto && item : fx.textures3D) {
			bool cond1 = !item.filename.empty() && (StringToFXShared(item.shared) != FXShared::ref);
			bool cond2 = item.size.absolute || (StringToFXShared(item.shared) == FXShared::ref);
			if (cond1 || cond2) {
				createTexture3DLambda(item, i);
				resolvedtex3D.insert(i);
			}
			texture3Dmap[item.name] = i;
			i++;
		}
		for (int i = 0; auto && item : fx.buffers) {
			if (item.size.absolute || StringToFXShared(item.shared) == FXShared::ref) {
				createBufferLambda(item, i);
				bufmap[item.name] = i;
				resolvedbuf.insert(i);
			}
			i++;
		}

		//サイズの解決。baseに指定されたリソースを検索して相対サイズ→絶対サイズに変換
		auto resolveSizeLambda = [&](auto& size, bool texture = true) {
			if (size.absolute)
				return;
			FXSize src;
			if (size.base.empty()) {
				if (category == FXCategory::deform && texture) {
					//baseが空白の場合でもdeformのテクスチャはdefaultOutputSizeではなく画面のサイズ準拠
					auto rtDesc = param.defaultRT.tex->desc();
					src = FXSize(rtDesc.Width, rtDesc.Height, 1, 2);
				} else {
					src = FXSize(fx.defaultOutputSize.x, fx.defaultOutputSize.y, fx.defaultOutputSize.z, fx.defaultOutputDimension);
				}
			} else if (texturemap.contains(size.base)) {
				const auto& r = fx.textures[texturemap[size.base]];
				//共有リソースをサイズの決定元にしてはいけない
				if (StringToFXShared(r.shared) == FXShared::ref)
					ThrowIfFailed(E_INVALIDARG, std::format(L"resolving resource size failed, shared resources cannot be specified in size.base {}", L(size.base)), __FILEW__, __LINE__);
				src = r.size;
			} else if (texture3Dmap.contains(size.base)) {
				const auto& r = fx.textures3D[texture3Dmap[size.base]];
				if (StringToFXShared(r.shared) == FXShared::ref)
					ThrowIfFailed(E_INVALIDARG, std::format(L"resolving resource size failed, shared resources cannot be specified in size.base {}", L(size.base)), __FILEW__, __LINE__);
				src = r.size;
			} else if (bufmap.contains(size.base)) {
				const auto& r = fx.buffers[bufmap[size.base]];
				if (StringToFXShared(r.shared) == FXShared::ref)
					ThrowIfFailed(E_INVALIDARG, std::format(L"resolving resource size failed, shared resources cannot be specified in size.base {}", L(size.base)), __FILEW__, __LINE__);
				src = r.size;
			} else if (size.base == "TOTALMATERIALCOUNT") {
				//全モデルの材質数合計を基準とする1次元数
				UINT nMats = 0;
				//なんでかこう書かないとループせずにnMatsには最初のモデルの材質分しか加算されないんだが
				for (int i = 0; i < param.materialCount.size(); i++) {
					nMats += param.materialCount[i];
				}
				src = FXSize(max(1,nMats),1,1, 1);	//0要素のバッファは作れないので最低1確保する
			} else if (size.base == "VERTEXCOUNT" && category == FXCategory::deform) {
				src = FXSize(fx.defaultOutputSize.x, fx.defaultOutputSize.y, fx.defaultOutputSize.z, 1);
			} else
				ThrowIfFailed(E_INVALIDARG, std::format(L"resolving resource size failed, resource not found {}", L(size.base)), __FILEW__, __LINE__);
			size = FXSize::convert(src, size);
			};

		//相対サイズのテクスチャ・バッファの作成
		for (int i = 0;  auto && item : fx.textures) {
			if (!resolvedtex.contains(i)) {
				resolveSizeLambda(item.size);
				createTextureLambda(item, i);
				texturemap[item.name] = i;
			}
			i++;
		}
		for (int i = 0; auto && item : fx.textures3D) {
			if (!resolvedtex3D.contains(i)) {
				resolveSizeLambda(item.size);
				createTexture3DLambda(item, i);
				texture3Dmap[item.name] = i;
			}
			i++;
		}
		for (int i = 0; auto && item:fx.buffers) {
			if (!resolvedbuf.contains(i)) {
				resolveSizeLambda(item.size, false);
				createBufferLambda(item,i);
				bufmap[item.name] = i;
			}
			i++;
		}

		//材質注釈の基礎情報を得る(材質注釈の要不要と材質注釈テンプレート)
		if (!hasMatDesc() || !restoreMatDesc)	//既に設定された材質注釈があれば使う
			matDescs = fx.matDescs;
		std::string valStructName, texStructName, tex3DStructName;
		if (hasMatDesc()) {
			matDescs.matDescTemplate = ReadDescFile(fx.matDescs.matDescTemplate);	//テンプレートファイルを開く
			//構造体の定義
			parser.ParseTemplate(matDescs.matDescTemplate, m_path, this);
			valStructName = matDescs.name + "Value";
			ss << "struct " << valStructName << " {\n";
			for (int i = 0; i < parser.varmap.size(); i++) {
				auto it = std::find_if(parser.varmap.begin(), parser.varmap.end(), [i](const auto& p) { return p.second.order == i; });
				if (it != parser.varmap.end()) {
					auto& var = it->second;
					auto typestr = FXMatDescVar::TypeStr(var.type);
					if (var.nComp == 1)
						ss << "  " << typestr << " " << it->first << ";\n";	//float1 v だとlerp(rgb1,rgb2,v)とした時に返り値がrだけになるという罠があるのでfloat1ではなくfloatで出力する
					else
						ss << "  " << typestr << var.nComp << " " << it->first << ";\n";
				}
			}
			ss << "};\n";

			texStructName = matDescs.name + "Texture";
			ss << "struct " << texStructName << " {\n";
			for (auto&& [name, tex] : parser.texmap) {
				ss << "  bool has" << name << ";\n";
				ss << "  Texture2D<float4> " << name << ";\n";
			}
			ss << "};\n";

			tex3DStructName = matDescs.name + "Texture3D";
			ss << "struct " << tex3DStructName << " {\n";
			for (auto&& [name, tex] : parser.texmap3D) {
				ss << "  bool has" << name << ";\n";
				ss << "  Texture3D<float4> " << name << ";\n";
			}
			ss << "};\n";
		}

		//パスごとのリソース→レジスタ割り当て
		auto ureg_orig = ureg;
		auto treg_orig = treg;
		for (auto&& fxp : fx.passes) {
			//ファンクショナルパスはhlslコードと対応してないのでスキップ
			if (fxp.IsFunctional())
				continue;
			ss << "#ifdef YRZ_PASS_" << fxp.name << "\n";

			//RTV,DSV,UAVに指定されたテクスチャ・バッファはちゃんと全部存在するか？
			for (auto&& u : fxp.UAV) {
				bool found = std::find_if(fx.textures.begin(), fx.textures.end(), [&](const auto& t) {return u.name == t.name; }) != fx.textures.end();
				found |= std::find_if(fx.textures3D.begin(), fx.textures3D.end(), [&](const auto& t) {return u.name == t.name; }) != fx.textures3D.end();
				found |= std::find_if(fx.buffers.begin(), fx.buffers.end(), [&](const auto& t) {return u.name == t.name; }) != fx.buffers.end();
				if (!found)
					ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} UAV[] texture {} is not found", L(fxp.name), L(u.name)), __FILEW__, __LINE__);
			}
			int rtvdim = 0;
			XMUINT3 rtvsize = XMUINT3(0,0,0);
			for (auto&& u : fxp.RTV) {
				if (u.name.empty())
					continue;
				auto it2d = std::find_if(fx.textures.begin(), fx.textures.end(), [&](const auto& t) {return u.name == t.name; });
				auto it3d = std::find_if(fx.textures3D.begin(), fx.textures3D.end(), [&](const auto& t) {return u.name == t.name; });
				if (it2d == fx.textures.end() && it3d == fx.textures3D.end())
					ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} RTV[] texture {} is not found", L(fxp.name), L(u.name)), __FILEW__, __LINE__);
				//2Dと3Dが混在してないか？
				if (rtvdim == 0) {
					rtvdim = (it2d != fx.textures.end()) ? 2 : 3;
				} else {
					if ((rtvdim == 2 && it3d != fx.textures3D.end()) || (rtvdim == 3 && it2d != fx.textures.end()))
						ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} RTV[] must have resources of the same dimensions and size", L(fxp.name)), __FILEW__, __LINE__);
				}
				//サイズの違うリソースが割り当てられてないか？
				if (rtvsize.x == 0 && rtvsize.y == 0 && rtvsize.z == 0) {
					if (rtvdim == 2) {
						auto idx = texturemap.at(it2d->name);
						auto desc = textures[idx].desc();
						rtvsize.x = desc.Width;	rtvsize.y = desc.Height;	rtvsize.z = 1;
					} else {
						auto idx = texturemap.at(it3d->name);
						auto desc = textures3D[idx].desc();
						rtvsize.x = desc.Width;	rtvsize.y = desc.Height;	rtvsize.z = desc.DepthOrArraySize;
					}
				} else {
					if (rtvdim == 2) {
						auto idx = texturemap.at(it2d->name);
						auto desc = textures[idx].desc();
						if (rtvsize.x != desc.Width || rtvsize.y != desc.Height) 
							ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} RTV[] must have resources of the same dimensions and size", L(fxp.name)), __FILEW__, __LINE__);
					} else {
						auto idx = texturemap.at(it3d->name);
						auto desc = textures3D[idx].desc();
						if (rtvsize.x != desc.Width || rtvsize.y != desc.Height || rtvsize.z != desc.DepthOrArraySize)
							ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} RTV[] must have resources of the same dimensions and size", L(fxp.name)), __FILEW__, __LINE__);
					}
				}
			}
			if (!fxp.DSV.name.empty()) {
				bool found = std::find_if(fx.textures.begin(), fx.textures.end(), [&](const auto& t) {return fxp.DSV.name == t.name; }) != fx.textures.end();
				if (!found)
					ThrowIfFailed(E_INVALIDARG, std::format(L"pass:{} DSV texture {} is not found", L(fxp.name), L(fxp.DSV.name)), __FILEW__, __LINE__);
			}


			auto texWriteLambda = [&](const auto& item, const std::string& dim) {
				bool found = false;
				for (auto&& u : fxp.UAV) {
					if (item.name == u.name) {
						ss << "RWTexture" << dim << "<" << item.type << ">" << item.name << " : register(u" << ureg << ");\n";
						ureg++;
						found = true;
					}
				}
				for (auto&& r : fxp.RTV) {
					//他のパスでSRVに入れられて参照される事はあって、そういう時にコンパイルエラーが出るので、シンボルだけ有効にしておく
					if (item.name == r.name) {
						ss << "Texture" << dim << "<" << item.type << ">" << item.name << ";\n";
						found = true;
					}
				}

				if (item.name == fxp.DSV.name) {
					found = true;
				}

				if (!found) {
					ss << "Texture" << dim << "<" << item.type << ">" << item.name << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
					treg++;
				}

			};

			//UAV・RTV・DSVに入っているテクスチャでないなら、SRVで参照できるようにする
			for (int i = 0; auto && item : fx.textures) {
				if (!texturemap.contains(item.name))
					ThrowIfFailed(E_INVALIDARG, std::format(L"texture {} is not defined", L(item.name)), __FILEW__, __LINE__);
				texWriteLambda(item, "2D");
				i++;
			}

			//同様に、3Dテクスチャ
			for (int i = 0; auto && item : fx.textures3D) {
				if (!texture3Dmap.contains(item.name))
					ThrowIfFailed(E_INVALIDARG, std::format(L"texture3D {} is not defined", L(item.name)), __FILEW__, __LINE__);
				texWriteLambda(item, "3D");
				i++;
			}

			//同様に、バッファ
			for (int i = 0; auto && item : fx.buffers) {
				if (!bufmap.contains(item.name)) {
					ThrowIfFailed(E_INVALIDARG, std::format(L"buffer {} is not defined", L(item.name)), __FILEW__, __LINE__);
				}
				bool found = false;
				for (auto&& u : fxp.UAV) {
					if (item.name == u.name) {
						ss << "RWStructuredBuffer" << "<" << item.type << ">" << item.name << " : register(u" << ureg << ");\n";
						ureg++;
						found = true;
					}
				}
				if (!found) {
					ss << "StructuredBuffer" << "<" << item.type << ">" << item.name << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
					treg++;
				}

				i++;
			}


			//マテリアルストレージに入っているインデックス用バッファ
			if (hasMatDesc()) {
				ss << "StructuredBuffer<uint> " << matDescs.name << "_idx " << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
				treg++;
				ss << "StructuredBuffer<uint> " << matDescs.name << "_tex " << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
				treg++;
				ss << "StructuredBuffer<uint> " << matDescs.name << "_tex3D " << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
				treg++;
				ss << "StructuredBuffer<" << matDescs.name << "Value> " << matDescs.name << "_value" << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
				treg++;
				ss << "Texture2D<float4> " << matDescs.name << "_texture[]" << " : register(t" << treg << ", space" << param.SRVspace << "); \n";
				treg += parser.nTex * matDescs.Count();
				ss << "Texture3D<float4> " << matDescs.name << "_texture3D[]" << " : register(t0, space" << param.SRVspace3DTex << "); \n";
			}

			ss << "#endif\n";
			ureg = ureg_orig;
			treg = treg_orig;
		}

		//材質注釈ゲッター関数の定義
		if (hasMatDesc()) {
			ss << valStructName << " Get" << valStructName << "(uint ID, uint subID)\n{\n";
			ss << "  " << valStructName << " m = (" << valStructName << ")0; \n";
			ss << "  uint imat = " << matDescs.name << "_idx[ID] + subID;\n";
			ss << "  m = " << matDescs.name << "_value[imat];\n";
			ss << "  return m;\n}\n";

			ss << texStructName << " Get" << texStructName << "(uint ID, uint subID)\n{\n";
			ss << "  " << texStructName << " m = (" << texStructName << ")0; \n";
			ss << "  uint imat = " << matDescs.name << "_idx[ID] + subID;\n";
			ss << "  uint tidx = imat * " << parser.nTex << "; \n";
			for (auto&& [name, tex] : parser.texmap) {
				std::string ta = matDescs.name + "_tex[tidx + " + std::to_string(tex) + "]";
				ss << "  m.has" << name << " = (" << ta << " != -1);\n";
				ss << "  m." << name << " = " << matDescs.name << "_texture[" << ta << "]; \n";
			}
			ss << "  return m;\n}\n";


			ss << tex3DStructName << " Get" << tex3DStructName << "(uint ID, uint subID)\n{\n";
			ss << "  " << tex3DStructName << " m = (" << tex3DStructName << ")0; \n";
			ss << "  uint imat = " << matDescs.name << "_idx[ID] + subID;\n";
			ss << "  uint tidx = imat * " << parser.nTex3D << "; \n";
			for (auto&& [name, tex] : parser.texmap3D) {
				std::string ta = matDescs.name + "_tex3D[tidx + " + std::to_string(tex) + "]";
				ss << "  m.has" << name << " = (" << ta << " != -1);\n";
				ss << "  m." << name << " = " << matDescs.name << "_texture3D[" << ta << "]; \n";
			}
			ss << "  return m;\n}\n";
		}


		/* HLSLファイルの完成 */
		std::string resstr = ss.str();

		//code部分を読む
		for (auto&& c : fx.code)
			resstr += c + "\n";
	
		//入力ファイルの残り部分をhlslのソースとして読み込む
		std::stringstream hlslss;
		hlslss << ifs.rdbuf();
		std::string hlslstr = hlslss.str();
		//さっきまでで作ったヘッダとくっつける
		std::string sourcestr = resstr + hlslstr;
		//ここまででファイルの役割は終了
		ifs.close();

		//テスト用…JSONを解釈してhlslとくっつけたコンパイル対象のソースをテスト用ファイルへ出力
		//std::ofstream testofs("_YRZFX_testoutput.hlsl", std::ios::binary);
		//testofs << sourcestr;
		//testofs.close();

		compiledHLSL = sourcestr;


		/* 各パスの作成 */

		//パス起動条件用コンパイラ
		conditionCompiler = {};
		for (auto&& c : controllers) {
			std::string name = c.name;
			if (c.doppelCount >= 1) {
				//配列の添え字付き
				name += "[" + std::to_string(c.doppelIndex) + "]";
			}
			char* ptr = (char*)controllerCB.pData + c.offset;
			if (c.type == "float")
				conditionCompiler.fvars[name] = (float*)ptr;
			else if (c.type == "int" || c.type == "uint")
				conditionCompiler.ivars[name] = (int*)ptr;
		}
		for (auto&& [key, p] : usedConfig.CBPointers) {
			if (p.second == 0) {
				conditionCompiler.fvars[key] = (float*)(usedConfig.pCB + p.first);
			} else {
				conditionCompiler.ivars[key] = (int*)(usedConfig.pCB + p.first);
			}
		}
		//材質注釈用コンパイラ
		mdCompiler = conditionCompiler;

		//材質注釈用配列の初期化
		if (hasMatDesc()) {
			//各モデルの各材質毎の材質注釈ファイル名
			std::vector<std::vector<std::string>> mats(param.materialCount.size());
			if (restoreMatDesc) {
				//バックアップから復元
				mats = param.mdb.sourceFiles;
			} else {
				//デフォルトで埋める
				for (int i = 0; auto && m : mats) {
					m.resize(param.materialCount[i], matDescs.defaultFile);
					i++;
				}
			}
			SetMatDescAll(mats);
		}


		//リソースを検索して無かったら例外を出すラムダ
		auto findTexLambda = [&](std::string name)->Res* {
			if (texturemap.contains(name))
				return &textures[texturemap[name]];
			ThrowIfFailed(E_INVALIDARG, std::format(L"texture {} not found", L(name)), __FILEW__, __LINE__);
			return nullptr;
		};

		auto findTex3DLambda = [&](std::string name)->Res* {
			if (texture3Dmap.contains(name))
				return &textures3D[texture3Dmap[name]];
			ThrowIfFailed(E_INVALIDARG, std::format(L"texture3D {} not found", L(name)), __FILEW__, __LINE__);
			return nullptr;
			};

		//2Dでも3Dでもいいからテクスチャを探す場合
		auto findTexNDLambda = [&](std::string name)->Res* {
			if (texturemap.contains(name))
				return &textures[texturemap[name]];
			if (texture3Dmap.contains(name))
				return &textures3D[texture3Dmap[name]];
			ThrowIfFailed(E_INVALIDARG, std::format(L"texture {} not found", L(name)), __FILEW__, __LINE__);
			return nullptr;
			};

		auto findBufLambda = [&](std::string name)->Buf* {
			if (bufmap.contains(name))
				return &buffers[bufmap[name]];
			ThrowIfFailed(E_INVALIDARG, std::format(L"buffer {} not found", L(name)), __FILEW__, __LINE__);
			return nullptr;
			};

		//なんらかのリソースを探す場合
		auto findResLambda = [&](std::string name)->Res* {
			if (texturemap.contains(name))
				return &textures[texturemap[name]];
			if (texture3Dmap.contains(name))
				return &textures3D[texture3Dmap[name]];
			if (bufmap.contains(name))
				return &buffers[bufmap[name]];
			ThrowIfFailed(E_INVALIDARG, std::format(L"resource {} not found", L(name)), __FILEW__, __LINE__);
			return nullptr;
			};

		//パスごとのリソースの設定
		int iPass = 0;
		matDescTexReg.resize(fx.passes.size());
		for (auto&& item : fx.passes) {
			FXPassType passType = StringToFXPassType(item.type);
			passOrder.push_back(passType);
			auto ramo = StringToFXRasterModelTarget(item.rasterModelTarget);
			passRamos.push_back(ramo);
			passCond.push_back(ParsePassCond(*this, item.conditions));

			//ラスタライズの対象がリソース内のバッファだった場合、passVB,passIBにセット
			if (ramo == FXRasterModelTarget::buffer) {
				if (item.rasterVB.empty())
					passVB.push_back(nullptr);
				else
					passVB.push_back(findBufLambda(item.rasterVB));
				passIB.push_back(findBufLambda(item.rasterIB));
				customLayout.push_back(item.layout);
			} else {
				passVB.push_back(nullptr);
				passIB.push_back(nullptr);
				customLayout.push_back({});
			}

			//ファンクショナルパス
			if (item.IsFunctional()) {
				FXFunctionalPass p(dxr);
				p.type = passType;
				switch (passType) {
				case FXPassType::copy: {
					p.copyDest = findResLambda(item.dest);
					p.copySrc = findResLambda(item.src);
					auto sd = p.copySrc->desc();
					auto dd = p.copyDest->desc();
					//フォーマットなどが同じでないとコピーできないので
					if (sd.Format != dd.Format)
						ThrowIfFailed(E_INVALIDARG, std::format(L"copy : resource {} and {} don't have same format", L(item.src), L(item.dest)), __FILEW__, __LINE__);
					if (sd.Dimension != dd.Dimension)
						ThrowIfFailed(E_INVALIDARG, std::format(L"copy : resource {} and {} don't have same dimension", L(item.src), L(item.dest)), __FILEW__, __LINE__);
					if ((sd.Width != dd.Width) || (sd.Height != dd.Height) || (sd.DepthOrArraySize != dd.DepthOrArraySize))
						ThrowIfFailed(E_INVALIDARG, std::format(L"copy : resource {} and {} don't have same size", L(item.src), L(item.dest)), __FILEW__, __LINE__);
					break;
				}
				case FXPassType::clearRTV:
					p.clearTarget = findTexNDLambda(item.target);
					if ((p.clearTarget->caps & ResCaps::rtv) == ResCaps::none) {
						ThrowIfFailed(E_INVALIDARG, std::format(L"clearRTV : resource {} is not RTV capable", L(item.target)), __FILEW__, __LINE__);
					}
					p.clearValue = item.value;
					p.Update();
					break;
				case FXPassType::clearUAV:
					p.clearTarget = findResLambda(item.target);
					if ((p.clearTarget->caps & ResCaps::uav) == ResCaps::none) {
						ThrowIfFailed(E_INVALIDARG, std::format(L"clearUAV : resource {} is not UAV capable", L(item.target)), __FILEW__, __LINE__);
					}
					p.clearValue = item.value;
					p.Update();
					break;
				case FXPassType::mipmapgen:
					p.mipTarget = dynamic_cast<Tex2D*>(findTexLambda(item.target));
					p.SetupMipmapgen(p.mipTarget);
					break;
				}
				funcPasses.push_back(std::move(p));
				//今のところ、ファンクショナルパスではoutputSizeを読んでないのでできとう
				FXSize size = item.outputSize;
				resolveSizeLambda(size);
				outputSize.push_back(size);
				continue;	//ファンクショナルパスならここまでで終わり
			}

			
			//パスオブジェクトの作成または更新。初読み込みの場合などシェーダごと更新する場合はパスを再生成する
			//シェーダの更新が必要ない場合はオブジェクト自体は使いまわしてリソース一覧だけ再作成する
			if (updateShader) {
				passes.push_back(Pass(&dxr));
				passes[iPass].name = std::format(L"{}@{}",L(item.name), fs::path(filename).filename().wstring());
			}

			Pass& pass = passes[iPass];

			if (!updateShader)
				pass.Flush();

			//定義済みパスからコピー
			if (param.definedPass != nullptr) {
				pass.CBV = param.definedPass->CBV;
				pass.DSV = param.definedPass->DSV;
				pass.RTV = param.definedPass->RTV;
				pass.SRV = param.definedPass->SRV;
				pass.UAV = param.definedPass->UAV;
				pass.Samplers = param.definedPass->Samplers;
			}
			if (pass.SRV.size() <= param.SRVspace) {
				pass.SRV.resize(param.SRVspace + 1);
			}
			if (pass.SRV.size() <= param.SRVspace3DTex) {
				pass.SRV.resize(param.SRVspace3DTex + 1);
			}

			//リソースの設定
			pass.Samplers.insert(pass.Samplers.end(), samplers.begin(), samplers.end());

			if (!controllers.empty()) {
				pass.CBV.push_back(&controllerCB);
				pass.CBV.push_back(&globalCB);
			}

			auto checkViewLambda = [&](const auto* res, const std::string& name, ResCaps caps, const std::wstring& capstr) {
				if ((res->caps & caps) == ResCaps::none) {
					auto wname = L(name);
					ThrowIfFailed(E_INVALIDARG, std::format(L"{} doesn't have {} view, please add {} to {}'s view", wname, capstr, capstr, wname), __FILEW__, __LINE__);
				}
			};

			//UAV・RTV・DSVに入っているテクスチャでないなら、SRVに入れる
			//注意：当然だけどレジスタ割り当て時と同じロジックで列挙して追加する処理をしないといけない(レジスタ番号とリソース本体の対応付けがずれる)
			auto texPushLambda = [&](const auto& tex, auto* ptex) {
				bool found = false;
				for (auto&& u : item.UAV) {
					if (tex.name == u.name) {
						//viewにUAVが入ってるかチェック
						checkViewLambda(ptex, tex.name, ResCaps::uav, L"UAV");
						pass.UAV.push_back({ ptex, u.mipSlice });
						found = true;
					}
				}
				//テクスチャの番号順にRTVに入れると、MRTの順番が崩れるので、RTVへの実際の追加は後でやる
				for (auto&& r : item.RTV)
					if (tex.name == r.name)
						found = true;

				if (tex.name == item.DSV.name) {
					auto d = item.DSV;
					CV cv;
					if (d.clear) {
						DXGI_FORMAT fmt = ptex->desc().Format;
						bool hasStencil = (fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || fmt == DXGI_FORMAT_D24_UNORM_S8_UINT);
						if (hasStencil)
							cv = CV(d.depth, d.stencil);
						else
							cv = CV(d.depth);
					}
					//viewにDSVが入ってるかチェック
					checkViewLambda(ptex, tex.name, ResCaps::dsv, L"DSV");
					pass.DSV.push_back({ dynamic_cast<Tex2D*>(ptex), cv, d.mipSlice });
					found = true;
				}

				if (!found) {
					pass.SRV[param.SRVspace].push_back(ptex);
					//共有参照である場合は共有タグを追加
					for (auto&& s : sharedRef) {
						if (s.name == tex.name) {
							FXShareTag tag;
							tag.name = tex.name;
							tag.reg = pass.SRV[param.SRVspace].size() - 1;
							tag.space = param.SRVspace;
							shareTags[iPass].push_back(tag);
						}
					}
				}
			};

			//RTVへの追加
			for (auto&& r : item.RTV) {
				Res* ptex;
				//名前が空文字の時はデフォルトのRTを入れる
				if (r.name.empty()) {
					ptex = param.defaultRT.tex;
				} else {
					ptex = findTexNDLambda(r.name);
				}
				CV cv;
				if (r.clear)
					cv = CV(r.value);
				//viewにRTVが入ってるかチェック
				checkViewLambda(ptex, r.name, ResCaps::rtv, L"RTV");
				pass.RTV.push_back({ ptex, cv, r.mipSlice });
			}

			for (int i = 0; auto && tex : fx.textures) {
				Tex2D* ptex = &textures[texturemap[tex.name]];	//レジスタ割り当て時に判定してあるので必ずtexturemapにはtex.nameが入っている
				texPushLambda(tex, ptex);
				i++;
			}

			//3Dテクスチャ
			for (int i = 0; auto && tex : fx.textures3D) {
				Tex3D* ptex = &textures3D[texture3Dmap[tex.name]];
				texPushLambda(tex, ptex);
				i++;
			}

			//同様に、バッファ
			for (int i = 0; auto && buf : fx.buffers) {
				bool found = false;
				auto idx = bufmap[buf.name];
				for (auto&& u : item.UAV) {
					if (buf.name == u.name) {
						checkViewLambda(&buffers[idx], buf.name, ResCaps::uav, L"UAV");
						pass.UAV.push_back({ &buffers[idx] });
						found = true;
					}
				}
				if (!found) {
					pass.SRV[param.SRVspace].push_back(&buffers[idx]);
					//共有参照である場合は共有タグを追加
					for (auto&& s : sharedRef) {
						if (s.name == buf.name) {
							FXShareTag tag;
							tag.name = buf.name;
							tag.reg = pass.SRV[param.SRVspace].size() - 1;
							tag.space = param.SRVspace;
							shareTags[iPass].push_back(tag);
						}
					}
				}

				i++;
			}

			//RTV,DSVが空の場合はデフォルトの物を入れる
			if (pass.RTV.empty() && param.defaultRT.tex != nullptr) {
				pass.RTV.push_back(param.defaultRT);
			}
			if (pass.DSV.empty() && param.defaultZBuf.tex != nullptr) {
				pass.DSV.push_back(param.defaultZBuf);
			}

			//材質注釈用のテクスチャをSRVに入れる
			if (hasMatDesc()) {
				pass.SRV[usedConfig.SRVspace].push_back(&table_idx);
				pass.SRV[usedConfig.SRVspace].push_back(&table_tex);
				pass.SRV[usedConfig.SRVspace].push_back(&table_tex3D);
				pass.SRV[usedConfig.SRVspace].push_back(&matDescValues);

				matDescTexReg[iPass] = pass.SRV[usedConfig.SRVspace].size();
				SetupMatDescSRV(iPass);
			}


			//ラスタライザパスの場合、SRVにラスタライズ対象VB,IBが入っててはならないので
			//入ってたらダミーに挿げ替える。レジスタ番号を変えないために削除ではなくダミーに置き換え
			auto replaceRasterSRVLambda = [&](const std::vector<Buf*>& vb, const std::vector<Buf*>& ib) {
				for (int i = pass.SRV.size() - 1; i >= 0; i--) {
					for (int j = pass.SRV[i].size() - 1; j >= 0; j--) {
						bool removed = false;
						for (auto&& e : vb) {
							if (pass.SRV[i][j].res->res == e->res) {
								pass.SRV[i][j] = &m_dmyBuf;
								removed = true;
								break;
							}
						}
						if (!removed) {
							for (auto&& e : ib) {
								if (pass.SRV[i][j].res->res == e->res) {
									pass.SRV[i][j] = &m_dmyBuf;
									break;
								}
							}
						}
					}
				}
			};

			//outputSizeの決定
			auto os = ResolveOutputSize(item);

			//コンピュートパスの場合はnumthreadsの計算
			XMUINT3 numthreads = {};
			if (item.type == "compute") {
				numthreads = CalcNumthreads(item.numthreads, os.dimension);
				if (numthreads.x * numthreads.y * numthreads.z > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP) {
					ThrowIfFailed(E_INVALIDARG,
						std::format(L"{} : The product of each element of numthreads must be no greater than {}. ({}x{}x{} specified)",
							pass.name, D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, numthreads.x, numthreads.y, numthreads.z),
						__FILEW__, __LINE__
					);
				}
				//コンピュートパスの場合はnumthreadsでoutputSizeを割って端数切り上げした値を指定
				os.width = YRZ::CeilDiv((UINT)os.width, numthreads.x);
				os.height = YRZ::CeilDiv((UINT)os.height, numthreads.y);
				os.depth = YRZ::CeilDiv((UINT)os.depth, numthreads.z);
			}
			outputSize.push_back(os);

			//パスのタイプごとにシェーダをコンパイルしてセット
			if (updateShader) {
				//以下、シェーダの準備
				std::vector<std::wstring> option = param.compileOption;
				std::vector<std::wstring>macros;	//マクロ文字列保存用。コンパイラ通すまでに生きててもらわないと困るので
				for (auto&& m : item.macros) {
					option.push_back(L"-D");
					macros.push_back(L(m));
					option.push_back(macros.back().c_str());
				}
				//各パスごとのリソースバインドを参照できるようにするマクロ
				option.push_back(L"-D");
				std::wstring passMacro = L(std::format("YRZ_PASS_{}", item.name));
				option.push_back(passMacro.c_str());

				auto compileLambda = [&](auto& entry, auto& target) {
					auto shader = cache.Load(dxr, sourcestr.data(), sourcestr.size(), m_path.wstring(), filename, L(entry), L(target), option);
					//auto shader = dxr.CompileShaderMemory(sourcestr.data(), sourcestr.size(), nullptr, filename.c_str(), L(entry).c_str(), L(target).c_str(), option);
					return shader;
				};

				if (passType == FXPassType::raytracing) {
					auto lib = compileLambda(item.raygenShader, item.libTarget);
					std::wstring raygen = L(item.raygenShader);
					std::vector<std::wstring>miss;
					for (auto&& s : item.missShader)
						miss.emplace_back(L(s));
					std::vector<HitGroup>hg;
					for (auto& s : item.hitGroup) {
						HitGroup h;
						h.closestHit = L(s.closestHit);
						h.anyHit = L(s.anyHit);
						h.intersection = L(s.intersection);
						h.type = StringToD3D12HitGroupType(s.type);
						hg.emplace_back(h);
					}
					std::vector < std::wstring>callable;
					for (auto&& s : item.callableShader)
						callable.emplace_back(L(s));

					pass.RaytracingPass(lib, raygen, miss, hg, callable, item.maxPayloadSize, item.maxAttributeSize, item.maxRecursionDepth);

				} else if (passType == FXPassType::postprocess || passType == FXPassType::rasterizer) {
					auto vs = compileLambda(item.vertexShader, item.vsTarget);
					auto ps = compileLambda(item.pixelShader, item.psTarget);

					if (passType == FXPassType::postprocess)
						pass.PostProcessPass(vs, ps);
					else {
						//ラスタライザパス
						auto ied = FXInputElementDesc::Convert(param.layout);
						if (ramo == FXRasterModelTarget::buffer) {
							auto custom = FXInputElementDesc::Convert(customLayout[iPass]);
							if (passVB[iPass] == nullptr) {
								replaceRasterSRVLambda({}, { passIB[iPass] });
								pass.RasterizerPass(vs, ps, {}, { passIB[iPass] }, {},	//VBが無い場合はレイアウトは強制的に空を指定する
									item.blendDesc.Convert(), item.rasterizerDesc.Convert(), item.depthStencilDesc.Convert(),
									StringToD3D12PrimitiveTopologyType(item.primitiveTopologyType), item.sampleMask);
							} else {
								replaceRasterSRVLambda({ passVB[iPass] }, { passIB[iPass] });
								pass.RasterizerPass(vs, ps, { passVB[iPass] }, { passIB[iPass] }, custom,
									item.blendDesc.Convert(), item.rasterizerDesc.Convert(), item.depthStencilDesc.Convert(),
									StringToD3D12PrimitiveTopologyType(item.primitiveTopologyType), item.sampleMask);
							}
						} else {
							replaceRasterSRVLambda(param.rasterVB, param.rasterIB);
							pass.RasterizerPass(vs, ps, param.rasterVB, param.rasterIB, ied,
								item.blendDesc.Convert(), item.rasterizerDesc.Convert(), item.depthStencilDesc.Convert(),
								StringToD3D12PrimitiveTopologyType(item.primitiveTopologyType), item.sampleMask);
						}
					}
				} else if (passType == FXPassType::compute) {
					//numthreadsをオプションに追加
					option.push_back(L"-D");
					option.push_back(std::format(L"YRZ_NUMTHREADS=[numthreads({},{},{})]", numthreads.x, numthreads.y, numthreads.z));
					auto cs = compileLambda(item.computeShader, item.csTarget);
					pass.ComputePass(cs);
				} else {
					ThrowIfFailed(E_INVALIDARG, L"invalid pass type", __FILEW__, __LINE__);
				}
			} else {
				//ラスタライザの場合VBとIBだけ差し替え
				if (passType == FXPassType::rasterizer) {
					if (ramo == FXRasterModelTarget::buffer) {
						if (passVB[iPass]) {
							pass.SetVBIB({ passVB[iPass] }, { passIB[iPass] });
							replaceRasterSRVLambda({ passVB[iPass] }, { passIB[iPass] });
						} else {
							pass.SetVBIB({}, { passIB[iPass] });
							replaceRasterSRVLambda({}, { passIB[iPass] });
						}
					} else {
						pass.SetVBIB(param.rasterVB, param.rasterIB);
						replaceRasterSRVLambda(param.rasterVB, param.rasterIB);
					}
				}
				pass.Update();
			}
			
			iPass++;
		}


		/* 共有元の場合は共有先も必要に応じて更新 */
		if (!sharedSrc.empty() && watcher && updateSharedRef) {
			watcher->ResolveSharedResources();
		}

	}

	void FX::UpdateDefinedPass()
	{
		usedConfig.mdb.Backup(matDescs);
		Load(*m_dxr, m_filename, *m_cache, m_watcher, usedConfig, false, true, false);
	}

	std::string FX::ReadDescFile(const std::string& filename) {
		//ファイル名が空だった場合は空の材質注釈が返る(テンプレ通り、という事)
		if (filename.empty()) {
			return "";
		}
		std::string descstr;
		std::string descfile = filename.empty() ? matDescs.defaultFile : filename;
		std::ifstream ifs(m_path / L(descfile), std::ios::binary);
		if (!ifs) {
			ThrowIfFailed(E_FAIL, std::format(L"can't open meterial desc {}", L(filename)), __FILEW__, __LINE__);
			return "";
		}
		std::stringstream ss;
		ss << ifs.rdbuf();
		descstr = ss.str();
		ifs.close();
		return descstr;
	}


	//材質注釈バッファの再構築と全材質注釈の読み込み
	std::vector<std::tuple<int, int>> FX::SetMatDescAll(const std::vector<std::vector<std::string>>& desc)
	{
		std::vector<std::tuple<int, int>> errmats;	//エラーで読み込めなかった材質注釈が合った場合その番号(モデル番号,材質番号)を入れる

		//材質注釈のいらないエフェクトなら帰る
		if (!hasMatDesc()) {
			if (!desc.empty()) {
				ThrowIfFailed(E_INVALIDARG, L"don't need MatDesc, but desc is not empty", __FILEW__, __LINE__);
			}
			return errmats;
		}

		//指定されている材質注釈情報が現在のモデルなどの状態と一致しているのか検証
		//モデル数と材質数が一致するかチェック
		UINT nModel = usedConfig.materialCount.size();
		if (nModel != desc.size()) {
			ThrowIfFailed(E_INVALIDARG, L"number of config.materialCount and desc.size() not match", __FILEW__, __LINE__);
		}
		//各モデルの材質数が一致するかチェック
		for (int i = 0; auto && dd : desc) {
			if (usedConfig.materialCount[i] != dd.size()) {
				ThrowIfFailed(E_INVALIDARG,
					std::format(L"number of config.materialCount[{}]={} and desc[{}].size()={} not match", i, usedConfig.materialCount[i], i, dd.size()),
					__FILEW__, __LINE__
				);
			}
			i++;
		}

		matDescs.items.clear();
		std::wstring namew = L(matDescs.name);

		//name_idxバッファ モデルID→先頭材質番号
		matDescs.items.resize(nModel);
		std::vector<UINT> name_idx(nModel);
		UINT nMat = 0;	//全モデルの総材質数
		for (UINT i = 0; auto && idx : name_idx) {
			idx = nMat;
			nMat += desc[i].size();
			matDescs.items[i].resize(desc[i].size());
			i++;
		}
		table_idx = m_dxr->CreateBuf(name_idx.data(), sizeof(UINT), max(1, name_idx.size()));
		table_idx.SetName((namew + L"_idx").c_str());

		//name_vidx/_tidx/_tflgバッファ
		std::vector<float> name_value;
		UINT vcount = 0, idx = 0, tcount = 0, tcount3D = 0;
		for (UINT i = 0; i < nModel; i++) {
			nMat = desc[i].size();
			for (UINT j = 0; j < nMat; j++) {
				//材質注釈ファイルを読み込み、matDescStorage.itemsにセットする
				auto& item = matDescs.items[i][j];
				std::string descFileName = desc[i][j];

				auto parseLambda = [&]() {
					std::string descstr = ReadDescFile(descFileName);
					item = parser.Parse(descstr, i);
					item.sourceFile = descFileName;
				};

				//設定されたdescFileを読み取ろうとして失敗したらデフォルトファイルを読み込む
				try {
					parseLambda();
				} catch (...) {
					errmats.push_back({ i,j });
					descFileName = matDescs.defaultFile;
					parseLambda();
				}


				//テクスチャ読み込みの基準になるパス=matdescの格納されているパス
				fs::path texpath = (fs::path(m_path) / fs::path((char8_t*)item.sourceFile.c_str())).parent_path().lexically_normal();	

				vcount += parser.nVal;
				name_value.insert(name_value.end(), item.values.begin(), item.values.end());
				tcount += parser.nTex;
				
				auto textureLambda = [&]<class T>(T& dmy, auto nTex, auto& texturemap, auto& itemtextures, auto& parsermip, auto& textures, auto restype, auto& matDescTextures, auto& cache) {
					for (int k = 0; k < nTex; k++) {
						T tex;
						if (!itemtextures[k].empty()) {
							//定義されているリソースに名前が無いか探す
							if (texturemap.contains(itemtextures[k])) {
								tex = textures[texturemap[itemtextures[k]]];
							} else if (usedConfig.reservedMDRes.contains(itemtextures[k])) {
								//予約リソースか？
								Res* r = usedConfig.reservedMDRes[itemtextures[k]];
								tex = (r && r->type == dmy.type) ? *dynamic_cast<T*>(r) : dmy;	//問題点：予約リソースはコピーされるので予約リソースが本体側で再作成されてもそれに追従しない
							} else {
								//共有参照か？
								auto p = FindSharedSource(itemtextures[k]);
								if (p && p->type == restype) {
									tex = *dynamic_cast<T*>(p);
								} else if (itemtextures[k].find('.') != std::string::npos) {
									//共有参照でもない場合かつファイル名っぽい場合はファイルから読み込む
									bool mip = parsermip.contains(k);
									bool ok;
									fs::path p = texpath / L(itemtextures[k]);
									tex = cache.Get(p, mip, ok);
									//SetMatDescAllではテクスチャが見つからなくても例外を投げない(レンダラーの読み込み自体に失敗してしまうので)
									if (!ok)
										errmats.push_back({ i,j });
								} else {
									//デフォーマ由来のリソースの可能性があるがそれはエフェクトが全部読み込み終わってから出ないと解決できないので後でUpdateSharedで行う
									//とりあえずここではダミーテクスチャを代入しておく
									tex = dmy;
									tex.SetName(dmy.Name().c_str());
								}
							}
						} else {
							//テクスチャが空文字の場合、空のテクスチャをセット
							tex = dmy;
							tex.SetName(dmy.Name().c_str());
						}
						matDescTextures.push_back(tex);
					}
				};

				textureLambda(m_dmyTex, parser.nTex, texturemap, item.textures, parser.mip, textures, ResType::tex2D, matDescTextures, m_texCache);
				textureLambda(m_dmyTex3D, parser.nTex3D, texture3Dmap, item.textures3D, parser.mip3D, textures3D, ResType::tex3D, matDescTextures3D, m_texCache3D);

				idx++;
			}
		}

		matDescValues = m_dxr->CreateBuf(name_value.data(), sizeof(float)*parser.nVal, max(1, name_value.size()/parser.nVal));
		matDescValues.SetName((namew + L"_value").c_str());

		matDescValuesCPU = m_dxr->CreateBufCPU(name_value.data(), sizeof(float) * parser.nVal, max(1, name_value.size() / parser.nVal));
		matDescValuesCPU.SetName((namew + L"_valueCPU").c_str());
		matDescValuesCPU.res->Map(0, nullptr, (void**)&pMatDescValusCPU);

		//ユニークテクスチャ配列を作る所までやっとく
		MatDescUniqueTextures();

		return errmats;
	}


	void FX::SetMatDesc(const std::string& descfile, uint32_t iModel, uint32_t iMat, bool setValue, bool setTexture, bool passUpdate)
	{
		auto desc = ReadDescFile(descfile);
		auto parsed = parser.Parse(desc, iModel);

		//matDescの更新
		auto& item = matDescs.items[iModel][iMat];
		item.sourceFile = descfile;
		fs::path texpath = (m_path / fs::path((char8_t*)item.sourceFile.c_str())).parent_path().lexically_normal();	//テクスチャ読み込みの基準になるパス=matdescの格納されているパス

		//注目している材質は総材質番号でいうと何番？
		int idx = 0;
		for (uint32_t i = 0; i < iModel; i++)
			idx += matDescs.items[i].size();

		idx += iMat;

		//matDescValuesの更新
		if (setValue) {
			item.values = parsed.values;
			size_t size = sizeof(float) * parser.nVal;
			memcpy(pMatDescValusCPU + size * idx, item.values.data(), size);
			m_dxr->Upload(matDescValues, item.values.data(), size * idx, 0, size);
		}

		//matDescTextureの更新
		if (setTexture && parser.nTex!=0) {
			item.textures = parsed.textures;
			item.textures3D = parsed.textures3D;

			auto textureLambda = [&]<class T>(T& dmy, auto& texturemap, auto& textures, auto parser_nTex, auto& itemtextures, auto& parsermip, auto& matDescTextures, auto& m_texCache) {
				auto itex = idx * parser_nTex;
				for (int i = 0; i < parser_nTex; i++) {
					T tex;
					if (!itemtextures[i].empty()) {
						//予約語に含まれていないか？
						if (usedConfig.reservedMDRes.contains(itemtextures[i])) {
							Res* r = usedConfig.reservedMDRes[itemtextures[i]];
							tex = (r && dmy.type == r->type) ? *dynamic_cast<T*>(r) : dmy;
						} else if (texturemap.contains(itemtextures[i])) {
							//エフェクト内で定義・参照されているリソースではないか？
							tex = textures[texturemap[itemtextures[i]]];
						} else {
							//デフォーマを探してみる
							auto dfx = usedConfig.deformers[iModel];
							if (dfx && dfx->resMap.contains(itemtextures[i])) {
								Res* r = dfx->resMap[itemtextures[i]];
								tex = (r && r->type == dmy.type) ? *dynamic_cast<T*>(r) : dmy;
							} else {
								//共有参照を探してみる
								auto p = FindSharedSource(itemtextures[i]);
								if (p && p->type == dmy.type) {
									tex = *dynamic_cast<T*>(p);
								} else {
									//共有参照もない場合はファイルから読み込む
									bool ok;
									fs::path p = texpath / L(itemtextures[i]);
									tex = m_texCache.Get(p, parsermip.contains(i), ok);
									//ファイルが指定されるっぽくてファイルが無いならエラー出す
									if (!ok && !p.extension().empty()) {
										ThrowIfFailed(E_FAIL, L"texture " + p.wstring() + L" is not found", __FILEW__, __LINE__);
									}
								}
							}
						}
					} else {
						//テクスチャが空文字の場合、空のテクスチャをセット
						tex = dmy; //dxr.CreateTex2D(8, 8, dxr.BackBufferFormat());
						tex.SetName(m_dmyTex.Name().c_str());
					}
					matDescTextures[itex + i] = tex;
				}
			};

			//textureFlagsの更新
			textureLambda(m_dmyTex, texturemap, textures, parser.nTex, item.textures, parser.mip, matDescTextures, m_texCache);
			//m_dxr->Upload(table_tflg, &flag, sizeof(uint64_t) * idx, 0, sizeof(uint64_t));

			textureLambda(m_dmyTex3D, texture3Dmap, textures3D, parser.nTex3D, item.textures3D, parser.mip3D, matDescTextures3D, m_texCache3D);
			//m_dxr->Upload(table_tflg3D, &flag, sizeof(uint64_t) * idx, 0, sizeof(uint64_t));

			//パスからの参照を更新
			if (passUpdate) {
				Update();	//MatDescUniqueTextures()が呼ばれる
			}
		}

		//exprとcontrolersの更新
		item.exprs = parsed.exprs;
		item.controllers = std::move(parsed.controllers);
	}

	void FX::MatDescUniqueTextures()
	{
		if (!hasMatDesc())
			return;

		//matDescTexturesに格納されているユニークなテクスチャを見つける
		auto uniqLambda = [&]<class T>(T & dmy, auto& mdtexs, auto& unix) {
			unix.clear();
			std::vector<int>table(mdtexs.size());
			std::unordered_map<ID3D12Resource*, std::pair<int,T*>>dict;
			int found = 0;
			for (int i = 0; auto&& t : mdtexs) {
				if (t.res == dmy.res) {
					table[i] = -1;
				} else if (!dict.contains(t.res.Get())) {
					dict[t.res.Get()] = { found, &t };
					table[i] = found;
					unix.push_back(t);
					found++;
				} else {
					table[i] = dict[t.res.Get()].first;
				}
				i++;
			}
			auto buf = m_dxr->CreateBuf(nullptr, sizeof(int), max(1,table.size()));
			if (!table.empty())
				m_dxr->Upload(buf, table.data());
			return buf;
		};

		//テクスチャインデックスバッファの作成。全材質番号→ユニークテクスチャ番号の対応表
		table_tex = uniqLambda(m_dmyTex, matDescTextures, matDescUniqTex);
		table_tex3D = uniqLambda(m_dmyTex3D, matDescTextures3D, matDescUniqTex3D);


		//でば ... table_texとmatDescUniqTexの中身をダンプ
		/*
			std::wstring mdcon;
			int N = table_tex.desc().Width / 4;
			mdcon += std::format(L"table_idx : {} items\n", N);
			std::vector<int> table(N);
			m_dxr->Download(table.data(), table_tex);
			for (int i = 0; i < N; i++) {
				mdcon += std::to_wstring(table[i]) + L",";
			}
			mdcon += L"\n";

			int T = matDescUniqTex.size();
			for (int i = 0; i < T; i++) {
				mdcon += matDescUniqTex[i].Name() + L"\n";
			}

			std::wofstream ofs(L"uniqtexdebug.txt", std::ios::binary);
			if (!ofs)
				return;

			ofs << mdcon;
		*/
	}

	void FX::SetupMatDescSRV(int iPass)
	{
		if (!hasMatDesc())
			return;

		//SRVの挿げ替え、pass.updateは後続の処理に任せる
		int start = iPass == -1 ? 0 : iPass;
		int end = iPass == -1 ? passes.size() - 1 : iPass;

		for (int i = start; i <= end; i++) {
			//xの数に合わせた新しいチャンクサイズを計算するラムダ
			auto adjustLambda = [&](int cur, int x, int c) { return  max(cur, (max(x, 1) + c - 1) / c * c); };
			
			//チャンクサイズに合わせてSRVのサイズを変更する
			m_MDTexChunkSize = adjustLambda(m_MDTexChunkSize, matDescUniqTex.size(), matDescTexChunkSize);
			if (m_MDTexChunkSize + matDescTexReg[i] > passes[i].SRV[usedConfig.SRVspace].size())
				passes[i].SRV[usedConfig.SRVspace].resize(m_MDTexChunkSize + matDescTexReg[i]);
			
			m_MDTex3DChunkSize = adjustLambda(m_MDTex3DChunkSize, matDescUniqTex3D.size(), matDescTex3DChunkSize);
			if (m_MDTex3DChunkSize > passes[i].SRV[usedConfig.SRVspace3DTex].size())
				passes[i].SRV[usedConfig.SRVspace3DTex].resize(m_MDTex3DChunkSize);
			/*
			//チャンクサイズに合わせてSRVのサイズを変更する
			const int nTexChunk = 256;
			int nCurrentTexChunk = nTexChunk ? ((max(matDescUniqTex.size(), 1) + nTexChunk - 1) / nTexChunk) * nTexChunk : 0;
			if (nCurrentTexChunk > passes[i].SRV[usedConfig.SRVspace].size())
				passes[i].SRV[usedConfig.SRVspace].resize(nCurrentTexChunk + matDescTexReg[i]);

			const int nTexChunk3D = 256;
			int nCurrentTexChunk3D = nTexChunk3D ? ((max(matDescUniqTex3D.size(), 1) + nTexChunk3D - 1) / nTexChunk3D) * nTexChunk3D : 0;
			if (nCurrentTexChunk3D > passes[i].SRV[usedConfig.SRVspace3DTex].size())
				passes[i].SRV[usedConfig.SRVspace3DTex].resize(nCurrentTexChunk3D);
			*/

			//ユニークテクスチャに続いてチャンクサイズ分だけ埋める
			for (int j = 0; j < matDescUniqTex.size(); j++)
				passes[i].SRV[usedConfig.SRVspace][matDescTexReg[i] + j] = &matDescUniqTex[j];
			for (int j = matDescUniqTex.size(); j < m_MDTexChunkSize; j++)
				passes[i].SRV[usedConfig.SRVspace][matDescTexReg[i] + j] = &m_dmyTex;

			for (int j = 0; j < matDescUniqTex3D.size(); j++)
				passes[i].SRV[usedConfig.SRVspace3DTex][j] = &matDescUniqTex3D[j];
			for (int j = matDescUniqTex3D.size(); j < m_MDTex3DChunkSize; j++)
				passes[i].SRV[usedConfig.SRVspace3DTex][j] = &m_dmyTex3D;
		}
	}

	void FX::UpdateController()
	{
		for (auto&& ctrl : controllers) {
			auto* pCB = static_cast<char*>(controllerCB.pData);
			usedConfig.ControllerCallback(pCB + ctrl.offset, ctrl.iModel, ctrl.iItem, ctrl.type);
		}
	}

	void FX::ResolveMatDescController()
	{
		if (!hasMatDesc())
			return;

		int nValues = matDescValues.desc().Width / sizeof(float);	//材質注釈の値配列のサイズ
		int o = 0;
		int istart = nValues, iend = -1;	//コピー範囲計算用
		for (int i = 0; auto && mom : matDescs.items) {
			for (int j = 0; auto && md : mom) {

				for (auto&& c : md.controllers) {
					c.Update(*this);
				}

				if (!md.exprs.empty()) {
					for (auto&& e : md.exprs) {
						float v = e.e.Eval();
						bool ok = mdCompiler.error.empty();
						//TODO : これでint型が入っている場合に問題は無いのか？一応問題なく動いてそうではあるが…？
						if (ok && (md.values[e.ofs] != v)) {
							auto tp = parser.valTypes[e.ofs];	//型チェック
							if (tp == 0) {
								md.values[e.ofs] = v;	//float型ならそのまま
							} else if (tp == 1) {
								//int型
								int32_t iv = (int32_t)v;
								memcpy(&md.values[e.ofs], &iv, sizeof(int32_t));
							}
							istart = min(istart, (o + e.ofs));
							iend = max(iend, (o + e.ofs));
						}
					}
				}

				o += parser.nVal;
				j++;
			}
			i++;
		}

		if (iend >= 0) {
			std::vector<float> v(nValues);
			float* p = v.data();
			for (auto&& mom : matDescs.items) {
				for (auto&& md : mom) {
					memcpy(p, md.values.data(), sizeof(float) * parser.nVal);
					p += parser.nVal;
				}
			}

			size_t startByte = istart * sizeof(float);
			size_t transBytes = (iend - istart + 1) * sizeof(float);
			memcpy(pMatDescValusCPU + startByte, v.data() + istart, transBytes);
			m_dxr->CopyBufferRegion(matDescValues, startByte, matDescValuesCPU, startByte, transBytes);
			//m_dxr->Upload(matDescValues, v.data(), istart*sizeof(float), istart * sizeof(float), (iend - istart + 1) * sizeof(float));
		}
	}

	void FX::UpdateMatDescSharedTexture()
	{
		if (!hasMatDesc())
			return;
		
		bool update = false;
		int itex = 0;
		int itex3D = 0;
		for (int imo = 0;  auto&& mom : matDescs.items) {
			for (auto&& md : mom) {

				auto lambda = [&]<class T>(T& dmy, auto& mdtextures, auto& texturemap, auto& matDescTextures, auto& itex) {
					for (auto&& tex : mdtextures) {
						if (!tex.empty()) {
							fs::path p = (char8_t*)tex.c_str();
							if (p.extension().empty()) {
								//texはファイル名ではない
								if (!texturemap.contains(tex)) {
									//このエフェクトのリソースに含まれる名前でもない
									Res* r = nullptr;
									auto dfx = usedConfig.deformers[imo];
									if (dfx && dfx->resMap.contains(tex)) {
										//このモデルを処理するデフォーマに含まれるリソース？
										r = dfx->resMap[tex];
									} else {
										//それ以外→共有テクスチャ？
										r = FindSharedSource(tex);
									}
									if (r && (r->type == dmy.type)) {
										matDescTextures[itex] = *dynamic_cast<T*>(r);
										update = true;
									}
								}
							}
						}
						itex++;
					}
				};

				lambda(m_dmyTex, md.textures, texturemap, matDescTextures, itex);
				lambda(m_dmyTex3D, md.textures3D, texture3Dmap, matDescTextures3D, itex3D);
			}
			imo++;
		}

		if (update)
			Update();
	}

	//hlslファイルとFX構造体を単純にくっつけてファイルに保存
	void FX::SaveCombined(const std::wstring& fxfile, const std::wstring& hlsl, const FXTech& fx)
	{
		std::ifstream ifs(hlsl, std::ios::binary);
		if (!ifs) {
			ThrowIfFailed(E_FAIL, std::format(L"can't open file {}", hlsl), __FILEW__, __LINE__);
			return;
		}

		std::ofstream ofs(fxfile, std::ios::binary);
		if (!ofs) {
			ThrowIfFailed(E_FAIL, std::format(L"can't create file {}", fxfile), __FILEW__, __LINE__);
			return;
		}

		WriteHeader(ofs, "fx", fx);
		ofs << ifs.rdbuf();
		ofs.close();
		ifs.close();
	}

	Res* FX::FindSharedSource(std::string name)
	{
		if (m_watcher == nullptr) {
			return nullptr;
		}

		for (auto&& w : m_watcher->watchList) {
			if (w.fx) {
				for (auto&& src : w.fx->sharedSrc) {
					if (src.name == name) {
						if (src.type == ResType::buf) {
							return &w.fx->buffers[src.index];
						} else if (src.type == ResType::tex2D) {
							return &w.fx->textures[src.index];
						} else if (src.type == ResType::tex3D) {
							return &w.fx->textures3D[src.index];
						}
					}
				}
			}
		}
		return nullptr;
	}

	UINT FX::GetElemSize(const FXBuffer& buf)
	{
		//elemSizeが0でないならそのまま帰る
		if (buf.elemSize)
			return buf.elemSize;

		//問い合わせて有ったら終わり
		int size = usedConfig.ElemSizeQuery(buf.type);

		if (size)
			return size;

		//なかったら、組み込み型を探す
		int base = 0;
		std::string rest;

		auto typeLambda = [&](const std::string& t, int b) {
			if (base)
				return;
			auto idx = buf.type.find(t);

			//前方一致なので将来"inta"みたいな型が出来たらそれにマッチしてしまう事も有ろう
			if (idx == 0) {
				base = b;
				rest = buf.type.substr(t.size());
			}
			};

		//原始型のどれかを調べる
		typeLambda("bool", 4);
		typeLambda("int", 4);
		typeLambda("uint", 4);
		typeLambda("float", 4);
		typeLambda("double", 8);

		size = base;
		//数を調べる
		if (!rest.empty()) {
			auto idx = rest.find("x");
			if ((idx != std::string::npos) && (idx < rest.size() - 1)) {
				int l = std::atol(rest.data());
				int r = std::atol(rest.substr(idx + 1).data());
				size *= l * r;
			} else {
				int l = std::atol(rest.data());
				size *= l;
			}
		}

		if (size)
			return size;


		if (size == 0) {
			ThrowIfFailed(E_INVALIDARG, L"unknown elemSize : " + L(buf.type), __FILEW__, __LINE__);
		}
		return 0;
	}

	bool FXWatcher::Compile(FXWatchedItem& w, bool update)
	{
		bool restoreMD = (w.fx != nullptr);	//コンパイルエラーからの復帰や新規読み込みの場合、matDescを復旧しない
		try {
			if (w.fx) {
				//再コンパイル時の処理

				//共有元だった場合は共有源の参照を一時停止する
				if (!w.fx->sharedSrc.empty()) {
					std::unordered_set<std::string>share;
					for (auto& s : w.fx->sharedSrc)
						share.insert(s.name);
					StopSharedResources(share);
				}
				//材質注釈のバックアップ

				if (update) {
					w.config = w.fx->usedConfig;		//fx.usedConfigはモデルの増減に伴って更新される事があるのでupdateの場合は最新の値にする
					w.config.mdb.Backup(w.fx->matDescs);	//材質注釈データのバックアップ
				}
			}
			w.fx = std::make_unique<FX>();
			w.fx->Load(*dxr, w.path, *cache, this, w.config, true, update && restoreMD);
			return true;
		} catch (cereal::RapidJSONException ex) {
			auto err = FX::ReportJSONError(w.path);
			MessageBoxA(dxr->hWnd(), err.c_str(), "json error", MB_OK);
			w.fx = nullptr;
			w.config.mdb = {};	//エフェクトがコンパイルエラー起こしたときは材質注釈も無くなる(エフェクトが無い状態のままモデルの増減が行われると矛盾が出るため)
			return false;
		} catch (std::exception ex) {
			//コンパイルエラーの表示
			if (MessageBoxA(dxr->hWnd(), ex.what(), "compile error, YES->Copy hlsl to clipboard", MB_YESNO) == IDYES)
				CopyTextToClipboard(L(w.fx->compiledHLSL));
			w.fx = nullptr;
			w.config.mdb = {};	//エフェクトがコンパイルエラー起こしたときは材質注釈も無くなる(エフェクトが無い状態のままモデルの増減が行われると矛盾が出るため)
			return false;
		}
	}

	//監視を始めるファイルをパスで指定して、IDを返す
	//監視開始の際に読み込みを行い、fxオブジェクトを作成する(Get()で取得できる)
	void FXWatcher::StartWatch(int id, FXCategory cat, const fs::path& path, const FXLoadConfig& config, bool delayedLoading) {
		if (fs::exists(path)) {
			watchList.push_back({ id, cat, fs::last_write_time(path), nullptr, path, config });
			if (!delayedLoading)
				Compile(watchList.back(), false);
		} else {
			ThrowIfFailed(E_FAIL, std::format(L"{} not found", path.wstring()), __FILEW__, __LINE__);
		}
	}

	void FXWatcher::DelayedLoad(int id)
	{
		auto w = Find(id);
		if (w)
			Compile(*w, false);
	}


	//監視対象から外す。無効なidが指定されても例外は起こさない
	void FXWatcher::EndWatch(int id) {
		int n = watchList.size();
		for (int i = n - 1; i >= 0; i--) {
			if (watchList[i].id == id) {
				watchList.erase(watchList.begin() + i);
			}
		}
	}

	//pollInterval[ms]間隔でファイルの更新をチェックし、更新されていたら読み込みなおす
	bool FXWatcher::Poll() {
		bool updated = false;
		DWORD tm = timeGetTime();
		if (lastPolled + pollInterval >= tm) {
			//前回のポーリングより指定の時間間隔未満だったら帰る
			return false;
		}
		for (auto&& w : watchList) {
			//ソースが更新されてたらコンパイルする
			if (fs::exists(w.path)) {
				//削除されている可能性もあるので。その場合はファイルが復活し次第コンパイルを試みる
				auto ftime = fs::last_write_time(w.path);
				if (ftime > w.time) {
					Compile(w, true);
					w.time = ftime;
					updated = true;
				}
			}
		}

		lastPolled = tm;
		return updated;
	}

	//definedPassの更新が起こったので各エフェクトをリロードする
	void FXWatcher::UpdateDefinedPass()
	{
		auto updateLambda = [&](FXWatchedItem& w) {
			if (w.fx) {
				try {
					w.fx->UpdateDefinedPass();
				} catch (std::exception ex) {
					MessageBoxA(dxr->hWnd(), ex.what(), "update error", MB_OK);
					w.fx = nullptr;
				}
			}
		};

		for (int i = 0; auto && w : watchList)
			updateLambda(w);

		//共有参照を更新する
		ResolveSharedResources();
	}

	//共有リソースの解決
	void FXWatcher::ResolveSharedResources() {
		for (auto&& w : watchList) {
			//共有先エフェクトの各パスを走査
			if (w.fx && !w.fx->sharedRef.empty()) {
				for (int i = 0; auto && pass:w.fx->passes) {
					//パスで使っている共有参照があれば、共有源を検索する
					bool found = false;
					for (auto&& tag : w.fx->shareTags[i]) {
						Res* p = w.fx->FindSharedSource(tag.name);
						if (p) {
							//共有源が見つかればポインタを挿げ替え、パスのアップデートをする
							pass.SRV[tag.space][tag.reg] = p;
							found = true;
						}
					}
					if (found)
						pass.Update();
					i++;
				}
			}
		}
	}

	//namesに含まれる共有リソースを一時停止してダミーに戻す
	void FXWatcher::StopSharedResources(const std::unordered_set<std::string>& names)
	{
		std::unordered_set<FX*>fxs;

		//ウォッチリストに含まれる共有先エフェクトを検索
		for (auto&& w : watchList) {
			if (w.fx) {
				//共有参照を持ってる？
				if (!w.fx->sharedRef.empty()) {
					for (auto&& res : w.fx->sharedRef) {
						if (names.contains(res.name)) {
							fxs.insert(w.fx.get());
						}
					}
				}
				//MatDeskの中に共有参照が無いか…は分からない、でもシェアポだから落ちはしない、多分
			}
		}

		for (auto fx : fxs) {
			//各パスの共有参照をdmyに差し替え
			for (int i = 0; auto && pass:fx->passes) {
				bool found = false;
				for (auto&& tag : fx->shareTags[i]) {
					if (names.contains(tag.name)) {
						auto it = std::find_if(fx->sharedRef.begin(), fx->sharedRef.end(), [tag](auto& x) { return x.name == tag.name; });
						if (it != fx->sharedRef.end()) {
							if (it->type == ResType::buf)
								pass.SRV[tag.space][tag.reg] = dynamic_cast<Res*>(&fx->DmyBuf());
							else if (it->type == ResType::tex2D)
								pass.SRV[tag.space][tag.reg] = dynamic_cast<Res*>(&fx->DmyTex());
							else if (it->type == ResType::tex3D)
								pass.SRV[tag.space][tag.reg] = dynamic_cast<Res*>(&fx->DmyTex3D());

							//pass.SRV[tag.space][tag.reg] = (Res*)((it->type ) ? (void*)&(fx->DmyBuf()) : (void*)&(fx->DmyTex()));
							found = true;
						}
					}
				}
				if (found)
					pass.Update();
				i++;
			}
		}
	}



}

