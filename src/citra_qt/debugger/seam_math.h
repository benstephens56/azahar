// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>
#include "common/common_types.h"

/**
 * Floor check math of Ocarina of Time 3D (and the N64 games it is based on), used to find out
 * where "invisible seams" are: near-vertical triangles that the floor check accepts within 1 unit
 * (in XZ) of their vertices, where their plane extends far above or below the triangle.
 *
 * The arithmetic follows the game's (single precision, same order of operations), as reproduced by
 * exodus122's 3d_model_viewer (Math3D_TriChkPointParaYImpl and the plane height computation).
 */
namespace SeamMath {

/// Stored normals are scaled by 32767
constexpr float NormalFrac = 1.0f / 32767.0f;
/// Normal components below this are treated as zero by OoT3D (OoT uses 0.008)
constexpr float ZeroEpsilon = 0.00008f;
/// Leniency of the floor check in the XZ plane
constexpr float CheckDist = 1.0f;

inline bool IsZero(float f) {
    return std::fabs(f) < ZeroEpsilon;
}

struct Triangle {
    int index = -1; ///< Index in the collision file, -1 if entered manually
    std::array<std::array<s16, 3>, 3> vertices{};
    std::array<s16, 3> normal{};
    float dist = 0.0f;

    float Nx() const {
        return normal[0] * NormalFrac;
    }
    float Ny() const {
        return normal[1] * NormalFrac;
    }
    float Nz() const {
        return normal[2] * NormalFrac;
    }

    /// Whether the floor check can stand Link on this triangle at all (it faces up)
    bool IsStandable() const {
        return Ny() > 0.0f && !IsZero(Ny());
    }

    /// A wall (steeper than 60 degrees) that the floor check still accepts near its vertices
    bool IsSeam() const {
        return IsStandable() && Ny() <= 0.5f;
    }

    /// Height of the triangle's plane at (x, z), computed like the game does
    float HeightAt(float x, float z) const {
        const float nx = Nx();
        const float ny = Ny();
        const float nz = Nz();
        if (IsZero(ny)) {
            return 0.0f;
        }
        const float a = -nx * x;
        const float b = nz * z;
        const float c = a - b;
        const float e = c - dist;
        return e / ny;
    }

    /// How much the plane rises per unit moved in XZ, and the (unit) direction it rises in
    float RisePerUnit() const {
        return std::hypot(Nx(), Nz()) / Ny();
    }
    std::array<float, 2> UphillDirection() const {
        const float length = std::hypot(Nx(), Nz());
        if (length == 0.0f) {
            return {0.0f, 0.0f};
        }
        // The plane rises opposite to the horizontal part of its (upward facing) normal
        return {-Nx() / length, -Nz() / length};
    }

    /// The floor check's test of whether (x, z) is on the triangle, with its 1 unit of leniency
    /// (detMax 0 as used for scene collision)
    bool ContainsXZ(float x, float z) const {
        const float ny = Ny();
        if (IsZero(ny)) {
            return false;
        }
        const auto& v0 = vertices[0];
        const auto& v1 = vertices[1];
        const auto& v2 = vertices[2];
        const auto vx = [](const std::array<s16, 3>& v) { return static_cast<float>(v[0]); };
        const auto vz = [](const std::array<s16, 3>& v) { return static_cast<float>(v[2]); };

        // Bounding box grown by the check distance
        const float min_x = std::min({vx(v0), vx(v1), vx(v2)}) - CheckDist;
        const float max_x = std::max({vx(v0), vx(v1), vx(v2)}) + CheckDist;
        const float min_z = std::min({vz(v0), vz(v1), vz(v2)}) - CheckDist;
        const float max_z = std::max({vz(v0), vz(v1), vz(v2)}) + CheckDist;
        if (x < min_x || x > max_x || z < min_z || z > max_z) {
            return false;
        }

        // Within the check distance of a vertex
        constexpr float check_dist_sq = CheckDist * CheckDist;
        for (const auto& v : vertices) {
            const float dz = vz(v) - z;
            const float dx = vx(v) - x;
            if (dz * dz + dx * dx < check_dist_sq) {
                return true;
            }
        }

        // Inside the triangle
        const float det01 = (vz(v0) - z) * (vx(v1) - x) - (vx(v0) - x) * (vz(v1) - z);
        const float det12 = (vz(v1) - z) * (vx(v2) - x) - (vx(v1) - x) * (vz(v2) - z);
        const float det20 = (vz(v2) - z) * (vx(v0) - x) - (vx(v2) - x) * (vz(v0) - z);
        if ((det01 <= 0.0f && det12 <= 0.0f && det20 <= 0.0f) ||
            (det01 >= 0.0f && det12 >= 0.0f && det20 >= 0.0f)) {
            return true;
        }

        // Within the check distance of an edge, only for floors (not seams)
        if (std::fabs(ny) > 0.5f) {
            const auto near_edge = [&](const std::array<s16, 3>& a, const std::array<s16, 3>& b) {
                const float ez = vz(b) - vz(a);
                const float ex = vx(b) - vx(a);
                const float length_sq = ez * ez + ex * ex;
                if (length_sq == 0.0f) {
                    return false;
                }
                const float t = ((z - vz(a)) * ez + (x - vx(a)) * ex) / length_sq;
                if (t < 0.0f || t > 1.0f) {
                    return false;
                }
                const float pz = vz(a) + t * ez - z;
                const float px = vx(a) + t * ex - x;
                return pz * pz + px * px < check_dist_sq;
            };
            if (near_edge(v0, v1) || near_edge(v1, v2) || near_edge(v2, v0)) {
                return true;
            }
        }
        return false;
    }

