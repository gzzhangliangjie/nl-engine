# dump_map_actors.py - read-only recon of GuangZhouCity_UE58 City_Main.umap
# Run headless:
#   UnrealEditor-Cmd.exe GuangZhouCity.uproject -run=pythonscript -script="<this>.py" \
#     -stdout -unattended -nopause -nosplash -nullrhi
import unreal
import json
import math

OUT = r"E:\UE_STUDY_2026\nl-engine-game\.workbuddy\ue58-actors.json"

unreal.log("DUMP: loading City_Main")
loaded = unreal.EditorLoadingAndSavingUtils.load_map("/Game/Map/City_Main")
if not loaded:
    unreal.log_error("DUMP: load_map FAILED")
    raise SystemExit(1)

actors = unreal.EditorLevelLibrary.get_all_level_actors()
unreal.log(f"DUMP: actors={len(actors)}")

rows = []
for a in actors:
    try:
        cls = a.get_class().get_name()
        name = a.get_name()
        loc = a.get_actor_location()
        rot = a.get_actor_rotation()
        scale = a.get_actor_scale3d()
        origin, extent = a.get_actor_bounds(False)
        row = {
            "cls": cls,
            "name": name,
            "loc": [loc.x, loc.y, loc.z],
            "rot_q": [rot.quaternion().x, rot.quaternion().y, rot.quaternion().z, rot.quaternion().w],
            "scale": [scale.x, scale.y, scale.z],
            "origin": [origin.x, origin.y, origin.z],
            "extent": [extent.x, extent.y, extent.z],
        }
        smc = a.get_editor_property("static_mesh_component") if isinstance(a, unreal.StaticMeshActor) else None
        if smc and smc.static_mesh:
            row["mesh"] = smc.static_mesh.get_path_name()
        rows.append(row)
    except Exception as e:  # noqa: BLE001
        rows.append({"cls": "ERROR", "name": str(e)})

# summarize by class
by_cls = {}
for r in rows:
    by_cls[r["cls"]] = by_cls.get(r["cls"], 0) + 1

with open(OUT, "w", encoding="utf-8") as f:
    json.dump({"map": "City_Main", "actor_count": len(rows), "by_class": by_cls, "actors": rows}, f, ensure_ascii=False)

unreal.log(f"DUMP: DONE rows={len(rows)} out={OUT}")
unreal.log("DUMP: by_class=" + json.dumps(by_cls))
