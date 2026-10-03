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

/// The floor check casts its ray down from this far above the actor's previous Y (func_8002E2AC in
/// the OoT decomp, the same for child and adult Link). Floors below that start are candidates, and
/// the actor is put on the highest one if it's at or above the actor's current Y.
constexpr float FloorCheckHeight = 50.0f;

/// Angles of the game: 0x10000 per turn. Moving along yaw `y` adds (sin y, cos y) to (X, Z).
constexpr double Pi = 3.14159265358979323846;
inline double YawToRadians(double yaw) {
    return yaw * Pi / 32768.0;
}
inline u16 RadiansToYaw(double radians) {
    const long long yaw = std::llround(radians * 32768.0 / Pi);
    return static_cast<u16>(yaw & 0xFFFF);
}
/// Yaw of a direction in the XZ plane
inline u16 YawOf(double dx, double dz) {
    return RadiansToYaw(std::atan2(dx, dz));
}
/// Signed difference a - b of two yaws, in (-0x8000, 0x8000]
inline int YawDifference(u16 a, u16 b) {
    return static_cast<s16>(static_cast<u16>(a - b));
}

/// Angle of the control stick as the game computes it (Lib_GetControlStickData): 0 is up, and it
/// grows counterclockwise. Link's target yaw is the camera's input yaw plus this.
inline u16 StickAngle(double stick_x, double stick_y) {
    return RadiansToYaw(std::atan2(-stick_x, stick_y));
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
 * Finds the point closest to (x, z) where the floor check accepts the triangle (within the check
 * distance of a vertex) and its plane is between `min_height` and `max_height`. Returns (x, z)
 * itself if it already is, and nullopt if there is no such point.
 */
inline std::optional<std::array<float, 2>> ClosestPointInHeightRange(const Triangle& tri, float x,
                                                                     float z, float min_height,
                                                                     float max_height) {
    const double rise = tri.RisePerUnit();
    if (!std::isfinite(rise) || rise <= 0.0 || min_height > max_height) {
        return std::nullopt;
    }
    const auto [ux, uz] = tri.UphillDirection();
    // Positions along the uphill direction from (x, z) where the plane is at the two heights
    const double height_here = tri.HeightAt(x, z);
    const double s_min = (min_height - height_here) / rise;
    const double s_max = (max_height - height_here) / rise;
    const auto along = [&](double px, double pz) { return (px - x) * ux + (pz - z) * uz; };
    // Slightly smaller than the check distance, to stay inside despite rounding
    const double radius = CheckDist * 0.999;

    std::optional<std::array<float, 2>> best;
    double best_distance = 0.0;
    const auto consider = [&](double px, double pz) {
        const double distance = std::hypot(px - x, pz - z);
        if (!best || distance < best_distance) {
            best = std::array<float, 2>{static_cast<float>(px), static_cast<float>(pz)};
            best_distance = distance;
        }
    };

    for (std::size_t i = 0; i < 3; ++i) {
        const double cx = tri.vertices[i][0];
        const double cz = tri.vertices[i][2];
        const auto in_disc = [&](double px, double pz) {
            return std::hypot(px - cx, pz - cz) < radius;
        };
        const auto in_strip = [&](double px, double pz) {
            const double s = along(px, pz);
            return s >= s_min && s <= s_max;
        };

        // (x, z) moved straight into the band
        const double s = std::clamp(0.0, s_min, s_max);
        if (in_disc(x + ux * s, z + uz * s)) {
            consider(x + ux * s, z + uz * s);
        }
        // The closest point of the circle, if it's in the band
        const double to_center = std::hypot(cx - x, cz - z);
        if (to_center > radius) {
            const double px = cx + (x - cx) * radius / to_center;
            const double pz = cz + (z - cz) * radius / to_center;
            if (in_strip(px, pz)) {
                consider(px, pz);
            }
        }
        // The closest points of the band's two edges inside the circle
        for (const double edge : {s_min, s_max}) {
            const double lx = x + ux * edge;
            const double lz = z + uz * edge;
            // Direction of the edge (perpendicular to the uphill direction)
            const double wx = -uz;
            const double wz = ux;
            const double center_t = (cx - lx) * wx + (cz - lz) * wz;
            const double off_x = lx + wx * center_t - cx;
            const double off_z = lz + wz * center_t - cz;
            const double off_sq = off_x * off_x + off_z * off_z;
            if (off_sq >= radius * radius) {
                continue;
            }
            const double half = std::sqrt(radius * radius - off_sq);
            const double t = std::clamp(0.0, center_t - half, center_t + half);
            consider(lx + wx * t, lz + wz * t);
        }
    }
    return best;
}

/// Where Link can step onto a seam from a floor: the seam is at the floor's height there
struct MountSpot {
    std::array<float, 2> point; ///< X, Z
    float floor_height;         ///< Height of the floor (and the seam) there
    int floor_index;            ///< Index of the floor triangle
    int vertex;                 ///< Seam vertex whose circle the spot is in
};

/**
 * Finds the spots where the seam's plane meets the plane of a floor Link can stand on, within the
 * seam's vertex circles and on the floor (with the floor check's leniency), where that floor is
 * the highest one Link would stand on. Each spot is the middle of such a stretch, so it only
 * depends on the collision. Returns the spot closest to (x, z).
 */
inline std::optional<MountSpot> FindMountSpot(const Triangle& seam,
                                              std::span<const Triangle> collision, float x,
                                              float z) {
    if (!seam.IsSeam()) {
        return std::nullopt;
    }
    const double s_nx = seam.Nx() / seam.Ny();
    const double s_nz = seam.Nz() / seam.Ny();
    const double s_d = seam.dist / seam.Ny();
    const double radius = CheckDist * 0.999;

    std::optional<MountSpot> best;
    double best_distance = 0.0;
    for (int vertex = 0; vertex < 3; ++vertex) {
        const double cx = seam.vertices[vertex][0];
        const double cz = seam.vertices[vertex][2];
        // Floors near this vertex's circle
        std::vector<const Triangle*> floors;
        for (const auto& tri : collision) {
            if (tri.Ny() <= 0.5f || &tri == &seam) {
                continue;
            }
            const auto [min_x, max_x] =
                std::minmax({tri.vertices[0][0], tri.vertices[1][0], tri.vertices[2][0]});
            const auto [min_z, max_z] =
                std::minmax({tri.vertices[0][2], tri.vertices[1][2], tri.vertices[2][2]});
            if (cx + 2.0 < min_x || cx - 2.0 > max_x || cz + 2.0 < min_z || cz - 2.0 > max_z) {
                continue;
            }
            floors.push_back(&tri);
        }
        for (const Triangle* floor : floors) {
            // Seam height = floor height along the line a * x + b * z = c
            const double a = -s_nx + floor->Nx() / floor->Ny();
            const double b = -s_nz + floor->Nz() / floor->Ny();
            const double c = s_d - floor->dist / floor->Ny();
            const double length = std::hypot(a, b);
            if (length == 0.0) {
                continue;
            }
            const double offset = (a * cx + b * cz - c) / length;
            if (std::fabs(offset) >= radius) {
                continue;
            }
            // Chord of the line inside the circle, sampled for the part on this floor (and where
            // it's the floor Link would be on: no other floor up to 50 above it)
            const double foot_x = cx - a / length * offset;
            const double foot_z = cz - b / length * offset;
            const double dir_x = -b / length;
            const double dir_z = a / length;
            const double half = std::sqrt(radius * radius - offset * offset);
            constexpr int Samples = 64;
            int run_start = -1;
            int best_start = -1;
            int best_end = -1;
            for (int i = 0; i <= Samples + 1; ++i) {
                bool ok = false;
                if (i <= Samples) {
                    const double t = -half + 2.0 * half * i / Samples;
                    const float px = static_cast<float>(foot_x + dir_x * t);
                    const float pz = static_cast<float>(foot_z + dir_z * t);
                    ok = floor->ContainsXZ(px, pz);
                    if (ok) {
                        const float height = floor->HeightAt(px, pz);
                        for (const Triangle* other : floors) {
                            if (other != floor && other->ContainsXZ(px, pz)) {
                                const float other_height = other->HeightAt(px, pz);
                                if (other_height > height + 0.01f &&
                                    other_height < height + FloorCheckHeight) {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (ok && run_start < 0) {
                    run_start = i;
                } else if (!ok && run_start >= 0) {
                    if (i - 1 - run_start > best_end - best_start) {
                        best_start = run_start;
                        best_end = i - 1;
                    }
                    run_start = -1;
                }
            }
            if (best_start < 0) {
                continue;
            }
            const double t = -half + 2.0 * half * ((best_start + best_end) / 2.0) / Samples;
            const float px = static_cast<float>(foot_x + dir_x * t);
            const float pz = static_cast<float>(foot_z + dir_z * t);
            const double distance = std::hypot(px - x, pz - z);
            if (!best || distance < best_distance) {
                best = MountSpot{{px, pz}, floor->HeightAt(px, pz), floor->index, vertex};
                best_distance = distance;
            }
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
