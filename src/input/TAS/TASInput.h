#pragma once

#include "Common/precompiled.h"

namespace TasInput
{
	enum class MovieMode : uint32
	{
		Disabled = 0,
		Playback = 1,
		Record = 2,
	};

	enum class MovieRecordPolicy : uint32
	{
		ReadOnly = 0,
		ReadWrite = 1,
	};

	struct MovieSyncData
	{
		uint32 magic{};
		uint32 version{};
		uint64 movieHash{};
		uint64 frame{};
		uint32 rerecordCount{};
		uint32 signature{};
	};

	enum : uint32
	{
		kBtnA = 1u << 0,
		kBtnB = 1u << 1,
		kBtnX = 1u << 2,
		kBtnY = 1u << 3,
		kBtnL = 1u << 4,
		kBtnR = 1u << 5,
		kBtnZL = 1u << 6,
		kBtnZR = 1u << 7,
		kBtnPlus = 1u << 8,
		kBtnMinus = 1u << 9,
		kBtnUp = 1u << 10,
		kBtnDown = 1u << 11,
		kBtnLeft = 1u << 12,
		kBtnRight = 1u << 13,
		kBtnStickL = 1u << 14,
		kBtnStickR = 1u << 15,
		kBtnHome = 1u << 16,
	};

	struct ManualState
	{
		float lx{};
		float ly{};
		float rx{};
		float ry{};
		float zl{};
		float zr{};
		uint32 buttons{};
	};

	struct OverlayState
	{
		bool active{};
		bool manual{};
		bool playback{};
		bool frameAdvancePaused{};
		uint64 frame{};
		float lx{};
		float ly{};
		float rx{};
		float ry{};
		float zl{};
		float zr{};
		uint32 buttons{};
	};

	struct VPADMovieSample
	{
		float lx{};
		float ly{};
		float rx{};
		float ry{};
		float zl{};
		float zr{};
		uint32 buttons{};
		uint32 vpadHold{};
		bool hasVpadHold{};
	};

	// Reloads TAS playback configuration from wx config and parses the input file.
	// Returns false if parsing/loading failed while playback is enabled.
	bool ReloadFromConfig();
	bool IsDeterministicSchedulerEnabled();
	bool IsDeterministicTimeEnabled();
	void SetFrameAdvancePaused(bool paused);
	bool ToggleFrameAdvancePaused();
	bool IsFrameAdvancePaused();
	bool IsFrameAdvanceStepActive();
	void RequestFrameAdvanceStep(uint32 steps = 1);
	void RequestFrameAdvanceVisualRefresh(uint32 refreshCount = 1);
	uint32 GetPendingFrameAdvanceVisualRefreshPermits();
	void ClearFrameAdvancePending();
	void BlockInputUntilNextFrameAdvance();
	void ArmTimelineInputGuard(uint64 frame);
	bool ConsumeFrameAdvanceVisualRefreshPermit();
	void WaitForFrameAdvancePermit();
	void WaitForFrameAdvanceCpuPermit();
	void OnFramePresented(uint64 frame);
	void BeginVPADPoll(size_t playerIndex, uint64 frame);
	bool TryGetVPADPlaybackSample(size_t playerIndex, uint64 frame, VPADMovieSample& outSample);
	void RecordVPADSample(size_t playerIndex, uint64 frame, const VPADMovieSample& sample);

	MovieMode GetMovieMode();
	MovieRecordPolicy GetMovieRecordPolicy();
	bool IsMovieActive();
	bool IsMovieDesynced();
	bool IsStrictTasModeEnabled();
	void CaptureMovieSync(MovieSyncData& outSync, bool& hasSync);
	bool ValidateMovieSync(const MovieSyncData& sync, bool hasSync, std::string& outError);
	void OnTimelineLoaded(uint64 restoredFrame, bool hasSync, const MovieSyncData& sync);
	bool SerializeMovieBlob(std::vector<uint8>& outBlob);
	bool DeserializeMovieBlob(const uint8* data, size_t size, std::string& outError);
	bool ExportMovieToFile(const fs::path& path, std::string& outError);
	bool ReadMovieTitleIdFromFile(const fs::path& path, uint64& outTitleId, std::string& outError);
	bool ImportMovieForPlaybackFromFile(const fs::path& path, std::string& outError, bool anchorToFirstPlaybackFrame = false, uint64 playbackStartMovieFrame = 0);
	bool EnsureMovieRecordTimeline(const fs::path& path, std::string& outError);

	void SetManualInputEnabled(bool enabled);
	bool IsManualInputEnabled();
	void SetControllerInputPassthroughEnabled(bool enabled);
	bool IsControllerInputPassthroughEnabled();
	ManualState GetManualInputState(size_t playerIndex);
	void SetManualInputState(size_t playerIndex, const ManualState& state);
	void SetManualTurboMask(size_t playerIndex, uint32 turboMask);
	uint32 GetManualTurboMask(size_t playerIndex);
	void SetManualTurboInterval(size_t playerIndex, uint32 intervalFrames);
	uint32 GetManualTurboInterval(size_t playerIndex);
	bool HasOverlayData();
	OverlayState GetOverlayState(uint64 frame, size_t playerIndex = 0);

	// For VPAD mappings: returns true when TAS playback is active and this mapping has an override.
	// outValue range: buttons [0,1], sticks [-1,1], triggers [0,1].
	bool QueryVPADMappingValue(size_t playerIndex, uint64 frame, uint64 mapping, float& outValue);
}

