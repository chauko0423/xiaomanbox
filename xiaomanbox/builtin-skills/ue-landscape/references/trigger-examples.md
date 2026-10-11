# Trigger examples

## Should trigger

- 建一块 2 公里见方的平地地形
- 用 D:\maps\valley.png 这张高度图生成地形，高低差 300 米
- 给地形配上 RVT，颜色和高度都要
- RVT 是黑的 / 地形边缘一圈发黑
- 石头和地面融合没效果
- 这个关卡里的地形多大？

## Should not trigger

- 在地形上撒一片松树 → `ue-pcg-scattering`
- 把地形材质改成按坡度混合草和岩石 → `ue-material-authoring`
- 在这里放一块石头 → `ue-actor-placement`

## Ambiguous

- 「把地形弄高一点」: there is no sculpting. Offer to rebuild from a heightmap or move the
  landscape up with `location`, and ask which one.
- 「地形太暗」: could be lighting or material, not RVT. Check with `landscape_list` whether
  RVT is involved before touching it.

## Failure handling example

`landscape_setup_rvt` returns "还有没对上的：项目没开虚拟纹理支持". Reply: the RVT is wired
up, but it only works after the project setting is on and the editor restarts; ask whether to
go ahead. Do not say the RVT is working.
