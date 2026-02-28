#include "input/TAS/TASInput.h"

#include "gui/wxgui/wxCemuConfig.h"
#include "input/InputManager.h"
#include "input/emulated/VPADController.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/CafeSystem.h"
#include <boost/algorithm/string.hpp>
#include <condition_variable>

namespace
{
	enum TasButtonMask : uint32
	{
		kBtnA = TasInput::kBtnA,
		kBtnB = TasInput::kBtnB,
		kBtnX = TasInput::kBtnX,
		kBtnY = TasInput::kBtnY,
		kBtnL = TasInput::kBtnL,
		kBtnR = TasInput::kBtnR,
		kBtnZL = TasInput::kBtnZL,
		kBtnZR = TasInput::kBtnZR,
		kBtnPlus = TasInput::kBtnPlus,
		kBtnMinus = TasInput::kBtnMinus,
		kBtnUp = TasInput::kBtnUp,
		kBtnDown = TasInput::kBtnDown,
		kBtnLeft = TasInput::kBtnLeft,
		kBtnRight = TasInput::kBtnRight,
		kBtnStickL = TasInput::kBtnStickL,
		kBtnStickR = TasInput::kBtnStickR,
		kBtnHome = TasInput::kBtnHome,
	};

	struct TasFrameInput
	{
		uint64 frame{};
		float lx{};
		float ly{};
		float rx{};
		float ry{};
		float zl{};
		float zr{};
		uint32 buttons{};
		uint32 signature{};
		uint32 vpadHold{};
	};

	struct TasPlayerData
	{
		std::vector<TasFrameInput> frames;
		uint64 maxFrame{};
	};

	enum class MovieInputTiming : uint32
	{
		Frame = 0,
		Poll = 1,
	};

