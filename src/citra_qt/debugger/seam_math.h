// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
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

/**
 * How far a straight line from (x, z) along the (unit) direction stays where the floor check
 * accepts the seam: inside the triangle or the union of its vertex circles.
 */
inline double StraightLineLength(const Triangle& tri, double x, double z, double dir_x,
                                 double dir_z) {
    const double radius = CheckDist * 0.999;
    std::array<std::array<double, 2>, 4> intervals{};
    std::size_t count = 0;
    for (const auto& v : tri.vertices) {
        // |P + t D - C|^2 = r^2
        const double px = x - v[0];
        const double pz = z - v[2];
        const double b = dir_x * px + dir_z * pz;
        const double c = px * px + pz * pz - radius * radius;
        const double disc = b * b - c;
        if (disc < 0.0) {
            continue;
        }
        const double root = std::sqrt(disc);
        const double t_out = -b + root;
        if (t_out <= 0.0) {
            continue;
        }
        intervals[count++] = {-b - root, t_out};
    }

    // The triangle itself: the part of the line on the inner side of all three edges (kept a
    // little inside them, against rounding)
    constexpr double Margin = 0.001;
    const auto vx = [&](std::size_t i) { return static_cast<double>(tri.vertices[i][0]); };
    const auto vz = [&](std::size_t i) { return static_cast<double>(tri.vertices[i][2]); };
    const double area = (vx(1) - vx(0)) * (vz(2) - vz(0)) - (vz(1) - vz(0)) * (vx(2) - vx(0));
    if (area != 0.0) {
        const double orientation = area > 0.0 ? 1.0 : -1.0;
        double lo = -std::numeric_limits<double>::infinity();
        double hi = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < 3 && lo <= hi; ++i) {
            const std::size_t j = (i + 1) % 3;
            const double ex = vx(j) - vx(i);
            const double ez = vz(j) - vz(i);
            const double length = std::hypot(ex, ez);
            // Signed distance from the edge, inside positive: f0 + t * f1
            const double f0 = orientation * (ex * (z - vz(i)) - ez * (x - vx(i))) / length - Margin;
            const double f1 = orientation * (ex * dir_z - ez * dir_x) / length;
            if (f1 == 0.0) {
                if (f0 < 0.0) {
                    hi = -std::numeric_limits<double>::infinity();
                }
            } else if (f1 > 0.0) {
                lo = std::max(lo, -f0 / f1);
            } else {
                hi = std::min(hi, -f0 / f1);
            }
        }
        if (lo <= hi && hi > 0.0) {
            intervals[count++] = {lo, hi};
        }
    }

    std::sort(intervals.begin(), intervals.begin() + count);
    double reach = 0.0;
    bool started = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (intervals[i][0] > reach + 1e-9) {
            break;
        }
        reach = std::max(reach, intervals[i][1]);
        started = true;
    }
    return started ? reach : 0.0;
}

/// A straight line to walk along a seam
struct ClimbLine {
    u16 yaw;       ///< Direction
    double length; ///< How far it stays on the seam
    double gain;   ///< Height gained over that length
};

/// Every direction from (x, z) that climbs the seam, with how far it stays on it
inline std::vector<ClimbLine> AllClimbLines(const Triangle& tri, double x, double z) {
    std::vector<ClimbLine> lines;
    const double rise = tri.RisePerUnit();
    if (!std::isfinite(rise) || rise <= 0.0) {
        return lines;
    }
    const auto [ux, uz] = tri.UphillDirection();
    for (int yaw = 0; yaw < 0x10000; ++yaw) {
        const double angle = YawToRadians(yaw);
        const double dx = std::sin(angle);
        const double dz = std::cos(angle);
        const double rise_along = rise * (dx * ux + dz * uz);
        if (rise_along <= 0.0) {
            continue;
        }
        const double length = StraightLineLength(tri, x, z, dx, dz);
        if (length > 0.0) {
            lines.push_back(ClimbLine{static_cast<u16>(yaw), length, rise_along * length});
        }
    }
    return lines;
}

