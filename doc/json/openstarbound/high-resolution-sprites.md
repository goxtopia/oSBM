# 高清人物、服装与武器精灵图（pixelScale）

oSBM 支持让精灵图使用 2x / 4x 的实际像素密度，同时保留原来的显示大小和坐标单位。
这是**按图片显式启用**的功能：在该图片对应的 `.frames` 中声明 `"pixelScale": 2` 或 `4`。
未声明、或声明为 `1` 时，沿用原有尺寸规则；不会根据文件名 `@2x`、PNG 大小、种族或装备类型猜测倍率。

适用路径包括普通 Humanoid、使用 NetworkedAnimator 的人物/服装/武器动画、物品 Drawable 图标、
人物头像以及过场中的 Drawable。现有 PNG 上传、纹理过滤和绘制图层顺序保留，
不会在加载时把高清人物图缩回低分辨率。

## 两套像素单位

| 数据 | 使用的单位 |
|---|---|
| PNG 宽高、`.frames` 的 `size` / `begin` / `frameList` 矩形 | 实际高清像素（物理 texel） |
| 原来以像素定义的人物骨骼挂点、手持点、舞蹈和动画偏移 | 原来的 1x 逻辑像素 |
| 原来以世界格数定义的动画变换、碰撞多边形、发射位置等 | 原来的世界单位 |
| Drawable 的图片局部变换矩阵 | 逻辑像素到最终绘制空间的变换 |

例如原来一帧是 `43 × 43`，2x 图的一帧为 `86 × 86`，声明 `pixelScale: 2` 后仍占
`43 × 43` 逻辑像素；4x 则使用 `172 × 172`。这里的 43 只是示例，实际应读取原资源的帧尺寸。
世界换算的 `TilePixels` 仍为 8，不要修改它来实现高清。

## 一个 2x 动画图片的完整例子

假设某个模组自己定义的原图 `/myhd/actor.png` 有两帧，每帧 `43 × 43`，帧名为 `idle`、`walk.1`。
将整张 PNG 重绘为 `172 × 86` 后，`/myhd/actor.frames` 可以写成：

```json
{
  "pixelScale": 2,
  "frameGrid": {
    "size": [86, 86],
    "dimensions": [2, 1],
    "names": [["idle", "walk.1"]]
  },
  "aliases": {
    "default": "idle"
  }
}
```

图片引用仍为 `/myhd/actor.png:idle`、`/myhd/actor.png:walk.1`，无需添加新的图片指令。
`pixelScale` 也会作用于 `default` 这样的帧别名和带指令的图片引用。

如果使用 `frameGrid.begin`，起点也要乘 N；如果使用 `frameList`，矩形的四个坐标都乘 N。
`dimensions` 是帧的数量，保持不变。原有人物的全部帧名、别名和动作布局都应保留，
不要用这个两帧示例直接覆盖完整的人物帧表。

不需要切片的高清 PNG（例如一个独立遮罩）也可配一个只含元数据的同名 `.frames`：

```json
{ "pixelScale": 4 }
```

只有 1、2、4 是有效倍率。0、负数、小数和其他倍率会报告资源配置错误。

## 人物与服装的适配步骤

1. 按原图的画布、透明留白、肢体位置和帧布局重绘 N 倍 PNG。
2. 修改该图真正使用的 `.frames`：加入 `pixelScale`，把切片尺寸和坐标乘 N。
3. 保留原来的人物骨骼、手臂旋转中心、装备偏移、舞蹈偏移和运动/碰撞配置。
4. 头、身体、手臂、头发、表情、胡须、服装各层可以分别使用 1x / 2x / 4x。
   每张高清图都需要正确的 `.frames` 声明。旧服装无需为了配合高清身体而改倍率。
5. 保留已有帧名来兼容旧动作；增加/更换动作时仍按
   [humanoidConfig](humanoidConfig.md) 和 [animation](animation.md) 的现有接口适配。

`.frames` 按现有引擎规则查找：优先同名文件，其次目录中的 `default.frames`。
不要把混有 1x 和 2x 图片的整个目录的 `default.frames` 一律改成 2x；给高清图片配独立的同名文件更稳妥。
模组覆盖原路径时，PNG 和与其匹配的帧表必须一起提供。

头发/头盔遮罩以及人物头像裁切遮罩会按各自声明的密度对齐。
例如 2x 头发可以直接使用未标记的原版 1x 头盔遮罩；1x 头发也能使用 4x 遮罩。
遮罩在逻辑像素中心按最近邻采样到目标图的密度，不会把整个角色缩回 1x。
遮罩内容和透明留白仍须符合原来的逻辑画布，倍率转换不会自动修复画错的位置。

