// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/optional.hpp>
#include <cryptopp/hex.h>
#include <cryptopp/osrng.h>
#include <fmt/ranges.h>
#include "common/archives.h"
#include "common/bit_field.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/swap.h"
#include "common/timer.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/extra_hid.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "video_core/gpu.h"

namespace Core {

enum class ControllerStateType : u8 {
    PadAndCircle,
    Touch,
    Accelerometer,
    Gyroscope,
    IrRst,
    ExtraHidResponse
};

#pragma pack(push, 1)
struct ControllerState {
    ControllerStateType type;

    union {
        struct {
            union {
                u16_le hex = 0;

                BitField<0, 1, u16> a;
                BitField<1, 1, u16> b;
                BitField<2, 1, u16> select;
                BitField<3, 1, u16> start;
                BitField<4, 1, u16> right;
                BitField<5, 1, u16> left;
                BitField<6, 1, u16> up;
                BitField<7, 1, u16> down;
                BitField<8, 1, u16> r;
                BitField<9, 1, u16> l;
                BitField<10, 1, u16> x;
                BitField<11, 1, u16> y;
                BitField<12, 1, u16> debug;
                BitField<13, 1, u16> gpio14;
                // Bits 14-15 are currently unused
            };
            s16_le circle_pad_x;
            s16_le circle_pad_y;
        } pad_and_circle;

        struct {
            u16_le x;
            u16_le y;
            // This is a bool, u8 for platform compatibility
            u8 valid;
        } touch;

        struct {
            s16_le x;
            s16_le y;
            s16_le z;
        } accelerometer;

        struct {
            s16_le x;
            s16_le y;
            s16_le z;
        } gyroscope;

        struct {
            s16_le x;
            s16_le y;
            // These are bool, u8 for platform compatibility
            u8 zl;
            u8 zr;
        } ir_rst;

        struct {
            union {
                u32_le hex = 0;

                BitField<0, 5, u32> battery_level;
                BitField<5, 1, u32> zl_not_held;
                BitField<6, 1, u32> zr_not_held;
                BitField<7, 1, u32> r_not_held;
                BitField<8, 12, u32> c_stick_x;
                BitField<20, 12, u32> c_stick_y;
            };
        } extra_hid_response;
    };
};
static_assert(sizeof(ControllerState) == 7, "ControllerState should be 7 bytes");
#pragma pack(pop)

constexpr std::array<u8, 4> header_magic_bytes{{'C', 'T', 'M', 0x1B}};

#pragma pack(push, 1)
struct CTMHeader {
    std::array<u8, 4> filetype;  /// Unique Identifier to check the file type (always "CTM"0x1B)
    u64_le program_id;           /// ID of the ROM being executed. Also called title_id
    std::array<u8, 20> revision; /// Git hash of the revision this movie was created with
    u64_le clock_init_time;      /// The init time of the system clock
    u64_le id; /// Unique identifier of the movie, used to support separate savestate slots
    std::array<char, 32> author; /// Author of the movie
    u32_le rerecord_count;       /// Number of rerecords when making the movie
    u64_le input_count;          /// Number of inputs (button and pad states) when making the movie
    s64_le timing_base_ticks;    /// The base system tick count to initialize core timing with.

    std::array<u8, 156> reserved; /// Make heading 256 bytes so it has consistent size
};
static_assert(sizeof(CTMHeader) == 256, "CTMHeader should be 256 bytes");

namespace {

constexpr std::size_t NumStateTypes = 6;
constexpr u64 NoFrame = std::numeric_limits<u64>::max();

constexpr u8 TypeBit(ControllerStateType type) {
    return static_cast<u8>(1u << static_cast<u8>(type));
}

constexpr u8 AllTypes = (1u << NumStateTypes) - 1;

// Pad bits of ControllerState::pad_and_circle that are shown in the editor (A to Y)
constexpr u16 EditorButtonMask = 0x0FFF;

constexpr int CStickMax = 0x9C;
constexpr int ExtraHidCStickCenter = 0x800;
constexpr int ExtraHidCStickRadius = 0x7FF;

} // namespace

struct Movie::TasData {
    struct Frame {
        /// Inputs captured when the frame was emulated, in the order they were read
        std::vector<ControllerState> polls;
        /// Values shown in the editor (from the first input of each type, or set by the user)
        TasFrame values;
        /// Types (bits) whose inputs are generated from `values` instead of replayed from `polls`
        u8 edited = 0;
        /// Types (bits) that have at least one captured input
        u8 seen = 0;
        /// False for frames emulated before the editor was enabled
        bool known = true;
    };

    /// Replay position within the current frame
    struct Position {
        u64 frame = NoFrame;
        std::array<u32, NumStateTypes> cursors{};
        bool capturing = false;
    };

    struct State {
        std::vector<u8> data;
        Position position;
    };

    std::vector<Frame> frames;
    u64 first_frame = 0;
    Position position;
    bool overwrite = false;

    std::map<u64, State> states;
    u32 state_interval = 60;
    u32 state_capacity = 60;

    std::optional<u64> seek_request;
    std::optional<u64> seek_target;
    std::atomic<u64> current_frame{0};

