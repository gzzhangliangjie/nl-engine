# thirdparty/metis — vendored METIS 5 + GKlib

| 项 | 内容 |
|---|---|
| 来源 | `github.com/KarypisLab/METIS` + `github.com/KarypisLab/GKlib`（main 分支浅克隆，2026-11-07） |
| 用途 | nanite-tools 离线管线簇聚组（S2 M2.0：节点=簇、边=共享顶点数权重的图分割） |
| 许可 | `LICENSE.metis` / `LICENSE.gklib`（均为 Apache-2.0 系，与 MIT 项目可并存） |
| 改动 | ① 仅 `include/metis.h`：宽度宏 `IDXTYPEWIDTH 32` / `REALTYPEWIDTH 32` 取消注释（上游要求手动改）；② INT32_MIN 等 4 宏加 `#ifndef` guard（UCRT limits.h 已定义，MSVC 下 C4005 重定义告警）。其余源码原样 |
| 构建 | 本目录 `CMakeLists.txt` 自带，编出静态库 `nl_metis`；不使用上游构建系统 |

参考依据：bevy 0.15/0.16 离线管线同款选型（JMS55：「DAG 质量是 virtual geometry 最重要也最难的部分」，METIS 调参 `UFactor=1`、划分数故意少算防超发）；UE 用自研 GraphPartitioner（流送分页需求，我们 S3 前不需要）。
