-- dump_max_nodes.ms - read-only node inventory of the Zhujiang New Town .max scene
-- Run: 3dsmaxbatch.exe dump_max_nodes.ms -sceneFile="<path>.max"

out = createfile @"E:\UE_STUDY_2026\nl-engine-game\.workbuddy\max-nodes.txt"
format "sceneObjects=%\n" objects.count to:out
for o in objects do
(
    local cls = classof o as string
    local dx = o.max.x - o.min.x
    local dy = o.max.y - o.min.y
    local dz = o.max.z - o.min.z
    format "%|%|%,%,%|%,%,%|%,%,%\n" o.name cls o.min.x o.min.y o.min.z o.max.x o.max.y o.max.z dx dy dz to:out
)
close out
