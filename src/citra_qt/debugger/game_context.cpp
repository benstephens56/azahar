// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "citra_qt/debugger/game_context.h"
#include "core/core.h"
#include "core/memory_editor.h"

namespace GameContext {

namespace {

/// See include/z3D/z3Dactor.h and z3D.h of the OoT3D practice menu
constexpr GameLayout OoT3DLayout{
    .world_pos = 0x028,
    .prev_pos = 0x108,
    .world_rot_y = 0x036,
    .speed_xz = 0x06C,
    .floor_poly = 0x07C,
    .floor_height = 0x084,
    .bg_check_flags = 0x090,
    .main_camera = 0x364,
    .camera_global_context = 0x0D4,
    .camera_ptrs = 0xA54,
    .active_camera = 0xA64,
    .camera_input_yaw = 0x17E,
    .collision_header = 0xA98,
    .header_num_polygons = 0x0E,
    .header_poly_list = 0x1C,
    // actorCtx (0x208C) .actorList (0x0C) [ACTORTYPE_PLAYER] (2 * 8) .first (4)
    .player_actor = 0x208C + 0x0C + 2 * 8 + 4,
    // GameState.running
    .state_running = 0x101,
};

/**
 * See code/include/game/{actor,camera,context}.h of the MM3D practice tools. Fields they leave
 * as gaps are placed from the N64 decomp (zeldaret/mm): MM3D's structs match it, shifted by a few
 * bytes where the 3DS version adds fields.
 */
constexpr GameLayout MM3DLayout{
    .world_pos = 0x024,
    .prev_pos = 0x10C,
    .world_rot_y = 0x032,
    .speed_xz = 0x070,
    .floor_poly = 0x080,
    .floor_height = 0x088,
    .bg_check_flags = 0x094,
    .main_camera = 0x408,
    .camera_global_context = 0x0A0,
    .camera_ptrs = 0xA68,
    .active_camera = 0xA78,
    .camera_input_yaw = 0x14A,
    // After frameAdvCtx (0xAAC, 8 bytes), as in the N64 PlayState
    .collision_header = 0xAB4,
    .header_num_polygons = 0x10,
    .header_poly_list = 0x1C,
    // actorCtx.actorList (0x20C0) [ACTORTYPE_PLAYER] (2 * 0xC) .first (4)
    .player_actor = 0x20C0 + 2 * 0xC + 4,
    // State::status (game/states/state.h): Changing (0), Initialising or Running
    .state_running = 0x13C,
};

constexpr VAddr OffsetActorType = 0x002;
constexpr u8 ActorTypePlayer = 2;

/// How long to wait between searches for a GlobalContext that went missing
constexpr auto SearchInterval = std::chrono::seconds{3};

} // namespace

const GameLayout& LayoutOf(SeamMath::Game game) {
    return game == SeamMath::Game::MM3D ? MM3DLayout : OoT3DLayout;
}

Tracker& Tracker::Get(Core::System& system) {
    static Tracker tracker(system);
    return tracker;
}

Tracker::Tracker(Core::System& system_) : system{system_} {}

void Tracker::SetGame(SeamMath::Game new_game) {
    if (game != new_game) {
        game = new_game;
        ++generation;
    }
}

void Tracker::SetContext(std::optional<VAddr> address) {
    if (context != address) {
        context = address;
        ++generation;
    }
}

std::optional<u32> Tracker::ReadU32(VAddr address) const {
    const auto bytes = system.MemoryEditor().Peek(address, sizeof(u32));
    if (!bytes) {
        return std::nullopt;
    }
    u32 value;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value;
}

bool Tracker::IsValid(VAddr address, SeamMath::Game candidate) const {
    // The main camera points back to the GlobalContext it's part of, it's running, and it holds
    // Link's actor
    const GameLayout& layout = LayoutOf(candidate);
    if (ReadU32(address + layout.main_camera + layout.camera_global_context) != address) {
        return false;
    }
    // Still in use (an old one can look the same until its memory is reused)
    const auto running = system.MemoryEditor().Peek(address + layout.state_running, 1);
    if (!running || (*running)[0] == 0) {
        return false;
    }
    const auto player = ReadU32(address + layout.player_actor);
    if (!player || *player == 0) {
        return false;
    }
    const auto type = system.MemoryEditor().Peek(*player + OffsetActorType, 1);
    return type && (*type)[0] == ActorTypePlayer;
}

std::optional<VAddr> Tracker::Context() {
    if (context && IsValid(*context, game)) {
        return context;
    }
    // Only look for a new one after one was set or found, so nothing searches before that
    if (!context || !system.IsPoweredOn()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_search < SearchInterval) {
        return std::nullopt;
    }
    last_search = now;
    return Find();
}

std::optional<VAddr> Tracker::PlayerActor() {
    const auto address = Context();
    if (!address) {
        return std::nullopt;
    }
    const auto player = ReadU32(*address + LayoutOf(game).player_actor);
    if (!player || *player == 0) {
        return std::nullopt;
    }
    return player;
}

std::optional<VAddr> Tracker::Find() {
    // The GlobalContext is at the value of the one word that points to as many bytes before the
    // word itself as the main camera's pointer to it is into the GlobalContext
    auto snapshot = system.MemoryEditor().SnapshotWritableMemory(std::chrono::milliseconds{2000});
    if (!snapshot) {
        return std::nullopt;
    }
    const SeamMath::Game other =
        game == SeamMath::Game::MM3D ? SeamMath::Game::OoT3D : SeamMath::Game::MM3D;
    for (const auto candidate : {game, other}) {
        const GameLayout& layout = LayoutOf(candidate);
        const VAddr offset = layout.main_camera + layout.camera_global_context;
        for (const auto& region : *snapshot) {
            for (std::size_t i = 0; i + 4 <= region.data.size(); i += 4) {
                u32 value;
                std::memcpy(&value, region.data.data() + i, sizeof(value));
                if (value == 0 || region.base + i != value + offset) {
                    continue;
                }
                if (IsValid(value, candidate)) {
                    SetGame(candidate);
                    SetContext(value);
                    return value;
                }
            }
        }
    }
    return std::nullopt;
}

namespace {

/// Recursive descent parser and evaluator of address expressions (see EvaluateAddress)
class AddressParser {
public:
    AddressParser(const QString& text_,
                  const std::function<std::optional<VAddr>(const QString&)>& name_,
                  const std::function<std::optional<u32>(VAddr)>& read_u32_)
        : text{text_}, name{name_}, read_u32{read_u32_} {}