	std::mutex s_mutex;
	std::condition_variable s_frameAdvanceCv;
	bool s_enabled{};
	bool s_loop{};
	bool s_manualEnabled{true};
	bool s_controllerInputPassthroughEnabled{};
	bool s_strictTasMode{};
	bool s_deterministicScheduler{};
	bool s_deterministicTime{};
	bool s_frameAdvancePaused{};
	uint32 s_frameAdvanceSteps{};
	uint32 s_frameAdvanceVisualRefreshPermits{};
	bool s_frameAdvanceStepActive{};
	bool s_blockInputUntilNextFrameAdvance{};
	fs::path s_file;
	std::array<TasPlayerData, InputManager::kMaxVPADControllers> s_players;
	std::array<TasInput::ManualState, InputManager::kMaxVPADControllers> s_manualPlayers;
	std::array<uint32, InputManager::kMaxVPADControllers> s_manualTurboMasks{};
	std::array<uint32, InputManager::kMaxVPADControllers> s_manualTurboIntervals{};
	TasInput::MovieMode s_movieMode{TasInput::MovieMode::Disabled};
	TasInput::MovieRecordPolicy s_MovieRecordPolicy{TasInput::MovieRecordPolicy::ReadOnly};
	MovieInputTiming s_movieInputTiming{MovieInputTiming::Frame};
	uint64 s_movieHash{};
	uint64 s_movieTitleId{};
	uint32 s_movieRerecordCount{};
	bool s_movieDesynced{};
	bool s_movieDirty{};
	uint64 s_lastRecordedFrame{std::numeric_limits<uint64>::max()};
	uint64 s_lastMovieFlushFrame{std::numeric_limits<uint64>::max()};
	bool s_movieAnchorToFirstPlaybackFrame{};
	bool s_moviePlaybackFrameAnchorInitialized{};
	uint64 s_moviePlaybackFrameAnchor{};
	uint64 s_moviePlaybackStartMovieFrame{};
	bool s_playbackStartupSyncPending{};
	uint64 s_playbackStartupSyncBeginRuntimeFrame{std::numeric_limits<uint64>::max()};
	bool s_movieSignaturesTrusted{true};
	std::array<bool, InputManager::kMaxVPADControllers> s_pollPlaybackLatchedValid{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_pollPlaybackLatchedRuntimeFrame{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_pollPlaybackLatchedMovieFrame{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_pollPlaybackCursor{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_pollRecordCursor{};
	std::array<bool, InputManager::kMaxVPADControllers> s_playbackFrameAnchorInitialized{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_playbackRuntimeFrameAnchor{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_playbackMovieFrameAnchor{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_recordLastRuntimeFrame{};
	std::array<bool, InputManager::kMaxVPADControllers> s_passthroughLiveValid{};
	std::array<uint64, InputManager::kMaxVPADControllers> s_passthroughLiveFrame{};
	std::array<TasFrameInput, InputManager::kMaxVPADControllers> s_passthroughLiveInput{};
	bool s_playbackCursorRestoredFromBlob{};
	thread_local bool s_bypassTasQueryForLiveCapture = false;

	struct ScopedTasQueryBypass
	{
		const bool prev = s_bypassTasQueryForLiveCapture;
		ScopedTasQueryBypass()
		{
			s_bypassTasQueryForLiveCapture = true;
		}
		~ScopedTasQueryBypass()
		{
			s_bypassTasQueryForLiveCapture = prev;
		}
	};

	constexpr uint32 kMovieSignatureSalt = 0xC3D2F1A5u;
	constexpr uint32 kMovieSyncMagic = 0x4D53594Eu; // MSYN
	constexpr uint32 kMovieSyncVersion = 1;
	constexpr uint32 kMovieBlobMagic = 0x424D5443u; // CTMB
	constexpr uint32 kMovieBlobVersion = 3;
	const TasFrameInput* GetFrameFor(const TasPlayerData& player, uint64 frame, bool loop);

	std::string Trim(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && std::isspace((unsigned char)text[start]))
			++start;
		size_t end = text.size();
		while (end > start && std::isspace((unsigned char)text[end - 1]))
			--end;
		return text.substr(start, end - start);
	}

	std::vector<std::string> Split(const std::string& text, char separator)
	{
		std::vector<std::string> values;
		std::string current;
		for (char c : text)
		{
			if (c == separator)
			{
				values.emplace_back(current);
				current.clear();
			}
			else
			{
				current.push_back(c);
			}
		}
		values.emplace_back(current);
		return values;
	}

	bool ParseButtonToken(const std::string& token, uint32& maskOut)
	{
		const std::string upper = boost::to_upper_copy(Trim(token));
		if (upper.empty())
			return true;

		if (upper == "A") maskOut |= kBtnA;
		else if (upper == "B") maskOut |= kBtnB;
		else if (upper == "X") maskOut |= kBtnX;
		else if (upper == "Y") maskOut |= kBtnY;
		else if (upper == "L") maskOut |= kBtnL;
		else if (upper == "R") maskOut |= kBtnR;
		else if (upper == "ZL") maskOut |= kBtnZL;
		else if (upper == "ZR") maskOut |= kBtnZR;
		else if (upper == "PLUS" || upper == "START") maskOut |= kBtnPlus;
		else if (upper == "MINUS" || upper == "SELECT") maskOut |= kBtnMinus;
		else if (upper == "UP") maskOut |= kBtnUp;
		else if (upper == "DOWN") maskOut |= kBtnDown;
		else if (upper == "LEFT") maskOut |= kBtnLeft;
		else if (upper == "RIGHT") maskOut |= kBtnRight;
		else if (upper == "STICKL" || upper == "L3") maskOut |= kBtnStickL;
		else if (upper == "STICKR" || upper == "R3") maskOut |= kBtnStickR;
		else if (upper == "HOME") maskOut |= kBtnHome;
		else return false;
		return true;
	}

	bool ParseButtons(const std::string& text, uint32& maskOut)
	{
		maskOut = 0;
		std::string normalized = text;
		std::replace(normalized.begin(), normalized.end(), '+', '|');
		for (const auto& token : Split(normalized, '|'))
		{
			if (!ParseButtonToken(token, maskOut))
				return false;
		}
		return true;
	}

	float ClampStick(float value)
	{
		return std::clamp(value, -1.0f, 1.0f);
	}

	float ClampTrigger(float value)
	{
		return std::clamp(value, 0.0f, 1.0f);
	}

	bool ParseBoolToken(const std::string& token, bool& outValue)
	{
		const auto value = boost::to_lower_copy(Trim(token));
		if (value == "1" || value == "true" || value == "yes" || value == "on")
		{
			outValue = true;
			return true;
		}
		if (value == "0" || value == "false" || value == "no" || value == "off")
		{
			outValue = false;
			return true;
		}
		return false;
	}

	bool ParseUInt32AutoBase(const std::string& token, uint32& outValue)
	{
		const std::string trimmed = Trim(token);
		if (trimmed.empty())
			return false;
		char* end = nullptr;
		const unsigned long value = std::strtoul(trimmed.c_str(), &end, 0);
		if (end == nullptr || *end != '\0')
			return false;
		if (value > std::numeric_limits<uint32>::max())
			return false;
		outValue = static_cast<uint32>(value);
		return true;
	}

	bool ParseUInt64AutoBase(const std::string& token, uint64& outValue)
	{
		const std::string trimmed = Trim(token);
		if (trimmed.empty())
			return false;
		char* end = nullptr;
		const unsigned long long value = std::strtoull(trimmed.c_str(), &end, 0);
		if (end == nullptr || *end != '\0')
			return false;
		outValue = static_cast<uint64>(value);
		return true;
	}

	bool ParseMovieInputTiming(const std::string& token, MovieInputTiming& outValue)
	{
		const auto value = boost::to_lower_copy(Trim(token));
		if (value == "frame" || value == "0")
		{
			outValue = MovieInputTiming::Frame;
			return true;
		}
		if (value == "poll" || value == "1")
		{
			outValue = MovieInputTiming::Poll;
			return true;
		}
		return false;
	}

	uint32 Fnv1a32(const void* data, size_t size, uint32 seed = 2166136261u)
	{
		const auto* bytes = static_cast<const uint8*>(data);
		uint32 hash = seed;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 16777619u;
		}
		return hash;
	}

	uint32 ComputeRuntimeSignature(uint64 frame)
	{
		uint32 h = kMovieSignatureSalt;
		h = Fnv1a32(&frame, sizeof(frame), h);
		h = Fnv1a32(&LatteGPUState.frameCounter, sizeof(LatteGPUState.frameCounter), h);
		return h;
	}

	bool IsNeutralFrameInput(const TasFrameInput& in)
	{
		constexpr float eps = 0.0001f;
		return std::abs(in.lx) <= eps &&
			std::abs(in.ly) <= eps &&
			std::abs(in.rx) <= eps &&
			std::abs(in.ry) <= eps &&
			std::abs(in.zl) <= eps &&
			std::abs(in.zr) <= eps &&
			in.buttons == 0 &&
			in.vpadHold == 0;
	}

	bool EvaluateMovieSignaturesTrustedNoLock()
	{
		// Require at least two different real signatures.
		uint32 firstSignature = 0;
		bool haveFirstSignature = false;
		uint32 actionableSignedSamples = 0;
		for (const auto& player : s_players)
		{
			for (const auto& frame : player.frames)
			{
				if (frame.signature == 0 || IsNeutralFrameInput(frame))
					continue;
				++actionableSignedSamples;
				if (!haveFirstSignature)
				{
					firstSignature = frame.signature;
					haveFirstSignature = true;
				}
				else if (frame.signature != firstSignature)
				{
					return true;
				}
				if (actionableSignedSamples >= 64)
					break;
			}
			if (actionableSignedSamples >= 64)
				break;
		}
		return false;
	}

	bool TryAcquirePlaybackStartupSyncNoLock(size_t playerIndex, uint64 runtimeFrame)
	{
		(void)playerIndex;
		(void)runtimeFrame;
		// Deterministic playback now relies on frame anchors.
		if (s_playbackStartupSyncPending)
		{
			s_playbackStartupSyncPending = false;
			s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
		}
		return true;
	}

	bool ParseFrameInputColumns(size_t player, const std::array<std::string, 8>& columns, TasFrameInput& outInput)
	{
		if (player >= InputManager::kMaxVPADControllers)
			return false;

		outInput.frame = static_cast<uint64>(std::max<int64_t>(0, static_cast<int64_t>(atoll(columns[0].c_str()))));
		outInput.lx = ClampStick((float)atof(columns[1].c_str()));
		outInput.ly = ClampStick((float)atof(columns[2].c_str()));
		outInput.rx = ClampStick((float)atof(columns[3].c_str()));
		outInput.ry = ClampStick((float)atof(columns[4].c_str()));
		outInput.zl = ClampTrigger((float)atof(columns[5].c_str()));
		outInput.zr = ClampTrigger((float)atof(columns[6].c_str()));

		uint32 buttonsValue = 0;
		if (!ParseUInt32AutoBase(columns[7], buttonsValue))
		{
			if (!ParseButtons(columns[7], buttonsValue))
				return false;
		}
		outInput.buttons = buttonsValue;
		outInput.signature = 0;
		outInput.vpadHold = 0;
		return true;
	}

	void AppendFrameInput(size_t player, const TasFrameInput& input)
	{
		auto& out = s_players[player];
		out.maxFrame = std::max(out.maxFrame, input.frame);
		out.frames.emplace_back(input);
	}

	bool ParseLegacyCsvLine(const std::vector<std::string>& columns)
	{
		if (columns.size() != 8 && columns.size() != 9)
			return false;

		size_t player = 0;
		std::array<std::string, 8> data{};
		if (columns.size() == 9)
		{
			player = std::min<size_t>((size_t)std::max(0, atoi(columns[1].c_str())), InputManager::kMaxVPADControllers - 1);
			data = { columns[0], columns[2], columns[3], columns[4], columns[5], columns[6], columns[7], columns[8] };
		}
		else
		{
			data = { columns[0], columns[1], columns[2], columns[3], columns[4], columns[5], columns[6], columns[7] };
		}

		TasFrameInput input{};
		if (!ParseFrameInputColumns(player, data, input))
			return false;
		AppendFrameInput(player, input);
		return true;
	}

	bool ParseCustomMovieLine(const std::vector<std::string>& columns)
	{
		if (columns.empty())
			return true;

		const std::string tag = boost::to_upper_copy(columns[0]);
		if (tag == "CTM1")
			return true;

		if (tag == "M")
		{
			if (columns.size() < 3)
				return false;
			const auto key = boost::to_lower_copy(columns[1]);
			if (key == "loop")
			{
				bool v = s_loop;
				if (ParseBoolToken(columns[2], v))
					s_loop = v;
			}
			else if (key == "deterministic_scheduler")
			{
				bool v = s_deterministicScheduler;
				if (ParseBoolToken(columns[2], v))
					s_deterministicScheduler = v;
			}
			else if (key == "deterministic_time")
			{
				bool v = s_deterministicTime;
				if (ParseBoolToken(columns[2], v))
					s_deterministicTime = v;
			}
			else if (key == "movie_mode")
			{
				// Runtime movie mode is controlled by config/UI (record/playback buttons).
				// Do not let file metadata override current mode on reload.
			}
			else if (key == "movie_record_policy")
			{
				// Runtime Timeline movie mode is controlled by config/UI/hotkeys.
				// Keep metadata for export compatibility but ignore it on load.
			}
			else if (key == "input_timing")
			{
				MovieInputTiming timing = s_movieInputTiming;
				if (ParseMovieInputTiming(columns[2], timing))
				{
					s_movieInputTiming = timing;
					if (s_movieInputTiming == MovieInputTiming::Poll)
					{
						s_movieInputTiming = MovieInputTiming::Frame;
						cemuLog_log(LogType::Force, "TAS: coerced unsupported poll movie timing to frame timing (ctm metadata)");
					}
				}
			}
			else if (key == "rerecord_count")
			{
				uint32 v = s_movieRerecordCount;
				if (ParseUInt32AutoBase(columns[2], v))
					s_movieRerecordCount = v;
			}
			else if (key == "movie_hash")
			{
				uint64 v = s_movieHash;
				if (ParseUInt64AutoBase(columns[2], v))
					s_movieHash = v;
			}
			else if (key == "title_id")
			{
				uint64 v = s_movieTitleId;
				if (ParseUInt64AutoBase(columns[2], v))
					s_movieTitleId = v;
			}
			return true;
		}

		if (tag != "F" || columns.size() != 12)
			return false;

		size_t player = std::min<size_t>((size_t)std::max(0, atoi(columns[2].c_str())), InputManager::kMaxVPADControllers - 1);
		std::array<std::string, 8> data = { columns[1], columns[3], columns[4], columns[5], columns[6], columns[7], columns[8], columns[9] };
		TasFrameInput input{};
		if (!ParseFrameInputColumns(player, data, input))
			return false;
		uint32 signature = 0;
		uint32 vpadHold = 0;
		if (!ParseUInt32AutoBase(columns[10], signature) || !ParseUInt32AutoBase(columns[11], vpadHold))
			return false;
		input.signature = signature;
		input.vpadHold = vpadHold;
		AppendFrameInput(player, input);
		return true;
	}

	const TasFrameInput* GetFrameFor(const TasPlayerData& player, uint64 frame, bool loop)
	{
		if (player.frames.empty())
			return nullptr;

		uint64 queryFrame = frame;
		if (loop && player.maxFrame > 0)
			queryFrame = frame % (player.maxFrame + 1);

		auto it = std::upper_bound(player.frames.cbegin(), player.frames.cend(), queryFrame,
			[](uint64 f, const TasFrameInput& e) { return f < e.frame; });
		if (it == player.frames.cbegin())
			return nullptr;
		--it;
		return &(*it);
	}

	TasFrameInput EffectiveManualInputForFrame(size_t playerIndex, uint64 frame)
	{
		const auto& manual = s_manualPlayers[playerIndex];
		TasFrameInput in{};
		in.lx = ClampStick(manual.lx);
		in.ly = ClampStick(manual.ly);
		in.rx = ClampStick(manual.rx);
		in.ry = ClampStick(manual.ry);
		in.zl = ClampTrigger(manual.zl);
		in.zr = ClampTrigger(manual.zr);
		in.buttons = manual.buttons;
		const uint32 turboMask = s_manualTurboMasks[playerIndex];
		const uint32 turboInterval = std::max<uint32>(1, s_manualTurboIntervals[playerIndex]);
		if (turboMask != 0 && (((frame / turboInterval) & 1ull) != 0))
			in.buttons &= ~turboMask;
		in.frame = frame;
		in.signature = 0;
		return in;
	}

	TasFrameInput CaptureLiveVPADInputForFrame(size_t playerIndex, uint64 frame)
	{
		TasFrameInput in{};
		in.frame = frame;
		if (playerIndex >= InputManager::kMaxVPADControllers)
			return in;

		auto vpad = InputManager::instance().get_vpad_controller(playerIndex);
		if (!vpad)
			return in;

		ScopedTasQueryBypass bypass;
		vpad->InvalidateCachedReadState();
		vpad->controllers_update_states();

		const auto leftStick = vpad->get_axis();
		const auto rightStick = vpad->get_rotation();
		const auto triggers = vpad->get_trigger();
		in.lx = ClampStick(leftStick.x);
		in.ly = ClampStick(leftStick.y);
		in.rx = ClampStick(rightStick.x);
		in.ry = ClampStick(rightStick.y);
		in.zl = ClampTrigger(std::max(triggers.x, vpad->is_mapping_down(VPADController::kButtonId_ZL) ? 1.0f : 0.0f));
		in.zr = ClampTrigger(std::max(triggers.y, vpad->is_mapping_down(VPADController::kButtonId_ZR) ? 1.0f : 0.0f));

		auto setButton = [&in, &vpad](uint64 mapping, uint32 mask)
		{
			if (vpad->is_mapping_down(mapping))
				in.buttons |= mask;
		};

		setButton(VPADController::kButtonId_A, kBtnA);
		setButton(VPADController::kButtonId_B, kBtnB);
		setButton(VPADController::kButtonId_X, kBtnX);
		setButton(VPADController::kButtonId_Y, kBtnY);
		setButton(VPADController::kButtonId_L, kBtnL);
		setButton(VPADController::kButtonId_R, kBtnR);
		setButton(VPADController::kButtonId_ZL, kBtnZL);
		setButton(VPADController::kButtonId_ZR, kBtnZR);
		setButton(VPADController::kButtonId_Plus, kBtnPlus);
		setButton(VPADController::kButtonId_Minus, kBtnMinus);
		setButton(VPADController::kButtonId_Up, kBtnUp);
		setButton(VPADController::kButtonId_Down, kBtnDown);
		setButton(VPADController::kButtonId_Left, kBtnLeft);
		setButton(VPADController::kButtonId_Right, kBtnRight);
		setButton(VPADController::kButtonId_StickL, kBtnStickL);
		setButton(VPADController::kButtonId_StickR, kBtnStickR);
		setButton(VPADController::kButtonId_Home, kBtnHome);
		return in;
	}

	void UpsertFrameInput(size_t playerIndex, const TasFrameInput& input)
	{
		if (playerIndex >= s_players.size())
			return;
		auto& frames = s_players[playerIndex].frames;
		auto it = std::lower_bound(frames.begin(), frames.end(), input.frame, [](const TasFrameInput& lhs, uint64 frame) {
			return lhs.frame < frame;
		});
		if (it != frames.end() && it->frame == input.frame)
		{
			*it = input;
		}
		else
		{
			frames.insert(it, input);
		}
		s_players[playerIndex].maxFrame = std::max(s_players[playerIndex].maxFrame, input.frame);
	}

	void TruncateMovieAfterFrame(uint64 frame)
	{
		for (auto& player : s_players)
		{
			auto it = std::upper_bound(player.frames.begin(), player.frames.end(), frame, [](uint64 f, const TasFrameInput& rhs) {
				return f < rhs.frame;
			});
			player.frames.erase(it, player.frames.end());
			player.maxFrame = player.frames.empty() ? 0 : player.frames.back().frame;
		}
	}

	uint64 ComputeMovieHashNoLock()
	{
		uint64 hash = 1469598103934665603ull;
		const auto mix = [&hash](const void* ptr, size_t size)
		{
			const auto* bytes = static_cast<const uint8*>(ptr);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		mix(&titleId, sizeof(titleId));
		for (size_t i = 0; i < s_players.size(); ++i)
		{
			const uint64 playerIndex = i;
			mix(&playerIndex, sizeof(playerIndex));
			for (const auto& frame : s_players[i].frames)
				mix(&frame, sizeof(frame));
		}
		return hash;
	}

	bool FlushMovieToFileNoLock()
	{
		if (s_file.empty())
			return false;
		std::ofstream out(s_file, std::ios::trunc);
		if (!out.is_open())
			return false;
		auto WriteMovieTextNoLock = [](std::ostream& stream)
		{
			stream << "CTM1\n";
			stream << "M,loop," << (s_loop ? "1" : "0") << "\n";
			stream << "M,deterministic_scheduler," << (s_deterministicScheduler ? "1" : "0") << "\n";
			stream << "M,deterministic_time," << (s_deterministicTime ? "1" : "0") << "\n";
			stream << "M,movie_mode," << static_cast<uint32>(s_movieMode) << "\n";
			stream << "M,movie_record_policy," << static_cast<uint32>(s_MovieRecordPolicy) << "\n";
			stream << "M,input_timing," << ((s_movieInputTiming == MovieInputTiming::Poll) ? "poll" : "frame") << "\n";
			stream << "M,rerecord_count," << s_movieRerecordCount << "\n";
			stream << "M,movie_hash," << s_movieHash << "\n";
			const uint64 titleId = CafeSystem::GetForegroundTitleId();
			stream << "M,title_id," << (titleId != 0 ? titleId : s_movieTitleId) << "\n";

			for (size_t player = 0; player < s_players.size(); ++player)
			{
				for (const auto& frame : s_players[player].frames)
				{
					stream << "F," << frame.frame
						<< "," << player
						<< "," << frame.lx
						<< "," << frame.ly
						<< "," << frame.rx
						<< "," << frame.ry
						<< "," << frame.zl
						<< "," << frame.zr
						<< "," << frame.buttons
						<< "," << frame.signature
						<< "," << frame.vpadHold
						<< "\n";
				}
			}
		};
		WriteMovieTextNoLock(out);
		return out.good();
	}

	bool QueryFrameValue(const TasFrameInput& in, uint64 mapping, float& outValue)
	{
		const auto btn = [&](TasButtonMask m) { return (in.buttons & (uint32)m) != 0; };
		switch (mapping)
		{
		case VPADController::kButtonId_A: outValue = btn(kBtnA) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_B: outValue = btn(kBtnB) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_X: outValue = btn(kBtnX) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Y: outValue = btn(kBtnY) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_L: outValue = btn(kBtnL) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_R: outValue = btn(kBtnR) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_ZL: outValue = std::max(in.zl, btn(kBtnZL) ? 1.0f : 0.0f); return true;
		case VPADController::kButtonId_ZR: outValue = std::max(in.zr, btn(kBtnZR) ? 1.0f : 0.0f); return true;
		case VPADController::kButtonId_Plus: outValue = btn(kBtnPlus) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Minus: outValue = btn(kBtnMinus) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Up: outValue = btn(kBtnUp) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Down: outValue = btn(kBtnDown) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Left: outValue = btn(kBtnLeft) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Right: outValue = btn(kBtnRight) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_StickL: outValue = btn(kBtnStickL) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_StickR: outValue = btn(kBtnStickR) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_Home: outValue = btn(kBtnHome) ? 1.0f : 0.0f; return true;
		case VPADController::kButtonId_StickL_Left: outValue = std::max(0.0f, -in.lx); return true;
		case VPADController::kButtonId_StickL_Right: outValue = std::max(0.0f, in.lx); return true;
		case VPADController::kButtonId_StickL_Up: outValue = std::max(0.0f, in.ly); return true;
		case VPADController::kButtonId_StickL_Down: outValue = std::max(0.0f, -in.ly); return true;
		case VPADController::kButtonId_StickR_Left: outValue = std::max(0.0f, -in.rx); return true;
		case VPADController::kButtonId_StickR_Right: outValue = std::max(0.0f, in.rx); return true;
		case VPADController::kButtonId_StickR_Up: outValue = std::max(0.0f, in.ry); return true;
		case VPADController::kButtonId_StickR_Down: outValue = std::max(0.0f, -in.ry); return true;
		default:
			return false;
		}
	}

	TasInput::VPADMovieSample FrameInputToMovieSample(const TasFrameInput& in)
	{
		TasInput::VPADMovieSample out{};
		out.lx = in.lx;
		out.ly = in.ly;
		out.rx = in.rx;
		out.ry = in.ry;
		out.zl = in.zl;
		out.zr = in.zr;
		out.buttons = in.buttons;
		out.vpadHold = in.vpadHold;
		out.hasVpadHold = true;
		return out;
	}

	TasFrameInput MovieSampleToFrameInput(const TasInput::VPADMovieSample& in)
	{
		TasFrameInput out{};
		out.lx = ClampStick(in.lx);
		out.ly = ClampStick(in.ly);
		out.rx = ClampStick(in.rx);
		out.ry = ClampStick(in.ry);
		out.zl = ClampTrigger(in.zl);
		out.zr = ClampTrigger(in.zr);
		out.buttons = in.buttons;
		out.vpadHold = in.vpadHold;
		return out;
	}

	TasFrameInput ManualStateToFrameInput(const TasInput::ManualState& manual)
	{
		TasFrameInput in{};
		in.lx = ClampStick(manual.lx);
		in.ly = ClampStick(manual.ly);
		in.rx = ClampStick(manual.rx);
		in.ry = ClampStick(manual.ry);
		in.zl = ClampTrigger(manual.zl);
		in.zr = ClampTrigger(manual.zr);
		in.buttons = manual.buttons;
		in.vpadHold = 0;
		return in;
	}

	void UpdateManualStateFromFrameInputNoLock(size_t playerIndex, const TasFrameInput& input)
	{
		if (playerIndex >= s_manualPlayers.size())
			return;
		auto& manual = s_manualPlayers[playerIndex];
		manual.lx = ClampStick(input.lx);
		manual.ly = ClampStick(input.ly);
		manual.rx = ClampStick(input.rx);
		manual.ry = ClampStick(input.ry);
		manual.zl = ClampTrigger(input.zl);
		manual.zr = ClampTrigger(input.zr);
		manual.buttons = input.buttons;
	}

	uint32 ApplyTurboMask(uint32 buttons, uint32 turboMask, uint32 turboInterval, uint64 frame)
	{
		if (turboMask == 0 || turboInterval == 0)
			return buttons;

		const bool turboOnPhase = ((frame / turboInterval) & 1ull) == 0;
		if (turboOnPhase)
			return buttons;
		return buttons & ~turboMask;
	}

	void EnforceStrictTasPolicyNoLock()
	{
		if (!s_strictTasMode)
			return;
		s_deterministicScheduler = true;
		s_deterministicTime = true;
	}

	void ResetPollStateNoLock()
	{
		s_pollPlaybackLatchedValid.fill(false);
		s_pollPlaybackLatchedRuntimeFrame.fill(0);
		s_pollPlaybackLatchedMovieFrame.fill(0);
		s_pollPlaybackCursor.fill(0);
		s_pollRecordCursor.fill(0);
		s_playbackFrameAnchorInitialized.fill(false);
		s_playbackRuntimeFrameAnchor.fill(0);
		s_playbackMovieFrameAnchor.fill(0);
		s_recordLastRuntimeFrame.fill(std::numeric_limits<uint64>::max());
		s_passthroughLiveValid.fill(false);
		s_passthroughLiveFrame.fill(0);
	}

	void InitializeRecordPollCursorFromFramesNoLock()
	{
		for (size_t player = 0; player < s_players.size(); ++player)
		{
			const auto& frames = s_players[player].frames;
			s_pollRecordCursor[player] = frames.empty() ? 0 : (frames.back().frame + 1);
		}
	}

	uint64 ResolveMovieQueryFrameNoLock(uint64 runtimeFrame)
	{
		return runtimeFrame;
	}

	uint64 ResolvePlaybackMovieFrameNoLock(size_t playerIndex, uint64 runtimeFrame)
	{
		if (playerIndex >= s_players.size())
			return 0;
		if (s_movieMode != TasInput::MovieMode::Playback)
			return ResolveMovieQueryFrameNoLock(runtimeFrame);

		return s_pollPlaybackCursor[playerIndex];
	}
}

namespace TasInput
{
	bool ReloadFromConfig()
	{
		fs::path autoImportPath;
		bool shouldAutoImport = false;
		{
			std::scoped_lock lock(s_mutex);

			auto& cfg = GetWxGUIConfig().tas;
			s_enabled = true;
			s_loop = false;
			s_strictTasMode = cfg.strict_tas_mode;
			s_deterministicScheduler = cfg.deterministic_scheduler || s_strictTasMode;
			s_deterministicTime = cfg.deterministic_time || s_strictTasMode;
			// TAS input editor is always manual mode.
			s_manualEnabled = true;
			s_movieMode = (cfg.movie_mode == 2) ? MovieMode::Record : ((cfg.movie_mode == 1) ? MovieMode::Playback : MovieMode::Disabled);
			s_MovieRecordPolicy = (cfg.movie_record_policy == 1) ? MovieRecordPolicy::ReadWrite : MovieRecordPolicy::ReadOnly;
			s_movieDesynced = false;
			s_movieDirty = false;
			s_movieAnchorToFirstPlaybackFrame = false;
			s_moviePlaybackFrameAnchorInitialized = false;
			s_moviePlaybackFrameAnchor = 0;
			s_moviePlaybackStartMovieFrame = 0;
			s_playbackStartupSyncPending = false;
			s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
			s_movieSignaturesTrusted = true;
			ResetPollStateNoLock();
			s_playbackCursorRestoredFromBlob = false;
			s_lastMovieFlushFrame = std::numeric_limits<uint64>::max();

			if (s_movieMode == MovieMode::Disabled)
				s_movieHash = 0;
			else if (s_movieMode == MovieMode::Playback && !cfg.input_playback_file.empty())
			{
				const fs::path configuredMoviePath = fs::path(_utf8ToPath(cfg.input_playback_file));
				bool haveFrames = false;
				for (const auto& player : s_players)
				{
					if (!player.frames.empty())
					{
						haveFrames = true;
						break;
					}
				}
				if (configuredMoviePath != s_file || !haveFrames)
				{
					autoImportPath = configuredMoviePath;
					shouldAutoImport = !autoImportPath.empty();
				}
			}
		}

		if (shouldAutoImport)
		{
			std::string err;
			if (!ImportMovieForPlaybackFromFile(autoImportPath, err, true))
				cemuLog_log(LogType::Force, "TAS: auto-playback import failed path={} err={}", _pathToUtf8(autoImportPath), err);
		}

		return true;
	}

	bool IsDeterministicSchedulerEnabled()
	{
		std::scoped_lock lock(s_mutex);
		return s_deterministicScheduler || s_enabled || s_frameAdvancePaused || s_movieMode != MovieMode::Disabled;
	}

	bool IsDeterministicTimeEnabled()
	{
		std::scoped_lock lock(s_mutex);
		return s_deterministicTime || s_enabled || s_frameAdvancePaused || s_movieMode != MovieMode::Disabled;
	}

	void SetFrameAdvancePaused(bool paused)
	{
		{
			std::scoped_lock lock(s_mutex);
			s_frameAdvancePaused = paused;
			if (!paused)
			{
				s_frameAdvanceSteps = 0;
				s_frameAdvanceVisualRefreshPermits = 0;
				s_frameAdvanceStepActive = false;
				s_blockInputUntilNextFrameAdvance = false;
			}
		}
		s_frameAdvanceCv.notify_all();
	}

	bool ToggleFrameAdvancePaused()
	{
		bool newPaused = false;
		{
			std::scoped_lock lock(s_mutex);
			s_frameAdvancePaused = !s_frameAdvancePaused;
			if (!s_frameAdvancePaused)
			{
				s_frameAdvanceSteps = 0;
				s_frameAdvanceVisualRefreshPermits = 0;
				s_frameAdvanceStepActive = false;
				s_blockInputUntilNextFrameAdvance = false;
			}
			newPaused = s_frameAdvancePaused;
		}
		s_frameAdvanceCv.notify_all();
		return newPaused;
	}

	bool IsFrameAdvancePaused()
	{
		std::scoped_lock lock(s_mutex);
		return s_frameAdvancePaused;
	}

	bool IsFrameAdvanceStepActive()
	{
		std::scoped_lock lock(s_mutex);
		return s_frameAdvancePaused && s_frameAdvanceStepActive;
	}

	void RequestFrameAdvanceStep(uint32 steps)
	{
		if (steps == 0)
			return;
		{
			std::scoped_lock lock(s_mutex);
			if (!s_frameAdvancePaused)
				return;
			s_frameAdvanceStepActive = false;
			s_blockInputUntilNextFrameAdvance = false;
			// Clear stale refresh permits before stepping.
			s_frameAdvanceVisualRefreshPermits = 0;
			s_frameAdvanceSteps += steps;
		}
		for (size_t i = 0; i < InputManager::kMaxVPADControllers; ++i)
		{
			if (auto vpad = InputManager::instance().get_vpad_controller(i))
			{
				vpad->InvalidateCachedReadState();
				vpad->controllers_update_states();
			}
		}
		s_frameAdvanceCv.notify_all();
	}

	void RequestFrameAdvanceVisualRefresh(uint32 refreshCount)
	{
		if (refreshCount == 0)
			return;
		{
			std::scoped_lock lock(s_mutex);
			if (!s_frameAdvancePaused)
				return;
			const uint32 maxValue = std::numeric_limits<uint32>::max();
			if (s_frameAdvanceVisualRefreshPermits > maxValue - refreshCount)
				s_frameAdvanceVisualRefreshPermits = maxValue;
			else
				s_frameAdvanceVisualRefreshPermits += refreshCount;
		}
		s_frameAdvanceCv.notify_all();
	}

	uint32 GetPendingFrameAdvanceVisualRefreshPermits()
	{
		std::scoped_lock lock(s_mutex);
		if (!s_frameAdvancePaused)
			return 0;
		return s_frameAdvanceVisualRefreshPermits;
	}

	void ClearFrameAdvancePending()
	{
		{
			std::scoped_lock lock(s_mutex);
			if (!s_frameAdvancePaused)
				return;
			s_frameAdvanceSteps = 0;
			s_frameAdvanceVisualRefreshPermits = 0;
			s_frameAdvanceStepActive = false;
		}
		s_frameAdvanceCv.notify_all();
	}

	void BlockInputUntilNextFrameAdvance()
	{
	
	}

	void ArmTimelineInputGuard(uint64 frame)
	{
		(void)frame;
	}

	bool ConsumeFrameAdvanceVisualRefreshPermit()
	{
		std::scoped_lock lock(s_mutex);
		if (!s_frameAdvancePaused || s_frameAdvanceVisualRefreshPermits == 0)
			return false;
		--s_frameAdvanceVisualRefreshPermits;
		return true;
	}

	void WaitForFrameAdvancePermit()
	{
		std::unique_lock lock(s_mutex);
		if (!s_frameAdvancePaused || g_lattePauseRequested.load(std::memory_order_acquire))
			return;
		while (s_frameAdvancePaused && s_frameAdvanceSteps == 0 && s_frameAdvanceVisualRefreshPermits == 0 && !g_lattePauseRequested.load(std::memory_order_acquire))
		{
			s_frameAdvanceCv.wait_for(lock, std::chrono::milliseconds(1));
		}
		if (g_lattePauseRequested.load(std::memory_order_acquire))
			return;
		if (s_frameAdvancePaused && s_frameAdvanceVisualRefreshPermits > 0)
		{
			s_frameAdvanceStepActive = false;
			return;
		}
		if (s_frameAdvancePaused && s_frameAdvanceSteps > 0)
		{
			--s_frameAdvanceSteps;
			s_frameAdvanceStepActive = true;
		}
	}

	void WaitForFrameAdvanceCpuPermit()
	{
		std::unique_lock lock(s_mutex);
		if (!s_frameAdvancePaused || g_lattePauseRequested.load(std::memory_order_acquire))
			return;
		while (s_frameAdvancePaused && s_frameAdvanceSteps == 0 && !g_lattePauseRequested.load(std::memory_order_acquire))
		{
			s_frameAdvanceCv.wait_for(lock, std::chrono::milliseconds(1));
		}
	}

	void OnFramePresented(uint64 frame)
	{
		{
			std::scoped_lock lock(s_mutex);
			if (s_frameAdvancePaused)
				s_frameAdvanceStepActive = false;
			if (s_movieMode == MovieMode::Disabled || s_file.empty())
				return;
			if (s_movieMode == MovieMode::Playback)
				return;

			if (s_movieMode != MovieMode::Record)
				return;
			if (!s_movieDirty)
				return;
			if (s_lastMovieFlushFrame == std::numeric_limits<uint64>::max() || (frame - s_lastMovieFlushFrame) >= 30)
			{
				if (FlushMovieToFileNoLock())
				{
					s_movieDirty = false;
					s_lastMovieFlushFrame = frame;
				}
			}
			return;
		}
	}

	void BeginVPADPoll(size_t playerIndex, uint64 frame)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_players.size())
			return;
		if (!s_passthroughLiveValid[playerIndex] || s_passthroughLiveFrame[playerIndex] != frame)
			s_passthroughLiveValid[playerIndex] = false;
		if (s_pollPlaybackLatchedValid[playerIndex] && s_pollPlaybackLatchedRuntimeFrame[playerIndex] != frame)
			s_pollPlaybackLatchedValid[playerIndex] = false;
	}

	bool TryGetVPADPlaybackSample(size_t playerIndex, uint64 frame, VPADMovieSample& outSample)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_players.size() || s_movieMode != MovieMode::Playback)
			return false;
		if (!TryAcquirePlaybackStartupSyncNoLock(playerIndex, frame))
			return false;

		uint64 movieFrame = 0;
		if (s_pollPlaybackLatchedValid[playerIndex] && s_pollPlaybackLatchedRuntimeFrame[playerIndex] == frame)
		{
			movieFrame = s_pollPlaybackLatchedMovieFrame[playerIndex];
		}
		else
		{
			// Consume one movie sample per unique VPAD poll frame.
			// This prevents runtime frame jumps from skipping inputs.
			movieFrame = s_pollPlaybackCursor[playerIndex];
			s_pollPlaybackLatchedValid[playerIndex] = true;
			s_pollPlaybackLatchedRuntimeFrame[playerIndex] = frame;
			s_pollPlaybackLatchedMovieFrame[playerIndex] = movieFrame;
			if (s_pollPlaybackCursor[playerIndex] < std::numeric_limits<uint64>::max())
				++s_pollPlaybackCursor[playerIndex];
		}
		const auto* frameInput = GetFrameFor(s_players[playerIndex], movieFrame, s_loop);
		if (!frameInput)
			return false;

		if (s_movieSignaturesTrusted && !s_loop)
		{
			const uint32 runtimeSignature = ComputeRuntimeSignature(frame);
			if (frameInput->signature != 0 && frameInput->signature != runtimeSignature)
			{
				const auto& player = s_players[playerIndex];
				const auto findSignatureNearby = [&](uint64 centerFrame, uint64 windowFrames) -> const TasFrameInput*
				{
					if (player.frames.empty())
						return nullptr;
					const uint64 startFrame = (centerFrame > windowFrames) ? (centerFrame - windowFrames) : 0;
					const uint64 endFrame = centerFrame + windowFrames;
					auto it = std::lower_bound(player.frames.cbegin(), player.frames.cend(), startFrame,
						[](const TasFrameInput& e, uint64 f) { return e.frame < f; });
					for (; it != player.frames.cend() && it->frame <= endFrame; ++it)
					{
						if (it->signature == runtimeSignature)
							return &(*it);
					}
					return nullptr;
				};

				// Deterministic alignment for Loading/RNG Sections.
				const TasFrameInput* aligned = findSignatureNearby(movieFrame, 192);
				if (!aligned)
					aligned = findSignatureNearby(movieFrame, 2048);

				if (aligned)
				{
					movieFrame = aligned->frame;
					frameInput = aligned;
					s_pollPlaybackCursor[playerIndex] = movieFrame + 1;
					s_pollPlaybackLatchedMovieFrame[playerIndex] = movieFrame;
					s_movieDesynced = false;
					cemuLog_log(LogType::Force,
						"TAS: playback signature realign player={} runtimeFrame={} movieFrame={}",
						playerIndex, frame, movieFrame);
				}
				else
				{
					s_movieDesynced = true;
				}
			}
		}

		outSample = FrameInputToMovieSample(*frameInput);
		return true;
	}

	void RecordVPADSample(size_t playerIndex, uint64 frame, const VPADMovieSample& sample)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_players.size() || s_movieMode != MovieMode::Record)
			return;
		if (s_recordLastRuntimeFrame[playerIndex] == frame)
			return;
		s_recordLastRuntimeFrame[playerIndex] = frame;

		const uint64 movieFrame = s_pollRecordCursor[playerIndex]++;

		auto input = MovieSampleToFrameInput(sample);
		input.frame = movieFrame;
		input.signature = ComputeRuntimeSignature(frame);
		UpsertFrameInput(playerIndex, input);
		s_lastRecordedFrame = std::max(s_lastRecordedFrame, movieFrame);

		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		if (titleId != 0)
			s_movieTitleId = titleId;
		s_movieHash = ComputeMovieHashNoLock();
		s_movieDirty = true;
	}

