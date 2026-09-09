#pragma once

// Map Marker Framework interface definitions.


#include "Stubs.h"

#include "SKSE/Interfaces.h"

#include <string>

namespace RE
{
	enum class MARKER_TYPE;
}

namespace MMF
{
	struct MarkerInfo
	{
		[[nodiscard]] RE::MARKER_TYPE GetVanillaMarker() const;
		[[nodiscard]] bool HasExternalIcon() const;
		[[nodiscard]] std::string GetSourcePath() const;
		[[nodiscard]] std::string GetExportName() const;
		[[nodiscard]] std::string GetExportNameUndiscovered() const;
		[[nodiscard]] float GetIconScale() const;
		[[nodiscard]] bool IsHiddenFromHUD() const;
		detail::MARKER_INFO proxy_;
	};
	static_assert(sizeof(MarkerInfo) == 0x28);

	class MapMarkerInterface
	{
	public:
		enum
		{
			kVersion = 1,
		};
		[[nodiscard]] std::uint32_t Version() const;
		[[nodiscard]] RE::MARKER_TYPE GetVanillaMarkerType(RE::TESObjectREFR* a_refr) const;
		[[nodiscard]] std::string GetCustomMarkerName(RE::TESObjectREFR* a_refr) const;
		[[nodiscard]] MarkerInfo GetMapMarkerInfo(RE::TESObjectREFR* a_refr) const;
		[[nodiscard]] MarkerInfo GetLocalMarkerInfo(RE::BGSLocation* a_location) const;
	protected:
		[[nodiscard]] const detail::MapMarkerInterface* GetProxy() const;
	};

	void QueryMapMarkerInterface(
		const SKSE::MessagingInterface::Message* a_msg,
		MapMarkerInterface*& a_intfc);
}

#include "Interfaces.inl"
