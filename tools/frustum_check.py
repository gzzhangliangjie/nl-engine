#!/usr/bin/env python3
"""M4.1 offline frustum-cull reference (S4-PLAN: tools/frustum_check.py).

Reads a .nmesh asset's per-cluster bounding spheres and computes, on the CPU,
which clusters survive the exact same 5-plane sphere test the GPU compute pass
(cull.slang csCull) performs. The visible count is compared against the GPU
readback number logged by `viewer --mode cull` ("cull stats: visible N / M ...")
— a mechanical check, not an eyeball check.

Camera math is a line-for-line re-implementation of core/src/camera.cpp
(OrbitCamera::viewProjection: perspectiveZO x lookAt, column-major, D3D-style
[0,1] depth) and the Gribb-Hartmann plane extraction used by bevy
cull_shared.wesl:44-64 (5 planes: row3±row0, row3±row1, row2; far plane
skipped). Any drift between this file, camera.cpp and cull.slang is a bug.

Usage:
  python tools/frustum_check.py <asset.nmesh> [--yaw D] [--pitch D] [--dist F]
      [--width W] [--height H] [--expect N] [--list]

Exit codes: 0 ok / 1 failure (bad args, unreadable asset, count mismatch).
"""

import argparse
import math
import struct
import sys

NMESH_MAGIC = 0x48534D4E  # "NMSH"
RECORD_SIZE = 52          # NMeshletRecord: 4x u32 + sphere[4] + cone[3] + cutoff + error


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def perspective_zo(fov_y, aspect, near_z, far_z):
    """camera.cpp perspectiveZO — column-major, depth range [0,1]."""
    f = 1.0 / math.tan(fov_y * 0.5)
    m = [0.0] * 16
    m[0] = f / aspect
    m[5] = f
    m[10] = far_z / (near_z - far_z)
    m[11] = -1.0
    m[14] = (near_z * far_z) / (near_z - far_z)
    return m


def look_at(eye, center, up):
    """camera.cpp lookAt — right-handed, looking down -Z."""
    zx, zy, zz = eye[0] - center[0], eye[1] - center[1], eye[2] - center[2]
    ln = math.sqrt(zx * zx + zy * zy + zz * zz)
    zx, zy, zz = zx / ln, zy / ln, zz / ln            # camera backward

    xx = up[1] * zz - up[2] * zy
    xy = up[2] * zx - up[0] * zz
    xz = up[0] * zy - up[1] * zx
    ln = math.sqrt(xx * xx + xy * xy + xz * xz)
    xx, xy, xz = xx / ln, xy / ln, xz / ln            # camera right

    yx = zy * xz - zz * xy
    yy = zz * xx - zx * xz
    yz = zx * xy - zy * xx                            # camera up

    v = [0.0] * 16
    v[0], v[1], v[2] = xx, yx, zx
    v[4], v[5], v[6] = xy, yy, zy
    v[8], v[9], v[10] = xz, yz, zz
    v[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2])
    v[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2])
    v[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2])
    v[15] = 1.0
    return v


def multiply(a, b):
    """camera.cpp multiply — out = a * b, column-major m[col*4 + row]."""
    out = [0.0] * 16
    for col in range(4):
        for row in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + row] * b[col * 4 + k]
            out[col * 4 + row] = s
    return out


def view_projection(yaw_deg, pitch_deg, dist, width, height):
    """OrbitCamera::viewProjection with identical clamps and constants."""
    yaw = yaw_deg * math.pi / 180.0
    pitch = clamp(pitch_deg * math.pi / 180.0, -1.45, 1.45)
    d = clamp(dist, 0.45, 20.0)
    cp = math.cos(pitch)
    eye = (d * cp * math.sin(yaw), d * math.sin(pitch), d * cp * math.cos(yaw))
    aspect = width / height
    proj = perspective_zo(0.785398163, aspect, 0.1, 100.0)   # kFovY/kNear/kFar
    view = look_at(eye, (0.0, 0.0, 0.0), (0.0, 1.0, 0.0))
    return multiply(proj, view)