	MovieMode GetMovieMode()
	{
		std::scoped_lock lock(s_mutex);
		return s_movieMode;
	}

	MovieRecordPolicy GetMovieRecordPolicy()
	{
		std::scoped_lock lock(s_mutex);
		return s_MovieRecordPolicy;
	}

	bool IsMovieActive()
	{
		std::scoped_lock lock(s_mutex);
		return s_movieMode != MovieMode::Disabled;
	}

	bool IsMovieDesynced()
	{
		std::scoped_lock lock(s_mutex);
		return s_movieDesynced;
	}

	bool IsStrictTasModeEnabled()
	{
		std::scoped_lock lock(s_mutex);
		return s_strictTasMode;
	}

	void CaptureMovieSync(MovieSyncData& outSync, bool& hasSync)
	{
		std::scoped_lock lock(s_mutex);
		hasSync = s_movieMode != MovieMode::Disabled;
		outSync = {};
		if (!hasSync)
			return;
		outSync.magic = kMovieSyncMagic;
		outSync.version = kMovieSyncVersion;
		outSync.movieHash = s_movieHash;
		outSync.frame = LatteGPUState.frameCounter;
		outSync.rerecordCount = s_movieRerecordCount;
		outSync.signature = ComputeRuntimeSignature(outSync.frame);
	}