/// Each line's gain, as the worst of the directions within `tolerance` of it
inline std::vector<double> RobustGains(std::span<const ClimbLine> lines, int tolerance) {
    std::vector<double> gain_by_yaw(0x10000, 0.0);
    for (const auto& line : lines) {
        gain_by_yaw[line.yaw] = line.gain;
    }
    std::vector<double> robust(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        double worst = lines[i].gain;
        for (int off = -tolerance; off <= tolerance; ++off) {
            worst = std::min(worst, gain_by_yaw[static_cast<u16>(lines[i].yaw + off)]);
        }
        robust[i] = worst;
    }
    return robust;
}

/**
 * Among `lines`, the one that climbs the most where the seam rises at most `max_rise` per unit
 * walked (so that Link, moving a given distance per frame, gains under 50 per frame). Each line is
 * judged by the worst of the directions within `tolerance` of it, so that it still works when Link
 * goes slightly off it (the best lines can be right next to ones crossing a gap in the seam). Of
 * the lines gaining almost as much (99%), the steepest one still within `max_rise`, which climbs
 * the fastest.
 */
inline std::optional<ClimbLine> PickClimbLine(std::span<const ClimbLine> lines, double max_rise,
                                              int tolerance = 8) {
    const auto rise_of = [](const ClimbLine& line) { return line.gain / line.length; };
    const auto robust = RobustGains(lines, tolerance);
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (rise_of(lines[i]) <= max_rise && (!best || robust[i] > robust[*best])) {
            best = i;
        }
    }
    if (!best || robust[*best] <= 0.0) {
        return std::nullopt;
    }
    std::size_t steepest = *best;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const double rise = rise_of(lines[i]);
        if (robust[i] >= 0.99 * robust[*best] && rise <= max_rise &&
            rise > rise_of(lines[steepest])) {
            steepest = i;
        }
    }
    return lines[steepest];
}

/// Walking a straight line frame by frame, as the game computes it
struct ClimbSimulation {
    int frames = 0;          ///< Frames Link stays on the seam
    float worst_gain = 0.0f; ///< Most height gained in a frame
    float end_height = 0.0f; ///< Height at the last frame on the seam
    bool falls = false;      ///< A frame gains 50 or more: Link falls off there
};

/**
 * Walks Link from (x, z) at height y along `yaw`, `step` per frame, in the game's float math. On
 * steep seams, heights are only computed to a unit or two, so the gain per frame isn't constant
 * and its worst frame is what matters.
 */
inline ClimbSimulation SimulateClimb(const Triangle& tri, float x, float z, float y, u16 yaw,
                                     double step) {
    ClimbSimulation result;
    result.end_height = y;
    const double angle = YawToRadians(yaw);
    const float step_x = static_cast<float>(step * std::sin(angle));
    const float step_z = static_cast<float>(step * std::cos(angle));
    // Long enough for any seam, in case a line never leaves one
    for (int frame = 0; frame < 100000; ++frame) {
        x += step_x;
        z += step_z;
        if (!tri.ContainsXZ(x, z)) {
            break;
        }
        const float height = tri.HeightAt(x, z);
        const float gain = height - result.end_height;
        if (gain >= FloorCheckHeight) {
            result.falls = true;
            result.worst_gain = gain;
            break;
        }
        result.worst_gain = std::max(result.worst_gain, gain);
        // Link stays at his height when the seam goes down a little under him
        result.end_height = std::max(result.end_height, height);
        ++result.frames;
    }
    return result;
}

/**
 * Among `lines` from (x, z) at height y, the one that climbs the most when walked `step` per frame
 * without any frame gaining more than `max_gain` (checked frame by frame), judged like
 * PickClimbLine. Of the lines climbing almost as much (99%), the steepest one.
 */