    /// XZ distance from (x, z) to a vertex
    float VertexDistance(std::size_t vertex, float x, float z) const {
        return std::hypot(static_cast<float>(vertices[vertex][0]) - x,
                          static_cast<float>(vertices[vertex][2]) - z);
    }
};

/**
 * Finds the point closest to (x, z) where the plane of the triangle is at `height` and the floor
 * check accepts the triangle (within the check distance of a vertex). Returns nullopt if the
 * plane's line at that height doesn't pass through any vertex circle.
 */
inline std::optional<std::array<float, 2>> ClosestPointAtHeight(const Triangle& tri, float x,
                                                                float z, float height) {
    const float rise = tri.RisePerUnit();
    if (!std::isfinite(rise) || rise <= 0.0f) {
        return std::nullopt;
    }
    const auto [ux, uz] = tri.UphillDirection();
    // Closest point of the line at that height (all of it, not only the circles)
    const double along = (static_cast<double>(height) - tri.HeightAt(x, z)) / rise;
    const double px = x + ux * along;
    const double pz = z + uz * along;
    // Direction of the line (perpendicular to the uphill direction)
    const double wx = -uz;
    const double wz = ux;

    std::optional<std::array<float, 2>> best;
    double best_distance = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double cx = tri.vertices[i][0];
        const double cz = tri.vertices[i][2];
        // Part of the line inside this vertex's circle (slightly smaller, to stay inside)
        const double center_t = (cx - px) * wx + (cz - pz) * wz;
        const double off_x = px + wx * center_t - cx;
        const double off_z = pz + wz * center_t - cz;
        const double off_sq = off_x * off_x + off_z * off_z;
        const double radius = CheckDist * 0.999;
        if (off_sq >= radius * radius) {
            continue;
        }
        const double half = std::sqrt(radius * radius - off_sq);
        const double t = std::clamp(0.0, center_t - half, center_t + half);
        const double qx = px + wx * t;
        const double qz = pz + wz * t;
        const double distance = std::hypot(qx - x, qz - z);
        if (!best || distance < best_distance) {
            best = std::array<float, 2>{static_cast<float>(qx), static_cast<float>(qz)};
            best_distance = distance;
        }
    }
    return best;
}

/**
 * Reads the scene collision triangles of an OoT3D scene file (e.g. spot00_info.zsi, as extracted
 * from the game or included with exodus122's 3d_model_viewer). Intangible triangles are skipped.
 */
inline std::optional<std::vector<Triangle>> LoadZsi(std::span<const u8> data) {
    const auto read = [&data]<typename T>(std::size_t offset, T& value) {
        if (offset + sizeof(T) > data.size()) {
            return false;
        }
        std::memcpy(&value, data.data() + offset, sizeof(T));
        return true;
    };
    constexpr std::size_t FileOffset = 0x10;

    // Find the collision header command (0x03) in the scene header
    std::size_t command = FileOffset;
    u32 header = 0;
    for (;; command += 8) {
        u8 id = 0;
        if (!read(command, id) || id == 0x14) {
            return std::nullopt;
        }
        if (id == 0x03) {
            if (!read(command + 4, header)) {
                return std::nullopt;
            }
            break;
        }
    }
    const std::size_t header_offset = header + FileOffset;
    u16 num_vertices = 0;
    u16 num_polygons = 0;
    u32 vertex_list = 0;
    u32 polygon_list = 0;
    if (!read(header_offset + 0x0C, num_vertices) || !read(header_offset + 0x0E, num_polygons) ||
        !read(header_offset + 0x18, vertex_list) || !read(header_offset + 0x1C, polygon_list)) {
        return std::nullopt;
    }

    std::vector<std::array<s16, 3>> vertices(num_vertices);
    for (std::size_t i = 0; i < num_vertices; ++i) {
        if (!read(vertex_list + FileOffset + i * 6, vertices[i])) {
            return std::nullopt;
        }
    }

    std::vector<Triangle> triangles;
    for (std::size_t i = 0; i < num_polygons; ++i) {
        const std::size_t offset = polygon_list + FileOffset + i * 0x14;
        std::array<u16, 3> indices{};
        Triangle tri;
        tri.index = static_cast<int>(i);
        if (!read(offset + 0x2, indices) || !read(offset + 0xA, tri.normal) ||
            !read(offset + 0x10, tri.dist)) {
            return std::nullopt;
        }
        const u16 xp_flags = static_cast<u16>((indices[0] & 0xE000) >> 13);
        if (xp_flags & 2) {
            // Intangible
            continue;
        }
        bool valid = true;
        for (std::size_t v = 0; v < 3; ++v) {
            const u16 index = indices[v] & 0x1FFF;
            if (index >= vertices.size()) {
                valid = false;
                break;
            }
            tri.vertices[v] = vertices[index];
        }
        if (valid) {
            triangles.push_back(tri);
        }
    }
    return triangles;
}

} // namespace SeamMath