    void ResetPosition() {
        position = Position{};
    }
};
#pragma pack(pop)

static u64 GetInputCount(std::span<const u8> input) {
    u64 input_count = 0;
    for (std::size_t pos = 0; pos < input.size(); pos += sizeof(ControllerState)) {
        if (input.size() < pos + sizeof(ControllerState)) {
            break;
        }

        ControllerState state{};
        std::memcpy(&state, input.data() + pos, sizeof(ControllerState));
        if (state.type == ControllerStateType::PadAndCircle) {
            input_count++;
        }
    }
    return input_count;
}

Movie::Movie(Core::System& system_) : system{system_} {}

Movie::~Movie() = default;

template <class Archive>
void Movie::serialize(Archive& ar, const unsigned int file_version) {
    // Only serialize what's needed to make savestates useful for TAS:
    u64 _current_byte = static_cast<u64>(current_byte);
    ar & _current_byte;
    current_byte = static_cast<std::size_t>(_current_byte);
    ar & current_input;

    std::vector<u8> recorded_input_ = recorded_input;
    ar & recorded_input_;

    ar & init_time;
    ar & base_ticks;

    if (Archive::is_loading::value) {
        u64 savestate_movie_id;
        ar & savestate_movie_id;
        if (id != savestate_movie_id) {
            if (savestate_movie_id == 0) {
                throw std::runtime_error("You must close your movie to load this state");
            } else {
                throw std::runtime_error("You must load the same movie to load this state");
            }
        }
    } else {
        ar & id;
    }

    if (Archive::is_loading::value && tas) {
        // The replay position within the frame is restored by TasRestoreStatePosition when the
        // state is from the TAS editor, otherwise start the frame over
        std::scoped_lock lock{tas_mutex};
        tas->ResetPosition();
    }

    // Whether the state was made in MovieFinished state
    bool post_movie = play_mode == PlayMode::MovieFinished;
    ar & post_movie;

    if (Archive::is_loading::value && id != 0) {
        if (!read_only) {
            recorded_input = std::move(recorded_input_);
        }

        if (post_movie) {
            play_mode = PlayMode::MovieFinished;
            return;
        }

        if (read_only) {
            if (play_mode == PlayMode::Recording) {
                SaveMovie();
            }
            if (recorded_input_.size() >= recorded_input.size()) {
                throw std::runtime_error("Future event savestate not allowed in R/O mode");
            }
            // Ensure that the current movie and savestate movie are in the same timeline
            if (std::mismatch(recorded_input_.begin(), recorded_input_.end(),
                              recorded_input.begin())
                    .first != recorded_input_.end()) {
                throw std::runtime_error("Timeline mismatch not allowed in R/O mode");
            }

            play_mode = PlayMode::Playing;
            total_input = GetInputCount(recorded_input);
        } else {
            play_mode = PlayMode::Recording;
            rerecord_count++;
        }
    }
}

SERIALIZE_IMPL(Movie)

Movie::PlayMode Movie::GetPlayMode() const {
    return play_mode;
}

u64 Movie::GetCurrentInputIndex() const {
    return static_cast<u64>(std::nearbyint(current_input / 234.0 * SCREEN_REFRESH_RATE));
}
u64 Movie::GetTotalInputCount() const {
    return static_cast<u64>(std::nearbyint(total_input / 234.0 * SCREEN_REFRESH_RATE));
}

void Movie::CheckInputEnd() {
    if (current_byte + sizeof(ControllerState) > recorded_input.size()) {
        LOG_INFO(Movie, "Playback finished");
        if (tas) {
            // The whole movie is now in the TAS editor, continue by recording from here
            play_mode = PlayMode::Recording;
            read_only = false;
            system.frame_limiter.SetUnthrottled(false);
        } else {
            play_mode = PlayMode::MovieFinished;
        }
        playback_completion_callback();
    }
}

void Movie::Play(Service::HID::PadState& pad_state, s16& circle_pad_x, s16& circle_pad_y) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);
    current_input++;

    if (s.type != ControllerStateType::PadAndCircle) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::PadAndCircle), s.type);
        return;
    }

    pad_state.a.Assign(s.pad_and_circle.a);
    pad_state.b.Assign(s.pad_and_circle.b);
    pad_state.select.Assign(s.pad_and_circle.select);
    pad_state.start.Assign(s.pad_and_circle.start);
    pad_state.right.Assign(s.pad_and_circle.right);
    pad_state.left.Assign(s.pad_and_circle.left);
    pad_state.up.Assign(s.pad_and_circle.up);
    pad_state.down.Assign(s.pad_and_circle.down);
    pad_state.r.Assign(s.pad_and_circle.r);
    pad_state.l.Assign(s.pad_and_circle.l);
    pad_state.x.Assign(s.pad_and_circle.x);
    pad_state.y.Assign(s.pad_and_circle.y);
    pad_state.debug.Assign(s.pad_and_circle.debug);
    pad_state.gpio14.Assign(s.pad_and_circle.gpio14);

    circle_pad_x = s.pad_and_circle.circle_pad_x;
    circle_pad_y = s.pad_and_circle.circle_pad_y;
}

void Movie::Play(Service::HID::TouchDataEntry& touch_data) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);

    if (s.type != ControllerStateType::Touch) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::Touch), s.type);
        return;
    }

    touch_data.x = s.touch.x;
    touch_data.y = s.touch.y;
    touch_data.valid.Assign(s.touch.valid);
}

void Movie::Play(Service::HID::AccelerometerDataEntry& accelerometer_data) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);

    if (s.type != ControllerStateType::Accelerometer) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::Accelerometer), s.type);
        return;
    }

    accelerometer_data.x = s.accelerometer.x;
    accelerometer_data.y = s.accelerometer.y;
    accelerometer_data.z = s.accelerometer.z;
}

