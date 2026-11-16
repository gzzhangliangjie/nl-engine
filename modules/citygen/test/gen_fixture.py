# gen_fixture.py — 从 BusViewer 全广州分片裁珠江新城样本(Gen fixture for cg0-sample ctest)
# 源数据:E:/GuangzhouBusViewer/assets/_pull-roadnet/tile-3-2.json(2026-10-29 Overpass
# out:json 全广州 highway 分片,ODbL)。一次性生成,产物 zjx-sample.json 进 git;
# 计数锚写死进 modules/citygen/CMakeLists.txt cg0-sample 正则,改 fixture 必须同步。
import json

SRC = 'E:/GuangzhouBusViewer/assets/_pull-roadnet/tile-3-2.json'
S, W, N, E = 23.108, 113.316, 23.130, 113.342
KEEP_MAIN = {'motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'residential',
             'unclassified', 'service', 'motorway_link', 'trunk_link', 'primary_link',
             'secondary_link', 'tertiary_link', 'living_street'}

d = json.load(open(SRC, encoding='utf-8'))
ways = [e for e in d['elements'] if e['type'] == 'way']
sel = []
for w in ways:
    g = w.get('geometry') or []
    if not any(S <= p['lat'] <= N and W <= p['lon'] <= E for p in g):
        continue
    hw = (w.get('tags') or {}).get('highway', '')
    if hw not in KEEP_MAIN:
        continue
    sel.append(w)
sel.sort(key=lambda x: x['id'])          # 确定性:按 OSM way id 排序
sel = sel[:300]                          # 体量闸:fixture < ~1MB
out = {'version': 0.6, 'generator': 'citygen gen_fixture.py (subset of tile-3-2, ODbL)',
       'elements': sel}
with open(__file__.rsplit('\\', 1)[0] + '/zjx-sample.json', 'w', encoding='utf-8', newline='\n') as f:
    json.dump(out, f, ensure_ascii=False, indent=1, separators=(',', ': '))
print('ways:', len(sel), 'bytes:', len(json.dumps(out, ensure_ascii=False)))

# ---- M-CG0.2:建筑样本(源 = Overpass mail.ru 镜像 2026-11-16 实拉
# assets/osm/zjx-buildings.json,436 栋珠江新城 building ways,ODbL;
# assets/osm/ 不进 git,本脚本留档可复现)----
BLD_SRC = 'E:/UE_STUDY_2026/nl-engine-citygen/assets/osm/zjx-buildings.json'
bd = json.load(open(BLD_SRC, encoding='utf-8'))
bways = [e for e in bd['elements'] if e['type'] == 'way']
bways.sort(key=lambda x: x['id'])        # 确定性:按 OSM way id 排序
bsel = bways[:150]                       # 体量闸:~170KB
bout = {'version': 0.6,
        'generator': 'citygen gen_fixture.py (subset of zjx-buildings, ODbL)',
        'elements': bsel}
with open(__file__.rsplit('\\', 1)[0] + '/zjx-buildings-sample.json', 'w',
          encoding='utf-8', newline='\n') as f:
    json.dump(bout, f, ensure_ascii=False, indent=1, separators=(',', ': '))
lv = sum(1 for w in bsel if 'building:levels' in (w.get('tags') or {}))
ht = sum(1 for w in bsel if 'height' in (w.get('tags') or {}))
print('buildings:', len(bsel), 'levels:', lv, 'height:', ht)
