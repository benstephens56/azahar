// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <bit>
#include <cstring>
#include <optional>
#include <vector>
#include <QString>
#include <QStringList>
#include "common/common_types.h"

// Helpers shared by the memory watch and memory search tools.
namespace MemoryTools {

enum class ValueType : int {
    U8 = 0,
    S8,
    U16,
    S16,
    U32,
    S32,
    Float,
};

inline QStringList TypeNames() {
    return {QStringLiteral("u8"),   QStringLiteral("s8"),  QStringLiteral("u16"),
            QStringLiteral("s16"),  QStringLiteral("u32"), QStringLiteral("s32"),
            QStringLiteral("float")};
}

inline std::size_t ValueSize(ValueType type) {
    switch (type) {
    case ValueType::U8:
    case ValueType::S8:
        return 1;
    case ValueType::U16:
    case ValueType::S16:
        return 2;
    default:
        return 4;
    }
}

/// Reads a little endian value of the type's size from `data` into the low bits of a u32
inline u32 RawFromBytes(ValueType type, const u8* data) {
    u32 raw = 0;
    std::memcpy(&raw, data, ValueSize(type));
    return raw;
}

inline std::vector<u8> BytesFromRaw(ValueType type, u32 raw) {
    std::vector<u8> bytes(ValueSize(type));
    std::memcpy(bytes.data(), &raw, bytes.size());
    return bytes;
}

/// Converts a raw value to a double for ordered comparisons (increased, greater than, ...)
inline double NumericValue(ValueType type, u32 raw) {
    switch (type) {
    case ValueType::U8:
        return static_cast<u8>(raw);
    case ValueType::S8:
        return static_cast<s8>(static_cast<u8>(raw));
    case ValueType::U16:
        return static_cast<u16>(raw);
    case ValueType::S16:
        return static_cast<s16>(static_cast<u16>(raw));
    case ValueType::U32:
        return raw;
    case ValueType::S32:
        return static_cast<s32>(raw);
    case ValueType::Float:
        return std::bit_cast<float>(raw);
    }
    return 0.0;
}

inline QString FormatValue(ValueType type, u32 raw, bool hex) {
    if (hex) {
        return QStringLiteral("0x%1").arg(raw, static_cast<int>(ValueSize(type) * 2), 16,
                                          QLatin1Char('0'));
    }
    switch (type) {
    case ValueType::U8:
    case ValueType::U16:
    case ValueType::U32:
        return QString::number(raw);
    case ValueType::S8:
    case ValueType::S16:
    case ValueType::S32:
        return QString::number(static_cast<qint64>(NumericValue(type, raw)));
    case ValueType::Float:
        return QString::number(std::bit_cast<float>(raw), 'g', 9);
    }
    return {};
}

/// Parses user input into a raw value. Accepts "0x" prefixed hex for every type (for floats the
/// hex digits are the raw bits).
inline std::optional<u32> ParseValue(ValueType type, QString text) {
    text = text.trimmed();
    bool ok = false;
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        const u32 raw = text.mid(2).toUInt(&ok, 16);
        if (!ok) {
            return std::nullopt;
        }
        return raw & (ValueSize(type) == 4 ? 0xFFFFFFFFu : (1u << (ValueSize(type) * 8)) - 1);
    }
    if (type == ValueType::Float) {
        const float value = text.toFloat(&ok);
        return ok ? std::optional<u32>{std::bit_cast<u32>(value)} : std::nullopt;
    }
    const qlonglong value = text.toLongLong(&ok, 10);
    if (!ok) {
        return std::nullopt;
    }
    switch (ValueSize(type)) {
    case 1:
        return static_cast<u8>(value);
    case 2:
        return static_cast<u16>(value);
    default:
        return static_cast<u32>(value);
    }
}

inline std::optional<u32> ParseAddress(QString text) {
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }
    bool ok = false;
    const u32 address = text.toUInt(&ok, 16);
    return ok ? std::optional<u32>{address} : std::nullopt;
}

inline QString FormatAddress(u32 address) {
    return QStringLiteral("%1").arg(address, 8, 16, QLatin1Char('0')).toUpper();
}

} // namespace MemoryTools