void Movie::Play(Service::HID::GyroscopeDataEntry& gyroscope_data) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);

    if (s.type != ControllerStateType::Gyroscope) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::Gyroscope), s.type);
        return;
    }

    gyroscope_data.x = s.gyroscope.x;
    gyroscope_data.y = s.gyroscope.y;
    gyroscope_data.z = s.gyroscope.z;
}

void Movie::Play(Service::IR::PadState& pad_state, s16& c_stick_x, s16& c_stick_y) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);

    if (s.type != ControllerStateType::IrRst) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::IrRst), s.type);
        return;
    }

    c_stick_x = s.ir_rst.x;
    c_stick_y = s.ir_rst.y;
    pad_state.zl.Assign(s.ir_rst.zl);
    pad_state.zr.Assign(s.ir_rst.zr);
}

void Movie::Play(Service::IR::ExtraHIDResponse& extra_hid_response) {
    ControllerState s{};
    std::memcpy(&s, &recorded_input[current_byte], sizeof(ControllerState));
    current_byte += sizeof(ControllerState);

    if (s.type != ControllerStateType::ExtraHidResponse) {
        LOG_ERROR(Movie,
                  "Expected to read type {}, but found {}. Your playback will be out of sync",
                  static_cast<int>(ControllerStateType::ExtraHidResponse), s.type);
        return;
    }

    extra_hid_response.buttons.battery_level.Assign(
        static_cast<u8>(s.extra_hid_response.battery_level));
    extra_hid_response.c_stick.c_stick_x.Assign(s.extra_hid_response.c_stick_x);
    extra_hid_response.c_stick.c_stick_y.Assign(s.extra_hid_response.c_stick_y);
    extra_hid_response.buttons.r_not_held.Assign(static_cast<u8>(s.extra_hid_response.r_not_held));
    extra_hid_response.buttons.zl_not_held.Assign(
        static_cast<u8>(s.extra_hid_response.zl_not_held));
    extra_hid_response.buttons.zr_not_held.Assign(
        static_cast<u8>(s.extra_hid_response.zr_not_held));
}

void Movie::Record(const ControllerState& controller_state) {
    recorded_input.resize(current_byte + sizeof(ControllerState));
    std::memcpy(&recorded_input[current_byte], &controller_state, sizeof(ControllerState));
    current_byte += sizeof(ControllerState);
}

void Movie::Record(const Service::HID::PadState& pad_state, const s16& circle_pad_x,
                   const s16& circle_pad_y) {
    current_input++;

    ControllerState s{};
    s.type = ControllerStateType::PadAndCircle;

    s.pad_and_circle.a.Assign(static_cast<u16>(pad_state.a));
    s.pad_and_circle.b.Assign(static_cast<u16>(pad_state.b));
    s.pad_and_circle.select.Assign(static_cast<u16>(pad_state.select));
    s.pad_and_circle.start.Assign(static_cast<u16>(pad_state.start));
    s.pad_and_circle.right.Assign(static_cast<u16>(pad_state.right));
    s.pad_and_circle.left.Assign(static_cast<u16>(pad_state.left));
    s.pad_and_circle.up.Assign(static_cast<u16>(pad_state.up));
    s.pad_and_circle.down.Assign(static_cast<u16>(pad_state.down));
    s.pad_and_circle.r.Assign(static_cast<u16>(pad_state.r));
    s.pad_and_circle.l.Assign(static_cast<u16>(pad_state.l));
    s.pad_and_circle.x.Assign(static_cast<u16>(pad_state.x));
    s.pad_and_circle.y.Assign(static_cast<u16>(pad_state.y));
    s.pad_and_circle.debug.Assign(static_cast<u16>(pad_state.debug));
    s.pad_and_circle.gpio14.Assign(static_cast<u16>(pad_state.gpio14));

    s.pad_and_circle.circle_pad_x = circle_pad_x;
    s.pad_and_circle.circle_pad_y = circle_pad_y;

    Record(s);
}

void Movie::Record(const Service::HID::TouchDataEntry& touch_data) {
    ControllerState s{};
    s.type = ControllerStateType::Touch;

    s.touch.x = touch_data.x;
    s.touch.y = touch_data.y;
    s.touch.valid = static_cast<u8>(touch_data.valid);

    Record(s);
}

void Movie::Record(const Service::HID::AccelerometerDataEntry& accelerometer_data) {
    ControllerState s{};
    s.type = ControllerStateType::Accelerometer;

    s.accelerometer.x = accelerometer_data.x;
    s.accelerometer.y = accelerometer_data.y;
    s.accelerometer.z = accelerometer_data.z;

    Record(s);
}

void Movie::Record(const Service::HID::GyroscopeDataEntry& gyroscope_data) {
    ControllerState s{};
    s.type = ControllerStateType::Gyroscope;

    s.gyroscope.x = gyroscope_data.x;
    s.gyroscope.y = gyroscope_data.y;
    s.gyroscope.z = gyroscope_data.z;

    Record(s);
}

void Movie::Record(const Service::IR::PadState& pad_state, const s16& c_stick_x,
                   const s16& c_stick_y) {
    ControllerState s{};
    s.type = ControllerStateType::IrRst;

    s.ir_rst.x = c_stick_x;
    s.ir_rst.y = c_stick_y;
    s.ir_rst.zl = static_cast<u8>(pad_state.zl);
    s.ir_rst.zr = static_cast<u8>(pad_state.zr);

    Record(s);
}