	bool ValidateMovieSync(const MovieSyncData& sync, bool hasSync, std::string& outError)
	{
		std::scoped_lock lock(s_mutex);
		outError.clear();
		if (s_movieMode == MovieMode::Disabled)
			return true;
		if (!hasSync)
			return true;
		if (sync.magic != kMovieSyncMagic || sync.version != kMovieSyncVersion)
			return true;
		if (sync.movieHash != s_movieHash && s_MovieRecordPolicy == MovieRecordPolicy::ReadOnly)
		{
			outError = "Timeline movie sync mismatch (read-only mode)";
			if (s_strictTasMode)
				outError += " [strict TAS mode]";
			cemuLog_log(LogType::Force, "TAS: {}", outError);
			return true;
		}
		return true;
	}

	void OnTimelineLoaded(uint64 restoredFrame, bool hasSync, const MovieSyncData& sync)
	{
		std::scoped_lock lock(s_mutex);
		const auto preservedPlaybackCursor = s_pollPlaybackCursor;
		const auto preservedRecordCursor = s_pollRecordCursor;
		ResetPollStateNoLock();
		s_lastRecordedFrame = std::numeric_limits<uint64>::max();
		if (s_movieMode == MovieMode::Playback)
		{
			// Preserve cursor exactly: playback is poll driven.
			s_pollPlaybackCursor = preservedPlaybackCursor;
			s_pollRecordCursor = preservedRecordCursor;
			s_playbackCursorRestoredFromBlob = false;
			s_playbackStartupSyncPending = false;
			s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
			s_movieDesynced = false;
			cemuLog_log(LogType::Force,
				"TAS: Timeline playback anchored runtimeFrame={} movieFrame0={}",
				restoredFrame, s_pollPlaybackCursor[0]);
		}
		if (s_movieMode != MovieMode::Record)
			return;
		if (!hasSync || sync.magic != kMovieSyncMagic || sync.version != kMovieSyncVersion)
			return;
		if (s_MovieRecordPolicy == MovieRecordPolicy::ReadOnly)
			return;

		uint64 truncateAtFrame = restoredFrame;
		if (s_playbackCursorRestoredFromBlob)
		{
			const uint64 maxCursor = *std::max_element(preservedRecordCursor.begin(), preservedRecordCursor.end());
			truncateAtFrame = (maxCursor > 0) ? (maxCursor - 1) : 0;
		}
		TruncateMovieAfterFrame(truncateAtFrame);
		InitializeRecordPollCursorFromFramesNoLock();
		s_playbackCursorRestoredFromBlob = false;
		++s_movieRerecordCount;
		s_movieHash = ComputeMovieHashNoLock();
		s_movieDirty = true;
		FlushMovieToFileNoLock();
		s_movieDirty = false;
		s_lastMovieFlushFrame = restoredFrame;
	}