## 武器与手持物品

对 `.animation` 引用的各个武器 PNG 做同样处理：扩大 PNG 和帧切片，并声明每张图的 `pixelScale`。
保留 `offset`、旋转中心、变换组、手持点、枪口点、伤害多边形和脚本坐标的原有单位及数值。
它们不会因为纹理密度变高而自动扩大。

例如原图的逻辑手持点是 `[3, 2]`，重绘为 4x 后仍填写 `[3, 2]`，无需填写 `[12, 8]`。
已经以世界格数表示的配置也仍保持原值。高清的 `inventoryIcon` 如果走 Drawable 图片路径，也按此规则显示。

已经用 `scale: 0.5`、变换矩阵或 `?scalenearest=0.5` 手动补偿 2x 图的旧模组，
在未声明 `pixelScale` 时仍沿用原行为。如果迁移为 `pixelScale: 2`，应去掉那一层用于密度补偿的缩小，
否则会再缩小一次；用于美术造型的其他变换则保留。

## 图片指令与 Lua

对声明了 N 倍密度的图片，下列指令的像素参数仍写 1x 逻辑像素，引擎会映射到实际 texel：

- `crop=xMin;yMin;xMax;yMax`：裁剪坐标。
- `border` / `outline`：边框像素宽度。
- `addmask` / `submask`、`blendmult` / `blendscreen`：参考图偏移。

颜色替换不改变，`scale` / `scalenearest` 等的倍率仍是相对缩放，会改变逻辑显示大小。
掩码/混合参考图依据自身 `.frames` 声明匹配密度；不能把一张实际为 2x 的遮罩漏标为 1x。
指令也不会替代 `.frames` 的物理像素切片规则。

已有 `root.imageSize(path)` 和 `root.nonEmptyRegion(path)` 继续返回实际图片的像素尺寸/区域，
不会为了新功能改变旧脚本的返回类型或数据含义。新增：

```lua
local density = root.imagePixelScale("/myhd/actor.png:idle") -- 2
local size = root.logicalImageSize("/myhd/actor.png:idle")  -- {43, 43}
local physical = root.imageSize("/myhd/actor.png:idle")     -- {86, 86}
```

会从 `root.imageSize` 推导挂点或布局的高清模组脚本，改用 `root.logicalImageSize`；
直接处理图像 texel 的脚本继续用 `root.imageSize`。
`root.imageSpaces` 保持返回世界格子的用途，会使用密度归一化后的占格范围。
原有未标记资源的这些调用结果保持原样。

## 安装、切换和兼容边界

- 通过启动器模组管理器启用/停用整套高清资源包。2x、4x 可以作为互斥的资源包变体提供；
  切换后返回启动器并重新进入游戏，确保现有动画/装备缓存重建。
  此接口不负责自动选择 `@2x` 文件，也不自动生成高清绘画或动作。
- 标准图和旧模组无需适配。网络 Drawable 格式、角色存档结构、碰撞参数和服务器资源校验策略均未改。
- 新的高清资源包需要支持 `pixelScale` 的 oSBM 引擎。原版 Starbound 忽略这个字段，不能据此正确保持高清精灵的显示大小。
- 该机制针对 Drawable 精灵图。直接上传纹理、手写屏幕四边形、地图 tile atlas、字体或硬件光标等路径，
  不会自动推断逻辑尺寸；需要模组/对应渲染调用显式处理，不能全局替换全部贴图。
- 2x 图片像素数量约为原来的 4 倍，4x 约为 16 倍；解码、缓存、遮罩处理和显存成本都会增加。
  若最终显示区域不足以容纳新增 texel，高清细节仍会被采样丢弃。

## 检查与测试

适配时检查站立/跑跳/游泳、面朝左右、双手武器及枪口位置、穿脱旧/新服装、头发遮罩、人物头像和背包图标。
对比碰撞、发射点和伤害范围的配置，不能为了贴图高清而改变这些数值。

仓库提供无需 `packed.pak` 或 GL 上下文的 `sprite_tests`，动态生成自己的小 PNG 与 `.frames`，
检查 1x/2x/4x 的帧别名、实际图片尺寸、Drawable 边界、提交的顶点/UV、混合密度遮罩、图片指令及热重载。
在已配置并启用 `BUILD_TESTING` 的桌面构建上运行：

```sh
cmake --build build/<desktop-preset> --target sprite_tests --parallel 4
ctest --test-dir build/<desktop-preset> -R '^sprite_tests$' --output-on-failure
```

这些是资产和 CPU 绘制数据的回归检查，不能替代各平台 GPU/设备上的视觉验证。