inline std::optional<ClimbLine> PickSimulatedClimbLine(const Triangle& tri,
                                                       std::span<const ClimbLine> lines, float x,
                                                       float z, float y, double step,
                                                       double max_gain, int tolerance = 8) {
    const auto rise_of = [](const ClimbLine& line) { return line.gain / line.length; };
    const auto robust = RobustGains(lines, tolerance);
    // Candidates: lines that can't gain much more per frame than allowed (as the gain per frame
    // varies by a unit or two), and that climb about as much as the best of those
    const double max_rise = (max_gain + 3.0) / step;
    double best_robust = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (rise_of(lines[i]) <= max_rise) {
            best_robust = std::max(best_robust, robust[i]);
        }
    }
    struct Candidate {
        std::size_t index;
        double gain;
    };
    std::vector<Candidate> valid;
    double best_gain = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (rise_of(lines[i]) > max_rise || robust[i] < 0.9 * best_robust) {
            continue;
        }
        const auto simulation = SimulateClimb(tri, x, z, y, lines[i].yaw, step);
        if (simulation.falls || simulation.worst_gain > max_gain || simulation.frames == 0) {
            continue;
        }
        const double gain = simulation.end_height - y;
        valid.push_back({i, gain});
        best_gain = std::max(best_gain, gain);
    }
    std::optional<std::size_t> steepest;
    for (const auto& candidate : valid) {
        if (candidate.gain >= 0.99 * best_gain &&
            (!steepest || rise_of(lines[candidate.index]) > rise_of(lines[*steepest]))) {
            steepest = candidate.index;
        }
    }
    if (!steepest || best_gain <= 0.0) {
        return std::nullopt;
    }
    return lines[*steepest];
}

/**
 * The straight line from (x, z) that gains the most height before leaving the seam, among the
 * directions where the seam rises at most `max_rise` per unit walked (see PickClimbLine).
 */
inline std::optional<ClimbLine> BestClimbLine(
    const Triangle& tri, double x, double z,
    double max_rise = std::numeric_limits<double>::infinity()) {
    return PickClimbLine(AllClimbLines(tri, x, z), max_rise);
}

/**
 * How far (in height) a floor may be from Link's for spots to get onto a seam from to count as on
 * his level, when asked for. Floors further away are other levels (like rooms below), which he
 * can't walk to.
 */
constexpr float LevelTolerance = 100.0f;

/// Link's (adult) wall check: a sphere this far up from his feet, with this radius (Player)
constexpr float WallCheckHeight = 26.0f;
constexpr float WallCheckRadius = 18.0f;

/// Triangles near a seam that act as walls for Link (neither floors nor ceilings), but the seam
inline std::vector<const Triangle*> WallsNear(const Triangle& seam,
                                              std::span<const Triangle> collision) {
    const auto [seam_min_x, seam_max_x] =
        std::minmax({seam.vertices[0][0], seam.vertices[1][0], seam.vertices[2][0]});
    const auto [seam_min_z, seam_max_z] =
        std::minmax({seam.vertices[0][2], seam.vertices[1][2], seam.vertices[2][2]});
    const float margin = WallCheckRadius + 2.0f;
    std::vector<const Triangle*> walls;
    for (const auto& tri : collision) {
        if (&tri == &seam || tri.Ny() > 0.5f || tri.Ny() < -0.8f) {
            continue;
        }
        const auto [min_x, max_x] =
            std::minmax({tri.vertices[0][0], tri.vertices[1][0], tri.vertices[2][0]});
        const auto [min_z, max_z] =
            std::minmax({tri.vertices[0][2], tri.vertices[1][2], tri.vertices[2][2]});
        if (seam_max_x + margin < min_x || seam_min_x - margin > max_x ||
            seam_max_z + margin < min_z || seam_min_z - margin > max_z) {
            continue;
        }
        walls.push_back(&tri);
    }
    return walls;
}