void Movie::Record(const Service::IR::ExtraHIDResponse& extra_hid_response) {
    ControllerState s{};
    s.type = ControllerStateType::ExtraHidResponse;

    s.extra_hid_response.battery_level.Assign(extra_hid_response.buttons.battery_level);
    s.extra_hid_response.c_stick_x.Assign(extra_hid_response.c_stick.c_stick_x);
    s.extra_hid_response.c_stick_y.Assign(extra_hid_response.c_stick.c_stick_y);
    s.extra_hid_response.r_not_held.Assign(extra_hid_response.buttons.r_not_held);
    s.extra_hid_response.zl_not_held.Assign(extra_hid_response.buttons.zl_not_held);
    s.extra_hid_response.zr_not_held.Assign(extra_hid_response.buttons.zr_not_held);

    Record(s);
}

u64 Movie::GetOverrideInitTime() const {
    return init_time;
}

s64 Movie::GetOverrideBaseTicks() const {
    return base_ticks;
}

Movie::ValidationResult Movie::ValidateHeader(const CTMHeader& header) const {
    if (header_magic_bytes != header.filetype) {
        LOG_ERROR(Movie, "Playback file does not have valid header");
        return ValidationResult::Invalid;
    }

    std::string revision = fmt::format("{:02x}", fmt::join(header.revision, ""));
    if (revision != Common::g_scm_rev) {
        LOG_WARNING(
            Movie, // Refers to Citra intentionally, movie may be from Citra instead of Azahar
            "This movie was created on a different version of Citra, playback may desync");
        return ValidationResult::RevisionDismatch;
    }

    return ValidationResult::OK;
}

Movie::ValidationResult Movie::ValidateInput(std::span<const u8> input, u64 expected_count) const {
    return GetInputCount(input) == expected_count ? ValidationResult::OK
                                                  : ValidationResult::InputCountDismatch;
}

void Movie::SaveMovie() {
    LOG_INFO(Movie, "Saving recorded movie to '{}'", record_movie_file);
    FileUtil::IOFile save_record(record_movie_file, "wb");

    if (!save_record.IsGood()) {
        LOG_ERROR(Movie, "Unable to open file to save movie");
        return;
    }

    CTMHeader header = {};
    header.filetype = header_magic_bytes;
    header.program_id = program_id;
    header.clock_init_time = init_time;
    header.timing_base_ticks = base_ticks;
    header.id = id;

    std::memcpy(header.author.data(), record_movie_author.data(),
                std::min(header.author.size(), record_movie_author.size()));

    header.rerecord_count = rerecord_count;
    header.input_count = GetInputCount(recorded_input);

    std::string rev_bytes;
    CryptoPP::StringSource(Common::g_scm_rev, true,
                           new CryptoPP::HexDecoder(new CryptoPP::StringSink(rev_bytes)));
    std::memcpy(header.revision.data(), rev_bytes.data(), sizeof(CTMHeader::revision));

    save_record.WriteBytes(&header, sizeof(CTMHeader));
    save_record.WriteBytes(recorded_input.data(), recorded_input.size());

    if (!save_record.IsGood()) {
        LOG_ERROR(Movie, "Error saving movie");
    }
}

void Movie::SetPlaybackCompletionCallback(std::function<void()> completion_callback) {
    playback_completion_callback = completion_callback;
}

void Movie::StartPlayback(const std::string& movie_file) {
    LOG_INFO(Movie, "Loading Movie for playback");
    FileUtil::IOFile save_record(movie_file, "rb");
    const u64 size = save_record.GetSize();

    if (save_record.IsGood() && size > sizeof(CTMHeader)) {
        CTMHeader header;
        save_record.ReadArray(&header, 1);
        if (ValidateHeader(header) != ValidationResult::Invalid) {
            play_mode = PlayMode::Playing;
            record_movie_file = movie_file;

            std::array<char, 33> author{}; // Add a null terminator
            std::memcpy(author.data(), header.author.data(), header.author.size());
            record_movie_author = author.data();

            rerecord_count = header.rerecord_count;
            total_input = header.input_count;

            recorded_input.resize(size - sizeof(CTMHeader));
            save_record.ReadArray(recorded_input.data(), recorded_input.size());

            current_byte = 0;
            current_input = 0;
            id = header.id;
            program_id = header.program_id;

            tas_origin_ticks = system.IsPoweredOn() ? system.CoreTiming().GetTicks() : 0;
            if (tas) {
                // Play the whole movie as fast as possible to capture it into the TAS editor
                std::scoped_lock lock{tas_mutex};
                tas = std::make_unique<TasData>();
                system.frame_limiter.SetUnthrottled(true);
            }

            LOG_INFO(Movie, "Loaded Movie, ID: {:016X}", id);
        }
    } else {
        LOG_ERROR(Movie, "Failed to playback movie: Unable to open '{}'", movie_file);
    }
}

void Movie::StartRecording(const std::string& movie_file, const std::string& author) {
    play_mode = PlayMode::Recording;
    record_movie_file = movie_file;
    record_movie_author = author;
    rerecord_count = 1;
    tas_origin_ticks = system.IsPoweredOn() ? system.CoreTiming().GetTicks() : 0;

    if (tas) {
        std::scoped_lock lock{tas_mutex};
        tas = std::make_unique<TasData>();
        read_only = false;
    }

    // Generate a random ID
    CryptoPP::AutoSeededRandomPool rng;
    rng.GenerateBlock(reinterpret_cast<CryptoPP::byte*>(&id), sizeof(id));

    // Get program ID
    program_id = 0;
    system.GetAppLoader().ReadProgramId(program_id);

    LOG_INFO(Movie, "Enabling Movie recording, ID: {:016X}", id);
}