def frustum_planes(mvp):
    """Gribb-Hartmann rows from a column-major matrix (row i = column i of rows).

    mvp[c*4 + r] = M[c][r]; row i = (M[0][i], M[1][i], M[2][i], M[3][i]).
    Same 5 planes as bevy cull_shared.wesl / cull.slang sphereInFrustum.
    """
    def row(i):
        return (mvp[0 * 4 + i], mvp[1 * 4 + i], mvp[2 * 4 + i], mvp[3 * 4 + i])

    r0, r1, r2, r3 = row(0), row(1), row(2), row(3)
    return [
        (r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]),
        (r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]),
        (r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]),
        (r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]),
        r2,
    ]


def sphere_in_frustum(center, radius, planes):
    """Same test as cull.slang: normalized plane, outside if dist < -radius."""
    for p in planes:
        ln = math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])
        if ln <= 0.0:
            return False
        if (center[0] * p[0] + center[1] * p[1] + center[2] * p[2] + p[3]) / ln < -radius:
            return False
    return True


def load_spheres(path):
    """Read per-cluster bounding spheres (world space) from a .nmesh v1/2/3 file.

    Layout: NMeshHeader(20B) + name(nameLen) + NMeshletRecord[count](52B each);
    sphere[4] sits at record offset 16..32 (meshlet_io.hpp, static_assert'd).
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20:
        raise ValueError(f"{path}: truncated header ({len(data)}B)")
    magic, _version, name_len, meshlet_count, _vtx, _tri = struct.unpack_from(
        "<IHHIII", data, 0)
    if magic != NMESH_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08x} (expected NMSH)")
    base = 20 + name_len
    need = base + meshlet_count * RECORD_SIZE
    if len(data) < need:
        raise ValueError(f"{path}: truncated records ({len(data)}B < {need}B)")
    spheres = []
    for i in range(meshlet_count):
        off = base + i * RECORD_SIZE + 16
        spheres.append(struct.unpack_from("<4f", data, off))
    return spheres


def main():
    ap = argparse.ArgumentParser(description="M4.1 offline frustum-cull reference")
    ap.add_argument("nmesh")
    ap.add_argument("--yaw", type=float, default=12.0)
    ap.add_argument("--pitch", type=float, default=9.2)
    ap.add_argument("--dist", type=float, default=4.2)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--expect", type=int, default=None,
                    help="fail (exit 1) unless the visible count equals this")
    ap.add_argument("--list", action="store_true", help="print visible cluster ids")
    args = ap.parse_args()

    try:
        spheres = load_spheres(args.nmesh)
    except (OSError, ValueError) as e:
        print(f"frustum_check: FAIL {e}", file=sys.stderr)
        return 1

    mvp = view_projection(args.yaw, args.pitch, args.dist,
                          float(args.width), float(args.height))
    planes = frustum_planes(mvp)

    visible = []
    for i, (cx, cy, cz, r) in enumerate(spheres):
        if sphere_in_frustum((cx, cy, cz), r, planes):
            visible.append(i)

    print(f"frustum_check: {args.nmesh}")
    print(f"  pose      : yaw={args.yaw} pitch={args.pitch} dist={args.dist} "
          f"size={args.width}x{args.height}")
    print(f"  clusters  : {len(spheres)}")
    print(f"  visible   : {len(visible)}")
    print(f"  culled    : {len(spheres) - len(visible)}")
    if args.list:
        print("  ids       : " + (",".join(str(i) for i in visible) or "<none>"))
    if args.expect is not None and len(visible) != args.expect:
        print(f"  expect    : {args.expect} -> MISMATCH", file=sys.stderr)
        return 1
    if args.expect is not None:
        print(f"  expect    : {args.expect} -> MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
