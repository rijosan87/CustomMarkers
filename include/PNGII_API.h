#pragma once

#include <cstdint>

namespace PNGII::API
{
	inline constexpr std::uint32_t kVersion1 = 1;
	inline constexpr std::uint32_t kVersion2 = 2;
	inline constexpr char kExportName[] = "PNGII_RequestAPI";

	enum class Result : std::uint32_t
	{
		kSuccess = 0,
		kNoMatch = 1,
		kNotReady = 2,
		kBufferTooSmall = 3,
		kInvalidArgument = 4,
		kError = 5
	};

	using ResolveIconFunction = Result (*)(std::uint32_t formID, const char* iconLabel,
		std::int32_t soulStatus, char* path, std::uint32_t capacity,
		std::uint32_t* requiredSize) noexcept;

	struct InterfaceV1
	{
		std::uint32_t version;
		std::uint32_t structSize;
		ResolveIconFunction resolveIcon;
	};

	using GetIconLabelFunction = Result (*)(std::uint32_t formID, std::int32_t soulStatus,
		char* label, std::uint32_t capacity, std::uint32_t* requiredSize) noexcept;

	struct MapMarkerQuery
	{
		std::uint32_t structSize;
		std::uint32_t formID;
		std::uint32_t locationFormID;
		const char* comapIcon;
		std::uint16_t markerType;
		std::uint8_t isLocal;
		std::uint8_t isDoor;
		std::uint8_t undiscovered;
	};

	struct MapMarkerIconInfo
	{
		std::uint32_t structSize;
		const char* path;
		const char* undiscoveredPath;
		const char* primaryComapIcon;
		std::uint16_t primaryMarkerType;
		std::uint8_t map;
	};

	using ResolveMapMarkerIconFunction = Result (*)(const MapMarkerQuery* query, char* path, std::uint32_t capacity,
		std::uint32_t* requiredSize) noexcept;
	using GetMapMarkerIconCountFunction = Result (*)(std::uint32_t* count) noexcept;
	using GetMapMarkerIconFunction = Result (*)(std::uint32_t index, MapMarkerIconInfo* info) noexcept;

	using ReadIconFileFunction = Result (*)(const char* path, void* buffer, std::uint32_t capacity,
		std::uint32_t* requiredSize) noexcept;

	struct InterfaceV2
	{
		std::uint32_t version;
		std::uint32_t structSize;
		ResolveIconFunction resolveIcon;
		GetIconLabelFunction getIconLabel;
		ResolveMapMarkerIconFunction resolveMapMarkerIcon;
		GetMapMarkerIconCountFunction getMapMarkerIconCount;
		GetMapMarkerIconFunction getMapMarkerIcon;
		ReadIconFileFunction readIconFile;
	};

	using RequestAPIFunction = const void* (*)(std::uint32_t version) noexcept;
}