void Movie::SetReadOnly(bool read_only_) {
    read_only = read_only_;
}

static boost::optional<CTMHeader> ReadHeader(const std::string& movie_file) {
    FileUtil::IOFile save_record(movie_file, "rb");
    const u64 size = save_record.GetSize();

    if (!save_record || size <= sizeof(CTMHeader)) {
        return boost::none;
    }

    CTMHeader header;
    save_record.ReadArray(&header, 1);

    if (header_magic_bytes != header.filetype) {
        return boost::none;
    }

    return header;
}

void Movie::PrepareForPlayback(const std::string& movie_file) {
    auto header = ReadHeader(movie_file);
    if (header == boost::none)
        return;

    init_time = header.value().clock_init_time;
    base_ticks = header.value().timing_base_ticks;
}

void Movie::PrepareForRecording() {
    if (Settings::values.init_clock.GetValue() == Settings::InitClock::SystemTime) {
        long long init_time_offset = Settings::values.init_time_offset.GetValue();
        long long days_offset = init_time_offset / 86400;
        unsigned long long seconds_offset =
            std::abs(init_time_offset) - std::abs(days_offset * 86400);

        init_time =
            Common::Timer::GetTimeSinceJan1970().count() + seconds_offset + (days_offset * 86400);
    } else {
        init_time = Settings::values.init_time.GetValue();
    }

    base_ticks = Timing::GenerateBaseTicks();
}

Movie::ValidationResult Movie::ValidateMovie(const std::string& movie_file) const {
    LOG_INFO(Movie, "Validating Movie file '{}'", movie_file);

    FileUtil::IOFile save_record(movie_file, "rb");
    const u64 size = save_record.GetSize();

    if (!save_record || size <= sizeof(CTMHeader)) {
        return ValidationResult::Invalid;
    }

    CTMHeader header;
    save_record.ReadArray(&header, 1);

    if (header_magic_bytes != header.filetype) {
        return ValidationResult::Invalid;
    }

    auto result = ValidateHeader(header);
    if (result != ValidationResult::OK) {
        return result;
    }

    if (!header.input_count) { // Probably created by an older version.
        return ValidationResult::OK;
    }

    std::vector<u8> input(size - sizeof(header));
    save_record.ReadArray(input.data(), input.size());
    return ValidateInput(input, header.input_count);
}

Movie::MovieMetadata Movie::GetMovieMetadata(const std::string& movie_file) const {
    auto header = ReadHeader(movie_file);
    if (header == boost::none)
        return {};

    std::array<char, 33> author{}; // Add a null terminator
    std::memcpy(author.data(), header->author.data(), header->author.size());

    return {header->program_id, std::string{author.data()}, header->rerecord_count,
            header->input_count};
}

void Movie::Shutdown() {
    if (play_mode == PlayMode::Recording) {
        SaveMovie();
    }

    play_mode = PlayMode::None;
    if (tas) {
        std::scoped_lock lock{tas_mutex};
        tas = std::make_unique<TasData>();
    }
    recorded_input.resize(0);
    record_movie_file.clear();
    current_byte = 0;
    current_input = 0;
    init_time = 0;
    base_ticks = -1;
    id = 0;
}

template <typename... Targs>
void Movie::Handle(Targs&... Fargs) {
    if (play_mode == PlayMode::Playing) {
        ASSERT(current_byte + sizeof(ControllerState) <= recorded_input.size());
        const std::size_t position = current_byte;
        Play(Fargs...);
        if (tas) {
            // Capture the played back inputs into the TAS editor
            ControllerState state{};
            std::memcpy(&state, &recorded_input[position], sizeof(ControllerState));
            TasResolve(state);
        }
        CheckInputEnd();
    } else if (play_mode == PlayMode::Recording) {
        if (!tas) {
            Record(Fargs...);
            return;
        }
        // Record the live input, let the TAS editor replace it with the input of its table if it
        // has one for this frame, then apply the result to the input the game gets.
        const std::size_t position = current_byte;
        const u64 input = current_input;
        Record(Fargs...);
        ControllerState state{};
        std::memcpy(&state, &recorded_input[position], sizeof(ControllerState));
        state = TasResolve(state);
        std::memcpy(&recorded_input[position], &state, sizeof(ControllerState));
        current_byte = position;
        current_input = input;
        Play(Fargs...);
    }
}

void Movie::HandlePadAndCircleStatus(Service::HID::PadState& pad_state, s16& circle_pad_x,
                                     s16& circle_pad_y) {
    Handle(pad_state, circle_pad_x, circle_pad_y);
}

void Movie::HandleTouchStatus(Service::HID::TouchDataEntry& touch_data) {
    Handle(touch_data);
}

void Movie::HandleAccelerometerStatus(Service::HID::AccelerometerDataEntry& accelerometer_data) {
    Handle(accelerometer_data);
}

void Movie::HandleGyroscopeStatus(Service::HID::GyroscopeDataEntry& gyroscope_data) {
    Handle(gyroscope_data);
}

void Movie::HandleIrRst(Service::IR::PadState& pad_state, s16& c_stick_x, s16& c_stick_y) {
    Handle(pad_state, c_stick_x, c_stick_y);
}