	bool SerializeMovieBlob(std::vector<uint8>& outBlob)
	{
		std::scoped_lock lock(s_mutex);
		outBlob.clear();
		if (s_movieMode == MovieMode::Disabled)
			return false;

		auto appendBytes = [&outBlob](const void* data, size_t size)
		{
			const auto* src = static_cast<const uint8*>(data);
			outBlob.insert(outBlob.end(), src, src + size);
		};
		auto appendU32 = [&appendBytes](uint32 value) { appendBytes(&value, sizeof(value)); };
		auto appendU64 = [&appendBytes](uint64 value) { appendBytes(&value, sizeof(value)); };
		auto appendF32 = [&appendBytes](float value) { appendBytes(&value, sizeof(value)); };

		appendU32(kMovieBlobMagic);
		appendU32(kMovieBlobVersion);
		appendU32((uint32)s_movieMode);
		appendU32((uint32)s_MovieRecordPolicy);
		appendU32(s_loop ? 1u : 0u);
		appendU32(s_deterministicScheduler ? 1u : 0u);
		appendU32(s_deterministicTime ? 1u : 0u);
		appendU32(s_movieRerecordCount);
		appendU64(s_movieHash);
		appendU64(s_lastRecordedFrame);
		appendU32((uint32)s_players.size());

		for (const auto& player : s_players)
		{
			appendU64(player.maxFrame);
			appendU32((uint32)player.frames.size());
			for (const auto& frame : player.frames)
			{
				appendU64(frame.frame);
				appendF32(frame.lx);
				appendF32(frame.ly);
				appendF32(frame.rx);
				appendF32(frame.ry);
				appendF32(frame.zl);
				appendF32(frame.zr);
				appendU32(frame.buttons);
				appendU32(frame.signature);
				appendU32(frame.vpadHold);
			}
		}
		appendU32(static_cast<uint32>(s_movieInputTiming));
		for (size_t player = 0; player < s_pollPlaybackCursor.size(); ++player)
		{
			const uint64 cursor = (s_movieMode == MovieMode::Record) ? s_pollRecordCursor[player] : s_pollPlaybackCursor[player];
			appendU64(cursor);
		}

		return true;
	}

