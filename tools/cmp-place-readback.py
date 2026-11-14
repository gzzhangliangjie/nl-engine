# M14C1 诊断：对拍两模式的 place atlas 全 4 级回读 .bin（--probe-place-readback）。
# 文件格式：uint32 levels(=4) + 每级 [uint32 w, h + h×w × RGBA32_FLOAT（rowPitch 已剥）]。
import struct, sys

def read_dump(path):
    d = open(path, 'rb').read()
    off = 0
    nLevels, = struct.unpack_from('<I', d, off); off += 4
    levels = []
    for _ in range(nLevels):
        w, h = struct.unpack_from('<II', d, off); off += 8
        rows = []
        for y in range(h):
            rows.append(struct.unpack_from('<%df' % (w * 4), d, off))
            off += w * 16
        levels.append((w, h, rows))
    return levels

A = read_dump(sys.argv[1])
B = read_dump(sys.argv[2])
names = ['x', 'y', 'z', 'w']
total_diff = 0
for lv, ((wa, ha, ra), (wb, hb, rb)) in enumerate(zip(A, B)):
    print(f'--- level {lv}: adaptive {wa}x{ha}  u8 {wb}x{hb}')
    if wa != wb:
        print('    width differ, skip compare')
        continue
    # adaptive 高度 ×2（预留区在下）；均匀区 = adaptive 前 hb 行 vs u8 全部
    worst = []
    for y in range(hb):
        for x in range(wa):
            for c in range(4):
                va = ra[y][x * 4 + c]
                vb = rb[y][x * 4 + c]
                d = abs(va - vb)
                if d > 1e-6:
                    worst.append((d, x, y, names[c], va, vb))
    total_diff += len(worst)
    worst.sort(reverse=True)
    print(f'    uniform-region texels differing >1e-6: {len(worst)} / {hb * wa * 4}')
    for d, x, y, ch, va, vb in worst[:10]:
        print(f'    texel({x},{y}) {ch}: adaptive={va:.5f} u8={vb:.5f} delta={va - vb:+.5f}')
print('TOTAL diffs:', total_diff)