    /// The value, nullopt if the text is invalid; `evaluated` tells if it could be evaluated
    std::optional<VAddr> Parse(bool& evaluated) {
        ok = true;
        evaluated = true;
        const auto value = Expression();
        SkipSpaces();
        if (position != text.size()) {
            ok = false;
        }
        evaluated = available;
        return ok ? std::optional<VAddr>{value} : std::nullopt;
    }

private:
    void SkipSpaces() {
        while (position < text.size() && text[position].isSpace()) {
            ++position;
        }
    }

    VAddr Expression() {
        VAddr value = Term();
        while (ok) {
            SkipSpaces();
            if (position < text.size() && (text[position] == u'+' || text[position] == u'-')) {
                const bool add = text[position] == u'+';
                ++position;
                const VAddr term = Term();
                value = add ? value + term : value - term;
            } else {
                break;
            }
        }
        return value;
    }

    VAddr Term() {
        SkipSpaces();
        if (position >= text.size()) {
            ok = false;
            return 0;
        }
        if (text[position] == u'[') {
            ++position;
            const VAddr inner = Expression();
            SkipSpaces();
            if (position >= text.size() || text[position] != u']') {
                ok = false;
                return 0;
            }
            ++position;
            if (!ok || !available) {
                return 0;
            }
            const auto value = read_u32(inner);
            if (!value) {
                available = false;
                return 0;
            }
            return *value;
        }
        // A name or a hexadecimal number
        const qsizetype start = position;
        while (position < text.size() &&
               (text[position].isLetterOrNumber() || text[position] == u'_')) {
            ++position;
        }
        QString token = text.mid(start, position - start);
        if (token.isEmpty()) {
            ok = false;
            return 0;
        }
        if (token.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            token = token.mid(2);
        }
        bool is_number = false;
        const VAddr number = token.toUInt(&is_number, 16);
        if (is_number) {
            return number;
        }
        const auto value = name(token);
        if (!value) {
            // Unknown names make the text invalid; known ones may just be unavailable now
            const QString lower = token.toLower();
            if (lower != QStringLiteral("link") && lower != QStringLiteral("globalcontext")) {
                ok = false;
            }
            available = false;
            return 0;
        }
        return *value;
    }

    const QString& text;
    const std::function<std::optional<VAddr>(const QString&)>& name;
    const std::function<std::optional<u32>(VAddr)>& read_u32;
    qsizetype position = 0;
    bool ok = true;
    bool available = true;
};

} // namespace

std::optional<VAddr> EvaluateAddress(
    const QString& text, const std::function<std::optional<VAddr>(const QString&)>& name,
    const std::function<std::optional<u32>(VAddr)>& read_u32) {
    bool evaluated = false;
    const auto value = AddressParser(text, name, read_u32).Parse(evaluated);
    return evaluated ? value : std::nullopt;
}

bool IsValidAddress(const QString& text) {
    const std::function<std::optional<VAddr>(const QString&)> name =
        [](const QString& token) -> std::optional<VAddr> {
        const QString lower = token.toLower();
        if (lower == QStringLiteral("link") || lower == QStringLiteral("globalcontext")) {
            return 0;
        }
        return std::nullopt;
    };
    const std::function<std::optional<u32>(VAddr)> read_u32 = [](VAddr) {
        return std::optional<u32>{0};
    };
    bool evaluated = false;
    return AddressParser(text, name, read_u32).Parse(evaluated).has_value();
}

std::optional<VAddr> ResolveAddress(Core::System& system, const QString& text) {
    Tracker& tracker = Tracker::Get(system);
    const std::function<std::optional<VAddr>(const QString&)> name =
        [&tracker](const QString& token) -> std::optional<VAddr> {
        const QString lower = token.toLower();
        if (lower == QStringLiteral("link")) {
            return tracker.PlayerActor();
        }
        if (lower == QStringLiteral("globalcontext")) {
            return tracker.Context();
        }
        return std::nullopt;
    };
    const std::function<std::optional<u32>(VAddr)> read_u32 =
        [&system](VAddr address) -> std::optional<u32> {
        const auto bytes = system.MemoryEditor().Peek(address, sizeof(u32));
        if (!bytes) {
            return std::nullopt;
        }
        u32 value;
        std::memcpy(&value, bytes->data(), sizeof(value));
        return value;
    };
    return EvaluateAddress(text, name, read_u32);
}

bool IsRelativeAddress(const QString& text) {
    return text.contains(QStringLiteral("link"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("globalcontext"), Qt::CaseInsensitive) ||
           text.contains(u'[');
}

} // namespace GameContext