void Movie::HandleExtraHidResponse(Service::IR::ExtraHIDResponse& extra_hid_response) {
    Handle(extra_hid_response);
}
// -------------------------------------------------------------------------------------------------
// TAS editor
// -------------------------------------------------------------------------------------------------

void Movie::EnableTasEditor(bool enable) {
    std::scoped_lock lock{tas_mutex};
    if (!enable) {
        tas.reset();
        return;
    }
    if (tas) {
        return;
    }
    tas = std::make_unique<TasData>();
    read_only = false;
    if (play_mode == PlayMode::Recording || play_mode == PlayMode::Playing) {
        // Frames emulated before now are not known
        const u64 frame = TasFrameNow();
        tas->first_frame = frame;
        tas->frames.resize(frame);
        for (auto& f : tas->frames) {
            f.known = false;
        }
    }
}

bool Movie::IsTasEditorEnabled() const {
    return tas != nullptr;
}

u64 Movie::TasFrameNow() const {
    if (!system.IsPoweredOn()) {
        return 0;
    }
    // Frames are counted from the start of the movie (the core timing base ticks)
    const s64 origin = base_ticks >= 0 ? base_ticks : tas_origin_ticks;
    const s64 ticks = system.CoreTiming().GetTicks() - origin;
    return ticks > 0 ? static_cast<u64>(ticks) / VideoCore::FRAME_TICKS : 0;
}

ControllerState Movie::TasResolve(const ControllerState& live) {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return live;
    }
    auto& data = *tas;
    const u64 frame_index = TasFrameNow();
    if (frame_index < data.first_frame) {
        return live;
    }

    auto& position = data.position;
    if (frame_index != position.frame) {
        // Entering a new frame
        position.frame = frame_index;
        position.cursors.fill(0);
        position.capturing = frame_index >= data.frames.size() || data.overwrite;
        if (position.capturing) {
            if (frame_index >= data.frames.size()) {
                data.frames.resize(frame_index + 1);
            }
            data.frames[frame_index] = TasData::Frame{};
            // The captured frame differs from what later states were made with
            TasInvalidateStatesFrom(frame_index + 1);
        }
    }

    const auto type = live.type;
    const auto type_index = static_cast<std::size_t>(type);
    const u32 cursor = type_index < NumStateTypes ? position.cursors[type_index]++ : 0;
    auto& frame = data.frames[frame_index];

    if (position.capturing) {
        frame.polls.push_back(live);
        if (!(frame.seen & TypeBit(type))) {
            // The first input of each type gives the values shown in the editor
            auto& v = frame.values;
            switch (type) {
            case ControllerStateType::PadAndCircle:
                v.buttons = live.pad_and_circle.hex & EditorButtonMask;
                v.circle_x = live.pad_and_circle.circle_pad_x;
                v.circle_y = live.pad_and_circle.circle_pad_y;
                break;
            case ControllerStateType::Touch:
                v.touch = live.touch.valid != 0;
                v.touch_x = live.touch.x;
                v.touch_y = live.touch.y;
                break;
            case ControllerStateType::Accelerometer:
                v.accel = {live.accelerometer.x, live.accelerometer.y, live.accelerometer.z};
                break;
            case ControllerStateType::Gyroscope:
                v.gyro = {live.gyroscope.x, live.gyroscope.y, live.gyroscope.z};
                break;
            case ControllerStateType::IrRst:
                v.zl = live.ir_rst.zl != 0;
                v.zr = live.ir_rst.zr != 0;
                v.c_stick_x = live.ir_rst.x;
                v.c_stick_y = live.ir_rst.y;
                break;
            case ControllerStateType::ExtraHidResponse:
                if (!(frame.seen & TypeBit(ControllerStateType::IrRst))) {
                    v.zl = !live.extra_hid_response.zl_not_held;
                    v.zr = !live.extra_hid_response.zr_not_held;
                    v.c_stick_x =
                        static_cast<s16>((static_cast<int>(live.extra_hid_response.c_stick_x) -
                                          ExtraHidCStickCenter) *
                                         CStickMax / ExtraHidCStickRadius);
                    v.c_stick_y =
                        static_cast<s16>((static_cast<int>(live.extra_hid_response.c_stick_y) -
                                          ExtraHidCStickCenter) *
                                         CStickMax / ExtraHidCStickRadius);
                }
                break;
            }
            frame.seen |= TypeBit(type);
        }
        return live;
    }

    // Replay the exact captured input of the frame, unless the frame was edited
    if (!(frame.edited & TypeBit(type))) {
        u32 count = 0;
        const ControllerState* last = nullptr;
        for (const auto& poll : frame.polls) {
            if (poll.type != type) {
                continue;
            }
            if (count++ == cursor) {
                return poll;
            }
            last = &poll;
        }
        if (last) {
            return *last;
        }
    }

    // Generate the input from the values of the frame
    const auto& v = frame.values;
    ControllerState state{};
    state.type = type;
    switch (type) {
    case ControllerStateType::PadAndCircle:
        // Keep the bits not shown in the editor (debug, gpio14) from the live input
        state.pad_and_circle.hex = static_cast<u16>((live.pad_and_circle.hex & ~EditorButtonMask) |
                                                    (v.buttons & EditorButtonMask));
        state.pad_and_circle.circle_pad_x = v.circle_x;
        state.pad_and_circle.circle_pad_y = v.circle_y;
        break;
    case ControllerStateType::Touch:
        state.touch.x = v.touch_x;
        state.touch.y = v.touch_y;
        state.touch.valid = v.touch ? 1 : 0;
        break;
    case ControllerStateType::Accelerometer:
        state.accelerometer.x = v.accel[0];
        state.accelerometer.y = v.accel[1];
        state.accelerometer.z = v.accel[2];
        break;
    case ControllerStateType::Gyroscope:
        state.gyroscope.x = v.gyro[0];
        state.gyroscope.y = v.gyro[1];
        state.gyroscope.z = v.gyro[2];
        break;
    case ControllerStateType::IrRst:
        state.ir_rst.x = v.c_stick_x;
        state.ir_rst.y = v.c_stick_y;
        state.ir_rst.zl = v.zl ? 1 : 0;
        state.ir_rst.zr = v.zr ? 1 : 0;
        break;
    case ControllerStateType::ExtraHidResponse: {
        state.extra_hid_response.hex = live.extra_hid_response.hex;
        state.extra_hid_response.zl_not_held.Assign(v.zl ? 0 : 1);
        state.extra_hid_response.zr_not_held.Assign(v.zr ? 0 : 1);
        const auto to_extra_hid = [](s16 value) {
            return static_cast<u32>(std::clamp(
                ExtraHidCStickCenter + value * ExtraHidCStickRadius / CStickMax, 0, 0xFFF));
        };
        state.extra_hid_response.c_stick_x.Assign(to_extra_hid(v.c_stick_x));
        state.extra_hid_response.c_stick_y.Assign(to_extra_hid(v.c_stick_y));
        break;
    }
    }
    return state;
}