	bool DeserializeMovieBlob(const uint8* data, size_t size, std::string& outError)
	{
		std::scoped_lock lock(s_mutex);
		outError.clear();
		const MovieMode activeMovieModeBeforeLoad = s_movieMode;
		const auto& cfg = GetWxGUIConfig().tas;
		const uint32 configuredMovieMode = std::clamp<uint32>((uint32)cfg.movie_mode, 0, 2);
		const uint32 configuredTimelineMode = std::clamp<uint32>((uint32)cfg.movie_record_policy, 0, 1);
		const MovieMode runtimeMovieModePolicy = (configuredMovieMode == 2) ? MovieMode::Record : ((configuredMovieMode == 1) ? MovieMode::Playback : MovieMode::Disabled);
		const MovieRecordPolicy runtimeTimelineModePolicy = (configuredTimelineMode == 1) ? MovieRecordPolicy::ReadWrite : MovieRecordPolicy::ReadOnly;
		if (data == nullptr || size < sizeof(uint32) * 2)
		{
			outError = "Invalid movie blob";
			return false;
		}

		size_t offset = 0;
		auto readBytes = [data, size, &offset](void* out, size_t bytes) -> bool
		{
			if (offset + bytes > size)
				return false;
			memcpy(out, data + offset, bytes);
			offset += bytes;
			return true;
		};
		auto readU32 = [&readBytes](uint32& value) { return readBytes(&value, sizeof(value)); };
		auto readU64 = [&readBytes](uint64& value) { return readBytes(&value, sizeof(value)); };
		auto readF32 = [&readBytes](float& value) { return readBytes(&value, sizeof(value)); };

		uint32 magic = 0;
		uint32 version = 0;
		if (!readU32(magic) || !readU32(version))
		{
			outError = "Corrupted movie blob header";
			return false;
		}
		if (magic != kMovieBlobMagic || version == 0 || version > kMovieBlobVersion)
		{
			outError = "Unsupported movie blob";
			return false;
		}

		uint32 mode = 0;
		uint32 TimelineMode = 0;
		uint32 loop = 0;
		uint32 deterministicScheduler = 0;
		uint32 deterministicTime = 0;
		uint32 rerecordCount = 0;
		uint64 movieHash = 0;
		uint64 lastRecordedFrame = 0;
		uint32 playerCount = 0;
		std::array<uint64, InputManager::kMaxVPADControllers> restoredPlaybackCursor{};
		restoredPlaybackCursor.fill(0);
		bool hasRestoredPlaybackCursor = false;
		if (!readU32(mode) || !readU32(TimelineMode) || !readU32(loop) ||
			!readU32(deterministicScheduler) || !readU32(deterministicTime) ||
			!readU32(rerecordCount) || !readU64(movieHash) || !readU64(lastRecordedFrame) ||
			!readU32(playerCount))
		{
			outError = "Corrupted movie blob metadata";
			return false;
		}
		if (playerCount == 0 || playerCount > s_players.size())
		{
			outError = "Invalid movie blob player count";
			return false;
		}

		for (auto& p : s_players)
		{
			p.frames.clear();
			p.maxFrame = 0;
		}

		for (uint32 player = 0; player < playerCount; ++player)
		{
			uint64 maxFrame = 0;
			uint32 frameCount = 0;
			if (!readU64(maxFrame) || !readU32(frameCount))
			{
				outError = "Corrupted movie blob player header";
				return false;
			}

			auto& dst = s_players[player];
			dst.frames.reserve(frameCount);
			for (uint32 i = 0; i < frameCount; ++i)
			{
				TasFrameInput frameInput{};
				if (!readU64(frameInput.frame) ||
					!readF32(frameInput.lx) || !readF32(frameInput.ly) ||
					!readF32(frameInput.rx) || !readF32(frameInput.ry) ||
					!readF32(frameInput.zl) || !readF32(frameInput.zr) ||
					!readU32(frameInput.buttons) || !readU32(frameInput.signature))
				{
					outError = "Corrupted movie blob frame data";
					return false;
				}
				frameInput.lx = ClampStick(frameInput.lx);
				frameInput.ly = ClampStick(frameInput.ly);
				frameInput.rx = ClampStick(frameInput.rx);
				frameInput.ry = ClampStick(frameInput.ry);
				frameInput.zl = ClampTrigger(frameInput.zl);
				frameInput.zr = ClampTrigger(frameInput.zr);
				frameInput.vpadHold = 0;
				if (version >= 3)
				{
					if (!readU32(frameInput.vpadHold))
					{
						outError = "Corrupted movie blob frame hold data";
						return false;
					}
				}
				dst.frames.emplace_back(frameInput);
			}
			std::ranges::sort(dst.frames, {}, &TasFrameInput::frame);
			dst.maxFrame = dst.frames.empty() ? 0 : std::max(maxFrame, dst.frames.back().frame);
		}
		if (offset + sizeof(uint32) <= size)
		{
			uint32 inputTiming = 0;
			if (readU32(inputTiming))
			{
				s_movieInputTiming = (inputTiming == static_cast<uint32>(MovieInputTiming::Poll)) ? MovieInputTiming::Poll : MovieInputTiming::Frame;
				if (s_movieInputTiming == MovieInputTiming::Poll)
				{
					s_movieInputTiming = MovieInputTiming::Frame;
					cemuLog_log(LogType::Force, "TAS: coerced unsupported poll movie timing to frame timing (movie blob)");
				}
			}
		}
		if (version >= 2)
		{
			bool haveAllCursors = true;
			for (size_t player = 0; player < restoredPlaybackCursor.size(); ++player)
			{
				if (!readU64(restoredPlaybackCursor[player]))
				{
					haveAllCursors = false;
					break;
				}
			}
			hasRestoredPlaybackCursor = haveAllCursors;
		}

		s_loop = loop != 0;
		s_deterministicScheduler = deterministicScheduler != 0;
		s_deterministicTime = deterministicTime != 0;
		EnforceStrictTasPolicyNoLock();
		(void)mode;
		(void)TimelineMode;
		// Keep policy from runtime config.
		s_MovieRecordPolicy = runtimeTimelineModePolicy;

		if (activeMovieModeBeforeLoad == MovieMode::Playback)
		{
			s_movieMode = MovieMode::Playback;
		}
		else if (activeMovieModeBeforeLoad == MovieMode::Record && s_MovieRecordPolicy == MovieRecordPolicy::ReadWrite)
		{
			s_movieMode = MovieMode::Record;
		}

		else if (runtimeMovieModePolicy == MovieMode::Disabled && s_MovieRecordPolicy == MovieRecordPolicy::ReadOnly)
			s_movieMode = MovieMode::Playback;
		else if (runtimeMovieModePolicy == MovieMode::Disabled)
			s_movieMode = MovieMode::Disabled;
		else if (runtimeMovieModePolicy == MovieMode::Record && s_MovieRecordPolicy == MovieRecordPolicy::ReadWrite)
			s_movieMode = MovieMode::Record;
		else
			s_movieMode = MovieMode::Playback;
		s_movieRerecordCount = rerecordCount;
		s_movieHash = (movieHash != 0) ? movieHash : ComputeMovieHashNoLock();
		s_movieDesynced = false;
		s_movieDirty = false;
		s_movieAnchorToFirstPlaybackFrame = false;
		s_moviePlaybackFrameAnchorInitialized = false;
		s_moviePlaybackFrameAnchor = 0;
		s_moviePlaybackStartMovieFrame = 0;
		s_playbackStartupSyncPending = false;
		s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
		s_movieSignaturesTrusted = true;
		s_lastRecordedFrame = lastRecordedFrame;
		ResetPollStateNoLock();
		if (hasRestoredPlaybackCursor)
		{
			s_pollPlaybackCursor = restoredPlaybackCursor;
			s_pollRecordCursor = restoredPlaybackCursor;
		}
		else
		{
			InitializeRecordPollCursorFromFramesNoLock();
		}
		s_playbackCursorRestoredFromBlob = hasRestoredPlaybackCursor;
		s_movieSignaturesTrusted = EvaluateMovieSignaturesTrustedNoLock();
		if (!s_movieSignaturesTrusted)
			cemuLog_log(LogType::Force, "TAS: movie signatures marked untrusted (using frame-order playback)");
		return true;
	}