/**
 * Whether (a, b) is on a triangle with corners (a0, b0), (a1, b1), (a2, b2) projected along an
 * axis: within `check_dist` of a corner, "inside" with a determinant leniency of `det_max`, or
 * within `check_dist` of an edge when the normal's part along that axis is over 0.5
 * (Math3D_TriChkPointPara*Impl)
 */
inline bool TriChkPointPara(const std::array<std::array<float, 2>, 3>& v, float a, float b,
                            float det_max, float check_dist, float normal_part) {
    const float min_a = std::min({v[0][0], v[1][0], v[2][0]}) - check_dist;
    const float max_a = std::max({v[0][0], v[1][0], v[2][0]}) + check_dist;
    const float min_b = std::min({v[0][1], v[1][1], v[2][1]}) - check_dist;
    const float max_b = std::max({v[0][1], v[1][1], v[2][1]}) + check_dist;
    if (a < min_a || a > max_a || b < min_b || b > max_b) {
        return false;
    }
    const float check_dist_sq = check_dist * check_dist;
    for (const auto& c : v) {
        if ((c[0] - a) * (c[0] - a) + (c[1] - b) * (c[1] - b) < check_dist_sq) {
            return true;
        }
    }
    const auto det = [&](const std::array<float, 2>& p, const std::array<float, 2>& q) {
        return (p[0] - a) * (q[1] - b) - (p[1] - b) * (q[0] - a);
    };
    const float d01 = det(v[0], v[1]);
    const float d12 = det(v[1], v[2]);
    const float d20 = det(v[2], v[0]);
    if ((d01 <= det_max && d12 <= det_max && d20 <= det_max) ||
        (d01 >= -det_max && d12 >= -det_max && d20 >= -det_max)) {
        return true;
    }
    if (std::fabs(normal_part) > 0.5f) {
        for (int i = 0; i < 3; ++i) {
            const auto& p = v[i];
            const auto& q = v[(i + 1) % 3];
            const float ea = q[0] - p[0];
            const float eb = q[1] - p[1];
            const float length_sq = ea * ea + eb * eb;
            if (length_sq == 0.0f) {
                continue;
            }
            const float t = ((a - p[0]) * ea + (b - p[1]) * eb) / length_sq;
            if (t < 0.0f || t > 1.0f) {
                continue;
            }
            const float da = p[0] + t * ea - a;
            const float db = p[1] + t * eb - b;
            if (da * da + db * db < check_dist_sq) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Whether one of the walls pushes Link away when he stands at (x, z) with his feet at height y,
 * so he can't stand there: the game's wall check (BgCheck_SphVsStaticWall) of his wall check
 * sphere, which finds walls along Z and along X with some leniency.
 */
inline bool WallPushes(std::span<const Triangle* const> walls, float x, float y, float z) {
    const float cy = y + WallCheckHeight;
    const float radius = WallCheckRadius;
    for (const Triangle* wall : walls) {
        const auto& v = wall->vertices;
        if (cy < v[0][1] && cy < v[1][1] && cy < v[2][1]) {
            continue;
        }
        const float nx = wall->Nx();
        const float ny = wall->Ny();
        const float nz = wall->Nz();
        const float plane_dist = nx * x + ny * cy + nz * z + wall->dist;
        const float normal_xz = std::sqrt(nx * nx + nz * nz);
        if (std::fabs(plane_dist) > radius || normal_xz == 0.0f) {
            continue;
        }
        // Along Z: walls facing mostly along Z, at (x, y) in their XY projection
        const float along_z = std::fabs(nz) / normal_xz;
        if (along_z >= 0.4f && !IsZero(nz)) {
            const float min_z = std::min({v[0][2], v[1][2], v[2][2]}) - radius;
            const float max_z = std::max({v[0][2], v[1][2], v[2][2]}) + radius;
            const std::array<std::array<float, 2>, 3> projected{{
                {static_cast<float>(v[0][0]), static_cast<float>(v[0][1])},
                {static_cast<float>(v[1][0]), static_cast<float>(v[1][1])},
                {static_cast<float>(v[2][0]), static_cast<float>(v[2][1])},
            }};
            if (z >= min_z && z <= max_z && TriChkPointPara(projected, x, cy, 300.0f, 1.0f, nz)) {
                const float intersect = (-nx * x - ny * cy - wall->dist) / nz;
                const float distance = intersect - z;
                if (std::fabs(distance) <= radius / along_z && distance * nz <= 4.0f) {
                    return true;
                }
            }
        }
        // Along X: walls facing mostly along X, at (y, z) in their YZ projection
        const float along_x = std::fabs(nx) / normal_xz;
        if (along_x >= 0.4f && !IsZero(nx)) {
            const float min_x = std::min({v[0][0], v[1][0], v[2][0]}) - radius;
            const float max_x = std::max({v[0][0], v[1][0], v[2][0]}) + radius;
            const std::array<std::array<float, 2>, 3> projected{{
                {static_cast<float>(v[0][1]), static_cast<float>(v[0][2])},
                {static_cast<float>(v[1][1]), static_cast<float>(v[1][2])},
                {static_cast<float>(v[2][1]), static_cast<float>(v[2][2])},
            }};
            if (x >= min_x && x <= max_x && TriChkPointPara(projected, cy, z, 300.0f, 1.0f, nx)) {
                const float intersect = (-ny * cy - nz * z - wall->dist) / nx;
                const float distance = intersect - x;
                if (std::fabs(distance) <= radius / along_x && distance * nx <= 4.0f) {
                    return true;
                }
            }
        }
    }
    return false;
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
                                              std::span<const Triangle> collision, float x, float z,
                                              std::optional<float> near_height = std::nullopt,
                                              float level_tolerance = LevelTolerance) {
    if (!seam.IsSeam()) {
        return std::nullopt;
    }
    const double s_nx = seam.Nx() / seam.Ny();
    const double s_nz = seam.Nz() / seam.Ny();
    const double s_d = seam.dist / seam.Ny();
    const double radius = CheckDist * 0.999;
    const auto walls = WallsNear(seam, collision);

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
                    ok = floor->ContainsXZ(px, pz) &&
                         (!near_height ||
                          std::fabs(floor->HeightAt(px, pz) - *near_height) <= level_tolerance) &&
                         !WallPushes(walls, px, floor->HeightAt(px, pz), pz);
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
            // Positions are floats, and on a steep seam each step of X or Z can change its
            // height by about a unit, so it's rarely exactly at the floor's height: take the spot
            // in the run where it's closest above, nearest the run's middle
            const double t_start = -half + 2.0 * half * best_start / Samples;
            const double t_end = -half + 2.0 * half * best_end / Samples;
            const double t_middle = (t_start + t_end) / 2.0;
            float px = static_cast<float>(foot_x + dir_x * t_middle);
            float pz = static_cast<float>(foot_z + dir_z * t_middle);
            std::optional<float> best_gap;
            double best_from_middle = 0.0;
            constexpr int Steps = 256;
            for (int i = 0; i <= Steps; ++i) {
                const double t = t_start + (t_end - t_start) * i / Steps;
                const float sx = static_cast<float>(foot_x + dir_x * t);
                const float sz = static_cast<float>(foot_z + dir_z * t);
                const float gap = seam.HeightAt(sx, sz) - floor->HeightAt(sx, sz);
                const double from_middle = std::fabs(t - t_middle);
                if (gap < 0.0f || !seam.ContainsXZ(sx, sz)) {
                    continue;
                }
                if (!best_gap || gap < *best_gap - 0.01f ||
                    (gap < *best_gap + 0.01f && from_middle < best_from_middle)) {
                    px = sx;
                    pz = sz;
                    best_gap = gap;
                    best_from_middle = from_middle;
                }
            }
            const double distance = std::hypot(px - x, pz - z);
            if (!best || distance < best_distance) {
                best = MountSpot{{px, pz}, floor->HeightAt(px, pz), floor->index, vertex};
                best_distance = distance;
            }
        }
    }
    return best;
}

/// A spot to get onto a seam from, and the straight line to climb from there
struct MountClimb {
    MountSpot spot;
    ClimbLine line;
};

/**
 * The best straight line from (x, z) among the directions where the seam rises at most
 * `max_rise` per unit, searching coarsely through that range of directions and then around the
 * best one. Faster than BestClimbLine, for searching many starting points.
 */
inline std::optional<ClimbLine> QuickClimbLine(const Triangle& tri, double x, double z,
                                               double max_rise, int coarse_steps = 96) {
    const double rise = tri.RisePerUnit();
    if (!std::isfinite(rise) || rise <= 0.0) {
        return std::nullopt;
    }
    const auto [ux, uz] = tri.UphillDirection();
    const u16 uphill = YawOf(ux, uz);
    // Directions climbing at most max_rise: at least this far from straight uphill
    const int min_off =
        std::min<int>(0x3FFF, static_cast<int>(std::ceil(std::acos(std::min(1.0, max_rise / rise)) /
                                                         (2.0 * std::numbers::pi) * 0x10000)));
    const auto line = [&](int offset) {
        const u16 yaw = static_cast<u16>(uphill + offset);
        const double angle = YawToRadians(yaw);
        const double dx = std::sin(angle);
        const double dz = std::cos(angle);
        const double rise_along = rise * (dx * ux + dz * uz);
        if (rise_along <= 0.0 || rise_along > max_rise) {
            return ClimbLine{yaw, 0.0, -1.0};
        }
        const double length = StraightLineLength(tri, x, z, dx, dz);
        return ClimbLine{yaw, length, rise_along * length};
    };
    std::optional<ClimbLine> best;
    int best_offset = 0;
    const int window = 0x4000 - min_off;
    const int step = std::max(1, window / coarse_steps);
    for (const int side : {1, -1}) {
        for (int off = min_off; off < 0x4000; off += step) {
            const auto candidate = line(side * off);
            if (!best || candidate.gain > best->gain) {
                best = candidate;
                best_offset = side * off;
            }
        }
    }
    for (int off = best_offset - step; off <= best_offset + step; ++off) {
        const auto candidate = line(off);
        if (candidate.gain > best->gain) {
            best = candidate;
        }
    }
    if (!best || best->gain <= 0.0) {
        return std::nullopt;
    }
    return best;
}

/**
 * Searches the spots where Link can get onto the seam from a floor (where the seam's plane meets
 * the floor's plane, on the seam and on that floor, with no other floor up to 50 above), for the
 * one with the straight line that climbs the most, among the directions where the seam rises at
 * most `max_rise` per unit.
 */
inline std::optional<MountClimb> BestMountClimb(const Triangle& seam,
                                                std::span<const Triangle> collision,
                                                double max_rise,
                                                std::optional<float> near_height = std::nullopt,
                                                float level_tolerance = LevelTolerance) {
    if (!seam.IsSeam()) {
        return std::nullopt;
    }
    const double s_nx = seam.Nx() / seam.Ny();
    const double s_nz = seam.Nz() / seam.Ny();
    const double s_d = seam.dist / seam.Ny();
    const auto [seam_min_x, seam_max_x] =
        std::minmax({seam.vertices[0][0], seam.vertices[1][0], seam.vertices[2][0]});
    const auto [seam_min_z, seam_max_z] =
        std::minmax({seam.vertices[0][2], seam.vertices[1][2], seam.vertices[2][2]});
    const double min_x = seam_min_x - 1.0;
    const double max_x = seam_max_x + 1.0;
    const double min_z = seam_min_z - 1.0;
    const double max_z = seam_max_z + 1.0;

    std::vector<const Triangle*> floors;
    for (const auto& tri : collision) {
        if (tri.Ny() <= 0.5f || &tri == &seam) {
            continue;
        }
        const auto [f_min_x, f_max_x] =
            std::minmax({tri.vertices[0][0], tri.vertices[1][0], tri.vertices[2][0]});
        const auto [f_min_z, f_max_z] =
            std::minmax({tri.vertices[0][2], tri.vertices[1][2], tri.vertices[2][2]});
        if (max_x + 1.0 < f_min_x || min_x - 1.0 > f_max_x || max_z + 1.0 < f_min_z ||
            min_z - 1.0 > f_max_z) {
            continue;
        }
        floors.push_back(&tri);
    }
    const auto walls = WallsNear(seam, collision);
    const auto mountable = [&](const Triangle* floor, float px, float pz) {
        if (!seam.ContainsXZ(px, pz) || !floor->ContainsXZ(px, pz)) {
            return false;
        }
        const float height = floor->HeightAt(px, pz);
        for (const Triangle* other : floors) {
            if (other != floor && other->ContainsXZ(px, pz)) {
                const float other_height = other->HeightAt(px, pz);
                if (other_height > height + 0.01f && other_height < height + FloorCheckHeight) {
                    return false;
                }
            }
        }
        // Somewhere Link can stand, not kept away by a wall
        return !WallPushes(walls, px, height, pz);
    };

    std::optional<MountClimb> best;
    const auto consider = [&](const Triangle* floor, double px, double pz, int coarse_steps) {
        const float fx = static_cast<float>(px);
        const float fz = static_cast<float>(pz);
        if (near_height && std::fabs(floor->HeightAt(fx, fz) - *near_height) > level_tolerance) {
            return false;
        }
        if (!mountable(floor, fx, fz)) {
            return false;
        }
        const auto line = QuickClimbLine(seam, fx, fz, max_rise, coarse_steps);
        if (line && (!best || line->gain > best->line.gain)) {
            best = MountClimb{{{fx, fz}, floor->HeightAt(fx, fz), floor->index, -1}, *line};
            return true;
        }
        return false;
    };
    for (const Triangle* floor : floors) {
        // Seam height = floor height along the line a * x + b * z = c
        const double a = -s_nx + floor->Nx() / floor->Ny();
        const double b = -s_nz + floor->Nz() / floor->Ny();
        const double c = s_d - floor->dist / floor->Ny();
        const double length = std::hypot(a, b);
        if (length == 0.0) {
            continue;
        }
        // Clip the line to the seam's bounding box: point (foot) and direction
        const double foot_x = a / length * (c / length);
        const double foot_z = b / length * (c / length);
        const double dir_x = -b / length;
        const double dir_z = a / length;
        double lo = -std::numeric_limits<double>::infinity();
        double hi = std::numeric_limits<double>::infinity();
        const auto clip = [&](double p, double d, double min, double max) {
            if (d == 0.0) {
                if (p < min || p > max) {
                    hi = -std::numeric_limits<double>::infinity();
                }
                return;
            }
            const double t0 = (min - p) / d;
            const double t1 = (max - p) / d;
            lo = std::max(lo, std::min(t0, t1));
            hi = std::min(hi, std::max(t0, t1));
        };
        clip(foot_x, dir_x, min_x, max_x);
        clip(foot_z, dir_z, min_z, max_z);
        if (lo > hi) {
            continue;
        }
        // Coarsely along the line, then finely around the best spot found on it
        constexpr double Step = 0.5;
        std::optional<double> best_t;
        for (double t = lo; t <= hi; t += Step) {
            if (consider(floor, foot_x + dir_x * t, foot_z + dir_z * t, 24)) {
                best_t = t;
            }
        }
        if (best_t) {
            for (double t = *best_t - Step; t <= *best_t + Step; t += Step / 16) {
                consider(floor, foot_x + dir_x * t, foot_z + dir_z * t, 96);
            }
        }
    }
    if (best) {
        // The best direction from there, exactly
        if (const auto exact =
                BestClimbLine(seam, best->spot.point[0], best->spot.point[1], max_rise)) {
            best->line = *exact;
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