void Movie::TasInvalidateStatesFrom(std::size_t frame) {
    // A state taken at the start of a frame is still valid if no input of the frame was read
    // before it was taken
    auto& states = tas->states;
    for (auto it = states.lower_bound(frame == 0 ? 0 : frame - 1); it != states.end();) {
        const bool before = it->first < frame;
        const bool clean =
            std::all_of(it->second.position.cursors.begin(), it->second.position.cursors.end(),
                        [](u32 c) { return c == 0; });
        if (before || (it->first == frame && clean)) {
            ++it;
        } else {
            it = states.erase(it);
        }
    }
}

std::size_t Movie::TasFrameCount() const {
    std::scoped_lock lock{tas_mutex};
    return tas ? tas->frames.size() : 0;
}

u64 Movie::TasFirstFrame() const {
    std::scoped_lock lock{tas_mutex};
    return tas ? tas->first_frame : 0;
}

Movie::TasFrame Movie::TasGetFrame(std::size_t index) const {
    std::scoped_lock lock{tas_mutex};
    if (!tas || index >= tas->frames.size()) {
        return {};
    }
    return tas->frames[index].values;
}

std::vector<Movie::TasFrame> Movie::TasGetFrames(std::size_t index, std::size_t count) const {
    std::scoped_lock lock{tas_mutex};
    std::vector<TasFrame> result;
    if (!tas) {
        return result;
    }
    for (std::size_t i = index; i < index + count && i < tas->frames.size(); ++i) {
        result.push_back(tas->frames[i].values);
    }
    return result;
}

bool Movie::TasIsFrameEdited(std::size_t index) const {
    std::scoped_lock lock{tas_mutex};
    return tas && index < tas->frames.size() && tas->frames[index].edited != 0;
}

bool Movie::TasIsFrameKnown(std::size_t index) const {
    std::scoped_lock lock{tas_mutex};
    return tas && index < tas->frames.size() && tas->frames[index].known;
}

void Movie::TasSetFrame(std::size_t index, const TasFrame& values) {
    std::scoped_lock lock{tas_mutex};
    if (!tas || index < tas->first_frame) {
        return;
    }
    if (index >= tas->frames.size()) {
        tas->frames.resize(index + 1);
    }
    auto& frame = tas->frames[index];
    const auto& old = frame.values;
    u8 changed = 0;
    if (values.buttons != old.buttons || values.circle_x != old.circle_x ||
        values.circle_y != old.circle_y) {
        changed |= TypeBit(ControllerStateType::PadAndCircle);
    }
    if (values.touch != old.touch || values.touch_x != old.touch_x ||
        values.touch_y != old.touch_y) {
        changed |= TypeBit(ControllerStateType::Touch);
    }
    if (values.accel != old.accel) {
        changed |= TypeBit(ControllerStateType::Accelerometer);
    }
    if (values.gyro != old.gyro) {
        changed |= TypeBit(ControllerStateType::Gyroscope);
    }
    if (values.zl != old.zl || values.zr != old.zr || values.c_stick_x != old.c_stick_x ||
        values.c_stick_y != old.c_stick_y) {
        changed |=
            TypeBit(ControllerStateType::IrRst) | TypeBit(ControllerStateType::ExtraHidResponse);
    }
    if (!changed) {
        return;
    }
    frame.values = values;
    frame.edited |= changed;
    frame.known = true;
    TasInvalidateStatesFrom(index);
}

void Movie::TasInsertFrames(std::size_t index, const std::vector<TasFrame>& values) {
    std::scoped_lock lock{tas_mutex};
    if (!tas || index < tas->first_frame || values.empty()) {
        return;
    }
    index = std::min(index, tas->frames.size());
    std::vector<TasData::Frame> frames(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        frames[i].values = values[i];
        frames[i].edited = AllTypes;
    }
    tas->frames.insert(tas->frames.begin() + index, frames.begin(), frames.end());
    TasInvalidateStatesFrom(index);
}