	bool ExportMovieToFile(const fs::path& path, std::string& outError)
	{
		std::scoped_lock lock(s_mutex);
		outError.clear();
		if (path.empty())
		{
			outError = "Invalid output path";
			return false;
		}
		if (s_movieMode == MovieMode::Disabled)
		{
			outError = "Movie mode is disabled";
			return false;
		}

		std::ofstream out(path, std::ios::trunc);
		if (!out.is_open())
		{
			outError = "Failed to open movie output file";
			return false;
		}

		out << "CTM1\n";
		out << "M,loop," << (s_loop ? "1" : "0") << "\n";
		out << "M,deterministic_scheduler," << (s_deterministicScheduler ? "1" : "0") << "\n";
		out << "M,deterministic_time," << (s_deterministicTime ? "1" : "0") << "\n";
		out << "M,movie_mode,1\n";
		out << "M,movie_record_policy," << static_cast<uint32>(s_MovieRecordPolicy) << "\n";
		out << "M,input_timing," << ((s_movieInputTiming == MovieInputTiming::Poll) ? "poll" : "frame") << "\n";
		out << "M,rerecord_count," << s_movieRerecordCount << "\n";
		out << "M,movie_hash," << s_movieHash << "\n";
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		out << "M,title_id," << (titleId != 0 ? titleId : s_movieTitleId) << "\n";
		for (size_t player = 0; player < s_players.size(); ++player)
		{
			for (const auto& frame : s_players[player].frames)
			{
				out << "F," << frame.frame
					<< "," << player
					<< "," << frame.lx
					<< "," << frame.ly
					<< "," << frame.rx
					<< "," << frame.ry
					<< "," << frame.zl
					<< "," << frame.zr
					<< "," << frame.buttons
					<< "," << frame.signature
					<< "," << frame.vpadHold
					<< "\n";
			}
		}
		if (!out.good())
		{
			outError = "Failed while writing movie output file";
			return false;
		}
		return true;
	}