void Movie::TasDeleteFrames(std::size_t index, std::size_t count) {
    std::scoped_lock lock{tas_mutex};
    if (!tas || index < tas->first_frame || index >= tas->frames.size()) {
        return;
    }
    count = std::min(count, tas->frames.size() - index);
    tas->frames.erase(tas->frames.begin() + index, tas->frames.begin() + index + count);
    TasInvalidateStatesFrom(index);
}

u64 Movie::TasCurrentFrame() const {
    std::scoped_lock lock{tas_mutex};
    return tas ? tas->current_frame.load() : 0;
}

bool Movie::TasHasState(u64 frame) const {
    std::scoped_lock lock{tas_mutex};
    return tas && tas->states.contains(frame);
}

std::size_t Movie::TasStateCount() const {
    std::scoped_lock lock{tas_mutex};
    return tas ? tas->states.size() : 0;
}

std::size_t Movie::TasStateMemoryUsage() const {
    std::scoped_lock lock{tas_mutex};
    std::size_t total = 0;
    if (tas) {
        for (const auto& [frame, state] : tas->states) {
            total += state.data.size();
        }
    }
    return total;
}

void Movie::TasSetStateInterval(u32 frames) {
    std::scoped_lock lock{tas_mutex};
    if (tas) {
        tas->state_interval = std::max(frames, 1u);
    }
}

void Movie::TasSetStateCapacity(u32 states) {
    std::scoped_lock lock{tas_mutex};
    if (tas) {
        tas->state_capacity = std::max(states, 2u);
    }
}

void Movie::TasRequestSeek(u64 frame) {
    std::scoped_lock lock{tas_mutex};
    if (tas && play_mode == PlayMode::Recording) {
        tas->seek_request = std::max(frame, tas->first_frame);
    }
}

bool Movie::TasIsSeeking() const {
    std::scoped_lock lock{tas_mutex};
    return tas && (tas->seek_request || tas->seek_target);
}

void Movie::TasSetOverwrite(bool overwrite) {
    std::scoped_lock lock{tas_mutex};
    if (tas) {
        tas->overwrite = overwrite;
    }
}

std::optional<u64> Movie::TasTakeSeekRequest() {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return std::nullopt;
    }
    auto request = tas->seek_request;
    tas->seek_request.reset();
    return request;
}

std::optional<Movie::TasStateRef> Movie::TasFindState(u64 frame) const {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return std::nullopt;
    }
    auto it = tas->states.upper_bound(frame);
    if (it == tas->states.begin()) {
        return std::nullopt;
    }
    --it;
    return TasStateRef{it->first, it->second.data};
}

void Movie::TasRestoreStatePosition(u64 frame) {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return;
    }
    if (const auto it = tas->states.find(frame); it != tas->states.end()) {
        auto& position = tas->position;
        position = it->second.position;
        if (position.capturing && position.frame < tas->frames.size()) {
            if (position.frame + 1 < tas->frames.size()) {
                // The frame was fully captured after this state was taken, replay it
                position.capturing = false;
            } else {
                // Continue capturing the frame after the inputs read before the state
                auto& captured = tas->frames[position.frame];
                u32 consumed = 0;
                for (const u32 cursor : position.cursors) {
                    consumed += cursor;
                }
                if (consumed < captured.polls.size()) {
                    captured.polls.erase(captured.polls.begin() + consumed, captured.polls.end());
                }
                captured.seen = 0;
                for (const auto& poll : captured.polls) {
                    captured.seen |= TypeBit(poll.type);
                }
            }
        }
    }
    tas->current_frame = frame;
}

bool Movie::TasWantsState() const {
    std::scoped_lock lock{tas_mutex};
    // States are also kept while a movie is captured by playing it back, so that its frames can be
    // gone back to once it is edited
    if (!tas || (play_mode != PlayMode::Recording && play_mode != PlayMode::Playing)) {
        return false;
    }
    const u64 frame = TasFrameNow();
    return frame >= tas->first_frame && frame % tas->state_interval == 0 &&
           !tas->states.contains(frame);
}

void Movie::TasStoreState(std::vector<u8> state) {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return;
    }
    const u64 frame = TasFrameNow();
    auto& position = tas->position;
    TasData::Position saved = position;
    if (saved.frame != frame) {
        // No input of this frame was read yet
        saved = TasData::Position{};
    }
    tas->states[frame] = TasData::State{std::move(state), saved};

    // Over capacity: drop the state whose removal leaves the smallest gap, keeping the first
    auto& states = tas->states;
    while (states.size() > tas->state_capacity && states.size() > 2) {
        auto best = states.end();
        u64 best_gap = std::numeric_limits<u64>::max();
        for (auto it = std::next(states.begin()); std::next(it) != states.end(); ++it) {
            const u64 gap = std::next(it)->first - std::prev(it)->first;
            if (gap < best_gap) {
                best_gap = gap;
                best = it;
            }
        }
        if (best == states.end()) {
            break;
        }
        states.erase(best);
    }
}

void Movie::TasSetSeekTarget(std::optional<u64> frame) {
    std::scoped_lock lock{tas_mutex};
    if (tas) {
        tas->seek_target = frame;
    }
}

bool Movie::TasOnVBlank() {
    std::scoped_lock lock{tas_mutex};
    if (!tas) {
        return false;
    }
    const u64 frame = TasFrameNow();
    tas->current_frame = frame;
    if (tas->seek_target && frame >= *tas->seek_target) {
        tas->seek_target.reset();
        return true;
    }
    return false;
}

} // namespace Core