	bool ReadMovieTitleIdFromFile(const fs::path& path, uint64& outTitleId, std::string& outError)
	{
		std::scoped_lock lock(s_mutex);
		outTitleId = 0;
		outError.clear();
		if (path.empty())
		{
			outError = "Invalid movie file path";
			return false;
		}

		std::ifstream file(path);
		if (!file.is_open())
		{
			outError = "Failed to open movie file";
			return false;
		}

		std::string line;
		while (std::getline(file, line))
		{
			const std::string trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == '#')
				continue;

			auto columns = Split(trimmed, ',');
			for (auto& c : columns)
				c = Trim(c);
			if (columns.empty())
				continue;

			const std::string tag = boost::to_upper_copy(columns[0]);
			if (tag == "F")
				break;
			if (tag != "M" || columns.size() < 3)
				continue;

			const std::string key = boost::to_lower_copy(columns[1]);
			if (key != "title_id")
				continue;

			if (!ParseUInt64AutoBase(columns[2], outTitleId))
			{
				outError = "Invalid title_id metadata in movie file";
				return false;
			}
			return true;
		}
		return true;
	}

	bool ImportMovieForPlaybackFromFile(const fs::path& path, std::string& outError, bool anchorToFirstPlaybackFrame, uint64 playbackStartMovieFrame)
	{
		std::scoped_lock lock(s_mutex);
		outError.clear();
		if (path.empty())
		{
			outError = "Invalid movie file path";
			return false;
		}

		std::ifstream file(path);
		if (!file.is_open())
		{
			outError = "Failed to open movie file";
			return false;
		}

		for (auto& p : s_players)
		{
			p.frames.clear();
			p.maxFrame = 0;
		}
		s_movieHash = 0;
		s_movieTitleId = 0;
		s_movieRerecordCount = 0;
		s_movieInputTiming = MovieInputTiming::Frame;
		s_movieDesynced = false;
		s_movieDirty = false;
		s_lastRecordedFrame = std::numeric_limits<uint64>::max();

		std::string line;
		const bool parseAsCustomMovie = boost::iequals(path.extension().string(), ".ctm");
		while (std::getline(file, line))
		{
			const std::string trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == '#')
				continue;

			auto columns = Split(trimmed, ',');
			for (auto& c : columns)
				c = Trim(c);

			if (parseAsCustomMovie)
			{
				if (!ParseCustomMovieLine(columns))
				{
					outError = "Invalid CTM movie line: " + trimmed;
					return false;
				}
				continue;
			}

			if (!ParseLegacyCsvLine(columns))
			{
				outError = "Invalid legacy CSV movie line: " + trimmed;
				return false;
			}
		}

		for (auto& p : s_players)
			std::ranges::sort(p.frames, {}, &TasFrameInput::frame);

		s_file = path;
		s_movieMode = MovieMode::Playback;
		s_frameAdvancePaused = false;
		s_frameAdvanceSteps = 0;
		s_frameAdvanceVisualRefreshPermits = 0;
		s_frameAdvanceStepActive = false;
		s_blockInputUntilNextFrameAdvance = false;
		s_movieAnchorToFirstPlaybackFrame = false;
		s_moviePlaybackFrameAnchorInitialized = false;
		s_moviePlaybackFrameAnchor = 0;
		s_moviePlaybackStartMovieFrame = 0;

		// Keep startup sync disabled here.
		s_playbackStartupSyncPending = false;
		s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
		ResetPollStateNoLock();
		s_pollPlaybackCursor.fill(playbackStartMovieFrame);
		(void)anchorToFirstPlaybackFrame;
		InitializeRecordPollCursorFromFramesNoLock();
		s_playbackCursorRestoredFromBlob = anchorToFirstPlaybackFrame && playbackStartMovieFrame > 0;
		s_lastMovieFlushFrame = std::numeric_limits<uint64>::max();
		EnforceStrictTasPolicyNoLock();
		if (s_movieHash == 0)
			s_movieHash = ComputeMovieHashNoLock();
		s_movieSignaturesTrusted = EvaluateMovieSignaturesTrustedNoLock();
		if (!s_movieSignaturesTrusted)
			cemuLog_log(LogType::Force, "TAS: movie signatures marked untrusted (using frame-order playback)");
		return true;
	}

	bool EnsureMovieRecordTimeline(const fs::path& path, std::string& outError)
	{
		std::scoped_lock lock(s_mutex);
		outError.clear();
		if (path.empty())
		{
			outError = "Invalid movie file path";
			return false;
		}

		// Already recording this file.
		if (s_movieMode == MovieMode::Record && s_file == path)
			return true;
		if (s_movieMode == MovieMode::Playback)
			return true;
		if (s_movieMode == MovieMode::Disabled || s_file != path)
		{
			for (auto& p : s_players)
			{
				p.frames.clear();
				p.maxFrame = 0;
			}
			s_movieHash = 0;
			s_movieTitleId = 0;
			s_movieRerecordCount = 0;
			s_movieInputTiming = MovieInputTiming::Frame;
			s_movieDesynced = false;
			s_movieDirty = false;
			s_lastRecordedFrame = std::numeric_limits<uint64>::max();

			std::ifstream file(path);
			if (file.is_open())
			{
				std::string line;
				const bool parseAsCustomMovie = boost::iequals(path.extension().string(), ".ctm");
				while (std::getline(file, line))
				{
					const std::string trimmed = Trim(line);
					if (trimmed.empty() || trimmed[0] == '#')
						continue;

					auto columns = Split(trimmed, ',');
					for (auto& c : columns)
						c = Trim(c);

					if (parseAsCustomMovie)
					{
						if (!ParseCustomMovieLine(columns))
						{
							outError = "Invalid CTM movie line: " + trimmed;
							return false;
						}
					}
					else
					{
						if (!ParseLegacyCsvLine(columns))
						{
							outError = "Invalid legacy CSV movie line: " + trimmed;
							return false;
						}
					}
				}
			}
			else
			{
				// Keep recording/playback in one timing model.
				s_movieInputTiming = MovieInputTiming::Frame;
			}

			for (auto& p : s_players)
				std::ranges::sort(p.frames, {}, &TasFrameInput::frame);

			s_file = path;
		}

		s_movieMode = MovieMode::Record;
		s_frameAdvancePaused = false;
		s_frameAdvanceSteps = 0;
		s_frameAdvanceVisualRefreshPermits = 0;
		s_frameAdvanceStepActive = false;
		s_blockInputUntilNextFrameAdvance = false;
		s_movieAnchorToFirstPlaybackFrame = false;
		s_moviePlaybackFrameAnchorInitialized = false;
		s_moviePlaybackFrameAnchor = 0;
		s_moviePlaybackStartMovieFrame = 0;
		s_playbackStartupSyncPending = false;
		s_playbackStartupSyncBeginRuntimeFrame = std::numeric_limits<uint64>::max();
		s_movieSignaturesTrusted = true;
		ResetPollStateNoLock();
		InitializeRecordPollCursorFromFramesNoLock();
		s_playbackCursorRestoredFromBlob = false;
		s_lastMovieFlushFrame = std::numeric_limits<uint64>::max();
		EnforceStrictTasPolicyNoLock();
		s_movieHash = ComputeMovieHashNoLock();
		return true;
	}

	void SetManualInputEnabled(bool enabled)
	{
		std::scoped_lock lock(s_mutex);
		(void)enabled;
		s_manualEnabled = true;
	}

	bool IsManualInputEnabled()
	{
		std::scoped_lock lock(s_mutex);
		return s_manualEnabled;
	}

	void SetControllerInputPassthroughEnabled(bool enabled)
	{
		std::scoped_lock lock(s_mutex);
		s_controllerInputPassthroughEnabled = enabled;
	}

	bool IsControllerInputPassthroughEnabled()
	{
		std::scoped_lock lock(s_mutex);
		return s_controllerInputPassthroughEnabled;
	}

	ManualState GetManualInputState(size_t playerIndex)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualPlayers.size())
			return {};
		return s_manualPlayers[playerIndex];
	}

	void SetManualInputState(size_t playerIndex, const ManualState& state)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualPlayers.size())
			return;
		s_manualPlayers[playerIndex] = state;
	}

	void SetManualTurboMask(size_t playerIndex, uint32 turboMask)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualTurboMasks.size())
			return;
		s_manualTurboMasks[playerIndex] = turboMask;
	}

	uint32 GetManualTurboMask(size_t playerIndex)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualTurboMasks.size())
			return 0;
		return s_manualTurboMasks[playerIndex];
	}

	void SetManualTurboInterval(size_t playerIndex, uint32 intervalFrames)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualTurboIntervals.size())
			return;
		s_manualTurboIntervals[playerIndex] = std::max<uint32>(1, intervalFrames);
	}

	uint32 GetManualTurboInterval(size_t playerIndex)
	{
		std::scoped_lock lock(s_mutex);
		if (playerIndex >= s_manualTurboIntervals.size())
			return 1;
		return std::max<uint32>(1, s_manualTurboIntervals[playerIndex]);
	}

	bool HasOverlayData()
	{
		std::scoped_lock lock(s_mutex);
		return s_manualEnabled || s_enabled || s_movieMode != MovieMode::Disabled || s_frameAdvancePaused;
	}

	OverlayState GetOverlayState(uint64 frame, size_t playerIndex)
	{
		std::scoped_lock lock(s_mutex);
		OverlayState out{};
		out.frame = frame;
		out.frameAdvancePaused = s_frameAdvancePaused;
		if (playerIndex >= s_players.size())
			return out;

		// Playback overrides manual input when movie mode is active.
		if (s_movieMode == MovieMode::Playback)
		{
			out.active = true;
			out.manual = false;
			out.playback = true;
			const uint64 movieFrame = ResolvePlaybackMovieFrameNoLock(playerIndex, frame);
			const auto* playbackFrameInput = GetFrameFor(s_players[playerIndex], movieFrame, s_loop);
			if (!playbackFrameInput)
				return out;

			out.lx = playbackFrameInput->lx;
			out.ly = playbackFrameInput->ly;
			out.rx = playbackFrameInput->rx;
			out.ry = playbackFrameInput->ry;
			out.zl = playbackFrameInput->zl;
			out.zr = playbackFrameInput->zr;
			out.buttons = playbackFrameInput->buttons;
			return out;
		}

		if (s_manualEnabled)
		{
			out.active = true;
			out.manual = true;
			out.playback = false;
			auto frameInput = ManualStateToFrameInput(s_manualPlayers[playerIndex]);
			frameInput.buttons = ApplyTurboMask(
				frameInput.buttons,
				s_manualTurboMasks[playerIndex],
				std::max<uint32>(1, s_manualTurboIntervals[playerIndex]),
				frame);
			out.lx = frameInput.lx;
			out.ly = frameInput.ly;
			out.rx = frameInput.rx;
			out.ry = frameInput.ry;
			out.zl = frameInput.zl;
			out.zr = frameInput.zr;
			out.buttons = frameInput.buttons;
			return out;
		}

		if (!s_enabled && s_movieMode != MovieMode::Playback)
		{
			out.active = out.frameAdvancePaused;
			return out;
		}

		out.active = true;
		out.manual = false;
		out.playback = true;
		const uint64 movieFrame = ResolvePlaybackMovieFrameNoLock(playerIndex, frame);
		const auto* playbackFrameInput = GetFrameFor(s_players[playerIndex], movieFrame, s_loop);
		if (!playbackFrameInput)
			return out;

		out.lx = playbackFrameInput->lx;
		out.ly = playbackFrameInput->ly;
		out.rx = playbackFrameInput->rx;
		out.ry = playbackFrameInput->ry;
		out.zl = playbackFrameInput->zl;
		out.zr = playbackFrameInput->zr;
		out.buttons = playbackFrameInput->buttons;
		return out;
	}

	bool QueryVPADMappingValue(size_t playerIndex, uint64 frame, uint64 mapping, float& outValue)
	{
		if (s_bypassTasQueryForLiveCapture)
			return false;
		std::unique_lock lock(s_mutex);
		if (playerIndex >= s_players.size())
			return false;
		(void)frame;

		// Movie playback is injected at VPAD sample boundary (VPADRead)
		if (s_movieMode == MovieMode::Playback)
			return false;

		if (s_manualEnabled)
		{
			TasFrameInput frameInput{};
			uint32 turboMask = s_manualTurboMasks[playerIndex];
			uint32 turboInterval = std::max<uint32>(1, s_manualTurboIntervals[playerIndex]);
			if (s_controllerInputPassthroughEnabled)
			{
				if (!s_passthroughLiveValid[playerIndex] || s_passthroughLiveFrame[playerIndex] != frame)
				{
					lock.unlock();
					frameInput = CaptureLiveVPADInputForFrame(playerIndex, frame);
					lock.lock();
					s_passthroughLiveValid[playerIndex] = true;
					s_passthroughLiveFrame[playerIndex] = frame;
					s_passthroughLiveInput[playerIndex] = frameInput;
				}
				else
				{
					frameInput = s_passthroughLiveInput[playerIndex];
				}
				UpdateManualStateFromFrameInputNoLock(playerIndex, frameInput);
			}
			frameInput = ManualStateToFrameInput(s_manualPlayers[playerIndex]);
			frameInput.buttons = ApplyTurboMask(
				frameInput.buttons,
				turboMask,
				turboInterval,
				frame);
			return QueryFrameValue(frameInput, mapping, outValue);
		}

		if (s_movieMode == MovieMode::Record)
			return false;

		if (!s_enabled)
			return false;

		const uint64 movieFrame = ResolveMovieQueryFrameNoLock(frame);
		const auto* playbackFrameInput = GetFrameFor(s_players[playerIndex], movieFrame, s_loop);
		if (!playbackFrameInput)
			return false;

		return QueryFrameValue(*playbackFrameInput, mapping, outValue);
	}
}

