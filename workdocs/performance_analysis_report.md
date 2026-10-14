# oSBM 稳定版（stable-7ed6250）性能瓶颈深度检查报告

> **2026-10-14 复核更新**：当前工作树基于 `9c0bc719`，并非标题中的基准。原报告以下耗时、收益比例和“零风险”结论未经 Android 实测，不应据此直接扩大平台宏。逐项结论及本次实际修改见文末第 7 节。

> **报告状态**：静态深度代码审查已完成（代码暂未做任何修改）  
> **目标基准**：`stable-7ed6250`（Android 移动端引擎与渲染管线）  
> **核心参考文献**：`doc/optimization-history.md`、`source/client/`、`source/rendering/`、`source/game/`、`source/application/`

---

##  executive summary 概要总结

在对当前稳定分支 `stable-7ed6250` 进行全面的静态代码剖析后，我们发现：**当前 Android 平台虽然集成了基础的移动端架构与 rpmalloc 分配器，但在核心管线上存在严重的“性能优化孤岛”现象。**

Switch 移植版本在过去几周中通过一系列深度并发与降频优化，成功将实际游戏帧率从 **4.5 FPS 提升到了稳定 46 FPS**。然而，由于这些最关键的架构级改进全部被硬编码包裹在 `#ifdef STAR_SYSTEM_SWITCH` 条件编译宏中，**拥有 8 核心强劲 CPU 的 Android 平台完全被排除在外，退化为单线程全串行运行模式**。

同时，在渲染管线、UI 体系与 Android 帧同步逻辑中，还存在若干**直接浪费 40%~60% 渲染时间的结构性陷阱**。

```
+----------------------------------------------------------------------------------------------------+
|                                    当前 Android 主线程耗时瓶颈 (35~55ms)                           |
+-------------------------------------------------+--------------------------------------------------+
|  宇宙与世界实体模拟 tick (同步执行): 18 ~ 28ms   |  世界渲染与 GL 提交 (同步执行): 15 ~ 25ms        |
|  - updateAllEntities (全量 60Hz 模拟，无距离分级) |  - 11 个 WorldPainter 图层频繁 flush 断裂批处理   |
|  - Assets::json 争抢全局 mutex 互斥锁           |  - HUD 界面每帧全量递归重建 (无离屏复合复用)       |
|  - 单机本地回环每帧全量序列化/反序列化            |  - >50ms 门控阈值陷阱 (20~45 FPS 区间优化全休眠)  |
+-------------------------------------------------+--------------------------------------------------+
|  ==> 导致 Android 多核强芯只能跑出 18~28 FPS，且伴随双重休眠与 SurfaceFlinger 相位微抖动            |
+----------------------------------------------------------------------------------------------------+
```

---

## 1. 核心架构缺陷：关键优化被错误限制在 Switch 宏内

Switch 移植时沉淀了大量颠覆性的性能成果（见 `doc/optimization-history.md`），但它们目前并未共享给 Android。

### 1.1 并行模拟与渲染管线缺失（Sim ∥ Paint Parallel Pipeline）
- **涉及文件**：
  - [StarClientApplication.cpp](file:///home/wuyupeng/oSBM/source/client/StarClientApplication.cpp#L758-L764)
  - [StarClientApplication.cpp](file:///home/wuyupeng/oSBM/source/client/StarClientApplication.cpp#L810-L833)
  - [StarClientApplication.cpp](file:///home/wuyupeng/oSBM/source/client/StarClientApplication.cpp#L1956-L1988)
  - [StarClientApplication.hpp](file:///home/wuyupeng/oSBM/source/client/StarClientApplication.hpp#L130-L153)
- **机制对比**：
  - **Switch**：在 `updateRunning()` 中将模拟延迟，主线程在 `render()` 抽取不可变渲染快照 `renderData` 后，立即唤醒专用 Worker 线程并行执行 `m_universeClient->update(dt)`；主线程同时在 GPU/GL 上执行世界绘制。帧耗时由两者的**重叠并行时间 `max(Sim, Paint)`** 决定。
  - **Android**：宏被隔离，直接退化为 `#else` 分支的单线程同步执行：
    ```cpp
    #if defined(STAR_PLATFORM_MOBILE) && !defined(STAR_SYSTEM_SWITCH)
    {
      int64_t ucStart = Time::monotonicMicroseconds();
      m_universeClient->update(dt); // 串行在主线程阻塞执行 18~28ms
      s_phaseUniverseUs += Time::monotonicMicroseconds() - ucStart;
    }
    #endif
    ```
- **影响评估**：
  现代 Android 手机普遍具备 8 个物理核心（大核+中核+小核架构），多核算力极其富余。目前在 Android 上，主线程必须先执行完完整的世界模拟，再串行执行完整的世界渲染，两者耗时直接相加（`20ms + 18ms = 38ms`），帧率直接被压制在 25 FPS 左右。**这是限制 Android 设备发挥高帧率的最大单一架构瓶颈。**

---

### 1.2 HUD 离屏复合缓存未启用（HUD Framebuffer Composite Caching）
- **涉及文件**：
  - [StarClientApplication.cpp](file:///home/wuyupeng/oSBM/source/client/StarClientApplication.cpp#L839-L865)
- **机制对比**：
  - **Switch**：采用了“同步帧 + 复合帧”交替机制。在非同步帧（每 3 帧中的 2 帧），UI 层直接利用离屏 `hud` Framebuffer 缓存，仅用一个全屏单 Quad 贴图贴到底图上，再实时画出光标/触控层，单帧节省 6~8ms 的 UI 构造与顶点提交。
  - **Android**：由于宏隔离，Android 每一帧都在走完整流程：递归遍历所有 Pane 窗口、所有背包槽位、所有状态条、所有文字，重新计算布局并重新推入 GL 顶点缓冲区。
- **影响评估**：在复杂界面打开（例如开启制造界面、大背包、任务手册）时，主线程单帧白白浪费 5~9ms 纯 CPU 渲染开销。

---

### 1.3 全局 Assets 互斥锁争抢与缺少 Thread-Local 缓存
- **涉及文件**：
  - [StarAssets.cpp](file:///home/wuyupeng/oSBM/source/base/StarAssets.cpp#L614-L645)
- **机制对比**：
  - **Switch**：`Assets::json(path)` 使用了线程局部静态缓存 `thread_local TlJsonCache`，利用内容更新代数 `m_jsonContentEpoch` 实现了 99% 以上的高频无锁命中，查询耗时由 ~250µs 降至 ~2µs。
  - **Android**：宏未启用，每次调用 `Assets::json` 都会进入 `getAsset` 并无条件获取全局独占锁 `MutexLocker locker(m_assetsMutex)`。
- **影响评估**：主线程、服务端世界线程、异步光照计算线程、音效线程高频并发查询配置文件（武器属性、怪物行为树、动画状态、音频设定等），在 Android 上引起密集的互斥锁休眠与系统调用唤醒（Futex 系统调用），形成线程 Convoy 效应。

---

### 1.4 服务端实体更新全量无差别模拟（Server Entity Tick Spreading）
- **涉及文件**：
  - [StarWorldServer.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldServer.cpp#L649-L742)
- **机制对比**：
  - **Switch**：根据帧间隔动态计算负载分级：对于玩家视野 80 格以外的远端实体（地下生成的生物、地表远处的 NPC），在有负载时降频至 1/4 频率更新；对于近处非关键实体降频至 1/2 更新，并配合位移补偿与索引跳过。
  - **Android**：宏未启用，直接进入 `#else` 调用 `m_entityMap->updateAllEntities`：
    所有已加载区块中的所有实体（哪怕在屏幕几十格外的深坑里），每一帧无条件以 60Hz 满频率跑完整的物理检测与 Lua 脚本。
- **影响评估**：在大型地牢、前哨站、NPC 村落或多生物地形中，服务端 CPU 耗时会急剧飙升至 30~50ms，拖垮整个游戏。

---

### 1.5 单机本地网络同步冗余序列化与监控集合构建
- **涉及文件**：
  - [StarWorldServer.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldServer.cpp#L1592-L1598)
  - [StarWorldServer.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldServer.cpp#L2164-L2185)
  - [StarWorldClient.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldClient.cpp#L1942-L1947)
  - [StarWorldClient.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldClient.cpp#L2230-L2235)
- **机制对比**：
  - **Switch**：单人模式下将本地 Server 到 Client 的实体状态网络同步频率减半（1-in-2），配合客户端网络插值保持平滑；并且在不需要发包的帧，跳过全量实体集合的哈希计算与差值比对。
  - **Android**：每一帧全量序列化所有实体状态并进行回环反序列化，单机游戏在白白承担多余的网络封包开销。

---

## 2. 渲染管线与 GL 提交性能陷阱

### 2.1 ">50ms 负载门控阈值陷阱"（Load-Gated Throttle Trap）
- **涉及文件**：
  - [StarWorldClient.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldClient.cpp#L536)（`bool underLoad = frameGapUs > 50000;`）
  - [StarMainInterface.cpp](file:///home/wuyupeng/oSBM/source/frontend/StarMainInterface.cpp#L1094)（`bool underLoad = gapUs > 50000;`）
  - [StarPaneManager.cpp](file:///home/wuyupeng/oSBM/source/frontend/StarPaneManager.cpp#L329)（`bool underLoad = gapUs > 50000;`）
- **问题分析**：
  代码中对“UI 原语录制/重放（`m_barsRecording`）”、“静态植物与外围生物渲染缓存”、“窗口组件缓存”的启动条件硬编码为 `gapUs > 50000`（即帧率跌破 20 FPS）。
- **严重后果**：
  当游戏运行在 **22 FPS ~ 45 FPS（单帧耗时 22ms ~ 45ms）** 时，玩家已经感受到明显卡顿，但系统判定 `underLoad == false`！所有的降频与复用优化**全部罢工**，继续全量重新计算。这形成了一个严重的死锁状态：**性能原本可以靠复用优化爬升到 50+ FPS，但因为卡在 35 FPS 无法触发阈值，导致永远维持在低帧率**。

---

### 2.2 `WorldPainter` 11 层切分与无条件 `flush()` 打碎批处理
- **涉及文件**：
  - [StarWorldPainter.cpp](file:///home/wuyupeng/oSBM/source/rendering/StarWorldPainter.cpp#L309-L334)
- **代码实现**：
  ```cpp
  auto renderEntitiesUntil = [&](Maybe<RenderLayer> until) {
    while (true) {
      if (entityDrawableIterator == entityDrawables.end()) break;
      if (until && entityDrawableIterator->first >= *until) break;
      for (auto& edl : entityDrawableIterator->second)
        drawEntityLayer(*get<1>(edl), get<0>(edl), get<2>(edl));
      ++entityDrawableIterator;
    }
    m_renderer->flush(); // <--- 无论画没画，每层必调 flush
  };

  renderEntitiesUntil(RenderLayerBackgroundOverlay);
  drawDrawableSet(renderData.backgroundOverlays);
  renderEntitiesUntil(RenderLayerBackgroundTile);
  m_tilePainter->renderBackground(m_camera);
  renderEntitiesUntil(RenderLayerPlatform);
  m_tilePainter->renderMidground(m_camera);
  renderEntitiesUntil(RenderLayerBackParticle);
  renderParticles(renderData, Particle::Layer::Back);
  ... (共计 11 次分层交替)
  ```
- **问题分析**：
  世界实体与 Tile、粒子按照深度被拆解为 11 个微层。每次调用 `renderEntitiesUntil`，哪怕只绘制了一两个物体，末尾的 `m_renderer->flush()` 都会强行执行：
  1. 刷新即时原语并打包顶点；
  2. `glBufferSubData` 上传至 GPU；
  3. 切换 VAO 与纹理绑定；
  4. 触发一次 DrawCall。
- **影响评估**：
  在手机 GPU（如 Mali、Adreno 等基于 Tile-Based Deferred Rendering 架构的 GPU）上，频繁的小批次 Flush 和状态切换会打碎流水线，增加 CPU Driver 提交压力，显著拉长单帧绘制时间。

---

### 2.3 即时渲染顶点构建的逐顶点内存重分配（`accumulationBuffer.resize`）
- **涉及文件**：
  - [StarRenderer_opengl.cpp](file:///home/wuyupeng/oSBM/source/application/StarRenderer_opengl.cpp#L1568-L1585)
- **代码实现**：
  ```cpp
  auto appendBufferVertex = [&](RenderVertex const& v, uint8_t textureIndex, Vec2F textureCoordinateOffset, RenderVertex const& prev, RenderVertex const& next) {
    size_t off = accumulationBuffer.size();
    accumulationBuffer.resize(accumulationBuffer.size() + sizeof(GlRenderVertex));
    GlRenderVertex& glv = *(GlRenderVertex*)(accumulationBuffer.ptr() + off);
    ...
  ```
- **问题分析**：
  在 `GlRenderBuffer::set` 处理数千甚至上万个即时原语顶点时，在没有任何提前 `reserve` 的情况下，每次添加一个顶点（32 字节）就调用一次 `accumulationBuffer.resize`。
- **影响评估**：即使底层有扩容倍率，每次 `resize` 依然伴随着边界检查、尺寸计算与冗余零初始化，消耗密集的 CPU 内存周期。

---

### 2.4 光照计算中每帧重复读取与解析 JSON 配置
- **涉及文件**：
  - [StarWorldClient.cpp](file:///home/wuyupeng/oSBM/source/game/StarWorldClient.cpp#L2174-L2176)
- **代码实现**：
  ```cpp
  void WorldClient::lightingCalc() {
    ...
    m_lightingCalculator.setParameters(root.assets()->json("/lighting.config:lighting").set("pointAdditive", newLighting));
    m_lightingCalculator.setMonochrome(monochrome);
    m_lightingCalculator.begin(lightRange);
    ...
  ```
- **问题分析**：
  `lightingCalc()` 在主线程或异步光照线程中以 **60Hz 满频率运行**。它在每一帧光照计算前，都要：
  1. 调用 `root.assets()->json("/lighting.config:lighting")`（在 Android 上每次都要争抢全局 Asset Mutex 锁！）；
  2. 拷贝并构建一个临时的修改后 Json 对象；
  3. 解析 JSON 树重新赋值给内部的几十个浮点数学参数！
- **影响评估**：这些参数在游戏运行期间是完全静态的，每秒执行 60 次重复反序列化是纯粹的无谓浪费。

---

### 2.5 地形与液体 Chunk 的全视野逐帧 XXHash3 计算
- **涉及文件**：
  - [StarTilePainter.cpp](file:///home/wuyupeng/oSBM/source/rendering/StarTilePainter.cpp#L67-L79)
  - [StarTilePainter.cpp](file:///home/wuyupeng/oSBM/source/rendering/StarTilePainter.cpp#L125-L153)
- **问题分析**：
  `TilePainter::setup` 每帧计算视野内的所有 Chunk。为了判断方块是否有变化，它对当前视野内的全部 Chunk（通常 80~150 个 Chunk）执行：
  ```cpp
  m_terrainHashBuffer.clear();
  forEachRenderTile(...) { m_terrainHashBuffer.append(...); }
  return XXH3_64bits(m_terrainHashBuffer.ptr(), m_terrainHashBuffer.size());
  ```
- **影响评估**：即使在画面完全静止、没有任何方块被破坏或液体流动的情况下，依然要在每帧来回搬运数十 KB 的 Tile 数据做内存 Hash，占用了珍贵的 L1/L2 缓存与内存带宽。

---

## 3. 移动端游戏主循环与 Android Vsync 相位冲突

- **涉及文件**：
  - [StarMobilePlatform.cpp](file:///home/wuyupeng/oSBM/source/application/mobile/StarMobilePlatform.cpp#L4461-L4475)
- **代码实现**：
  ```cpp
  #ifdef STAR_SYSTEM_SWITCH
    bool presentPaced = m_maxFrameRate >= 59.0f;
  #elif defined(STAR_SYSTEM_IOS)
    bool vsyncPaced = IosVsyncPacer::ensureStarted();
    bool presentPaced = vsyncPaced;
  #else
    constexpr bool presentPaced = false; // Android 走此处
  #endif

  if (m_maxFrameRate > 0.0f && !presentPaced) {
    m_framePacer.tick();
    double frameSpare = m_framePacer.spareTime();
    ...
    int64_t spare = round(frameSpare * 1000.0);
    if (spare > 0)
      Thread::sleepPrecise((unsigned)std::min<int64_t>(spare, 1000));
  }
  ```
- **核心冲突剖析**：
  1. **双重阻塞陷阱**：在 Android 系统上，`SDL_GL_SwapWindow` 底层调用的是 `eglSwapBuffers`。当 SurfaceFlinger 帧缓冲区满时，`eglSwapBuffers` 本身就会**硬件阻塞等待下一个 Vsync 脉冲（通常阻塞 5~16ms）**。
  2. **相位漂移与半帧率锁定**：此时主循环已经消耗了一个完整的 Vsync 周期，但回到外层循环时，由于 `presentPaced` 被写死为 `false`，软件 `m_framePacer` 再次计算并认为还有多余时间，强制调用 `sleepPrecise(spare)`。
  3. **结果**：这一多余的休眠直接导致游戏错过了紧接着的下一个渲染提交窗口，瞬间发生**掉档（Drop to half-rate vsync）**——60 FPS 骤降为 30 FPS，30 FPS 骤降为 20 FPS，并且在触控滑动时表现为明显的微抖动（Jitter）。此外，现代 Android 普遍为 90Hz/120Hz 高刷屏，写死 60Hz 软件 sleep 会与系统的 Choreographer 严重拍频。

---

## 4. 瓶颈量化评估总览表

| 瓶颈项 | 所在模块 | 当前 Android 现状 | 预计可挽回耗时 | 优化难易度与风险 |
|---|---|---|---|---|
| **并行模拟管线缺失** | `ClientApplication` | 模拟与渲染全串行在主线程 | **15 ~ 25 ms / 帧** | 极低（Switch 方案成熟现成） |
| **HUD Framebuffer 离屏复合** | `ClientApplication` | 每帧全量重绘整个 UI 树 | **4 ~ 7 ms / 帧** | 极低（直接复用 Switch 实现） |
| **Assets 全局互斥锁竞争** | `StarAssets` | 每次查配置抢全局锁阻塞 | **2 ~ 5 ms / 帧** | 极低（启用 Thread-Local 缓存） |
| **服务端实体无差别 60Hz 模拟** | `WorldServer` | 远端边缘实体满频全量模拟 | **5 ~ 15 ms / 帧** | 极低（启用 Switch 距离分级） |
| **>50ms 门控阈值陷阱** | `WorldClient/UI` | 20~45 FPS 区间优化全部失效 | **3 ~ 6 ms / 帧** | 极低（阶梯化下调阈值至 25ms） |
| **Android 软件 Sleep 二次阻塞** | `StarMobilePlatform` | 与 SurfaceFlinger 硬件 Vsync 冲突 | 消除掉档/锁定 | 低（自适应 PresentPaced 对齐） |
| **光照配置每帧重复解析 JSON** | `WorldClient` | 60Hz 循环中重复反序列化 | **0.5 ~ 1.5 ms / 帧** | 极低（配置静态化缓存） |
| **即时渲染顶点逐个扩容** | `Renderer_opengl` | 频繁 resize 与内存清零 | **0.5 ~ 1.0 ms / 帧** | 极低（增加 reserve 预分配） |
| **WorldPainter 11 次分层 Flush** | `WorldPainter` | 过度切分 DrawCall 与状态切换 | **1 ~ 2 ms / 帧** | 中（需保证图层遮挡正确性） |

---

## 5. 建议推进路线图（分阶段行动建议）

按照“**最大收益优先、零功能改动、零代码风险**”的原则，建议后续分步实施：

### 阶段一：打通跨平台架构隔离（P0 级最高收益，预期释放 60%+ 性能）
1. **解除并发模拟管线对 Switch 的单平台限制**：
   将 `StarClientApplication.cpp` 与 `.hpp` 中的 `startSimTick` / `joinSimTick` 由 `#ifdef STAR_SYSTEM_SWITCH` 扩展为 `#if defined(STAR_SYSTEM_SWITCH) || defined(STAR_SYSTEM_ANDROID)`，让 Android 8 核心 CPU 正式启用 Sim ∥ Paint 双核重叠执行。
2. **解除 Assets Thread-Local 缓存限制**：
   将 `StarAssets.cpp` 中的 `tlJsonCache` 扩展到 `STAR_SYSTEM_FAMILY_MOBILE`，彻底根除高频资产查询的全局 Mutex 争抢。
3. **解除服务端实体距离分级限制**：
   将 `StarWorldServer.cpp` 中的 `updateAllEntitiesConditional` 扩展至移动平台，在复杂村庄与地牢中让远端实体自适应降频。
4. **修正 Android 帧同步逻辑**：
   在 `StarMobilePlatform.cpp` 中修正 `presentPaced`，使 Android 能够感知或合理处理 EGL SwapBuffers 的硬件 Vsync 阻塞，消除双重休眠与掉档。

### 阶段二：消除渲染层与界面层算力浪费（P1 级优化，预期再提升 15%~25%）
1. **启用 HUD Framebuffer 离屏复合**：
   在 Android 上复用 Switch 的 HUD 离屏 Quad 贴图方案，非同步帧直接贴图，将 UI 耗时从 8ms 压到 1.5ms。
2. **阶梯化重构门控阈值（破除 >50ms 陷阱）**：
   将 `underLoad` 阈值从单纯的 `> 50000µs` 改造为两级阶梯（例如 `> 25000µs` 开启轻量级 UI 录制与静态植物复用），让 30~40 FPS 区间也能顺畅激活优化，爬升至 50+ FPS。
3. **静态化光照配置**：
   在 `WorldClient` 初始化或配置变更时才更新 `m_lightingCalculator.setParameters`，去除每帧 60 次的 JSON 反序列化。
4. **顶点累积缓冲区预分配**：
   在 `GlRenderBuffer::set` 中对 `accumulationBuffer` 进行 `reserve`，消除逐顶点微扩容。

---

## 6. 结论

**结论非常明确**：当前稳定版在 Android 上的性能受限，绝大部分不是底层手机硬件算力不足，而是**大量已在 Switch 上验证极为成功的重磅性能优化被限制在了 Switch 宏内，导致 Android 处于未开启多核并发的‘裸奔’状态**。

只需将这些已被验证且零逻辑风险的并发管线、离屏复合、线程局部缓存和降频机制打通至 Android，并破除 >50ms 门控死锁与 Vsync 双重休眠，即可直接在手机端带来**翻倍级别的帧率跃升与极度顺滑的体验**。

---

## 7. 当前代码复核与 Android 采用情况（2026-10-14）

本节以实际代码为准，修正上文中不成立或尚无证据的推断。`doc/optimization-history.md` 开头明确说明，46 FPS 的主要验证来自 **Ryujinx**，并非 Android 设备，也不是本轮 Switch 真机测试。该历史收益包含 rpmalloc、Mesa 驱动修补等多项工作，不能归因于并行管线，更不能推出 Android 必然翻倍。Android 的服务端世界线程、光照线程等已经存在；“全引擎单线程”不准确，缺少的是客户端模拟与主线程绘制的这一段重叠。

| 项目 | 代码核实及本次处理 |
|---|---|
| Assets 线程局部 JSON 缓存 | **已扩展至 Android**，保留 Switch 路径、8192 条上限及热重载失效。额外修复原缓存只比较实例内部 epoch 的问题：改用进程内唯一的内容 epoch，避免更换资源管理器或模组后命中旧实例的数据。iOS/桌面继续走原有资产查询路径。 |
| 光照配置重复解析 | **已优化共享 WorldClient**。仍查询资产以感知热重载；仅在 JSON 内容或 `newLighting` 选项变化时构造覆盖配置并调用 `setParameters`。`monochromeLighting` 的即时切换保留；进入新世界后重新应用覆盖值。不是永久静态化配置。 |
| 模拟与绘制并行 | **暂缓默认开启**。不只涉及 ClientApplication：UniverseClient 的工作线程标记、生命周期包延迟处理也是 Switch 条件路径。现有保护只明确延迟 WorldStart/WorldStop，断线包和收包异常仍能调用 reset；主线程还有读取实时光照/预览数据的路径。需要继续审查 Lua/UI 回调、退出/异常及多次追赶 update，并在 Android 实测。管线也会增加一 tick 固定延迟。 |
| HUD framebuffer 复合 | **暂缓默认开启**。可以迁移，但要补齐 Android 窗口尺寸、界面缩放、世界/界面切换后的缓存失效，评估高分辨率全屏 RGBA 缓冲的显存和复合成本。沿用每三帧刷新还会降低 HUD 更新频率，不能称为零功能取舍。 |
| 服务端实体 tick spreading | **暂缓**。远处实体使用普通 dt、每四步才运行一次，会让模拟时间降至四分之一；近处用倍率 dt 也不保证物理、Lua 和触发逻辑等价。现有逻辑没有仅限单机的条件，不宜直接推广。 |
| 本地网络降频与监控集合跳过 | **暂缓新增服务端降频**。客户端到本地服务端的状态发送 ×4 已经按连接本地性跨平台启用。Switch 的服务端路径同时涉及全局远端发送定时器 ×2、本地每四步门控、普通插值配置，实际发送还取决于 `sendRemoteUpdates || sendLocalUpdates`，不是一个独立的“单机减半”开关。迁移应按客户端本地性处理，保持远端联机节奏。 |
| 50 ms 负载门槛 | **已有跨平台优化，暂不改阈值**。WorldClient 的外围渲染缓存、客户端从属实体更新降频，MainInterface 的 HUD 原语重放和 PaneManager 的窗口缓存均已可用于 Android。降到 25 ms 会更早触发动画和 UI 更新降频；直接改常数还可能在门槛附近反复切换，应依据帧耗时实测设计滞回。 |
| Android 帧同步 | **暂不修改**。TickRateApproacher 统计整帧频率，swap 阻塞已经影响 spareTime，所以代码不能证明存在报告描述的必然二次完整休眠。Android 启动时避免改 swap interval 有 VsyncReceiver 崩溃兼容注释。不能复制 Switch 的固定 60 Hz 判断；需先量测 swap/sleep 与实际刷新率，兼顾低帧率上限及 90/120 Hz 屏幕。 |
| 分层 flush | **暂不修改**。`flushImmediatePrimitives` 在没有原语时直接返回，空层不必然产生 DrawCall；`renderBuffer` 本身也会刷新即时原语，以保持绘制顺序。是否可删需核对坐标变换/状态/顺序并测量有效提交次数。 |
| 顶点 accumulationBuffer.reserve | **不采用报告中的推断**。ByteArray::resize(size) 不进行零初始化，reserve 已按倍增扩容，clear 后保留容量。预分配可能减少初次增长，但不能消除不存在的逐顶点重分配；额外扫描和过量预留也有成本。该 OpenGL 实现确实由 Android GLES 子类复用。 |
| Tile chunk 每帧 hash | **暂不修改**。缓存 key 需要反映瓦片及渲染输入变化；只有建立覆盖修改/加载/卸载及相关渲染变化的可靠版本或脏标记后，才适合替代内容 hash。原报告没有提供搬运/哈希耗时的 Android 测量。 |

本次代码修改仅涉及 `source/base/StarAssets.cpp/.hpp` 与 `source/game/StarWorldClient.cpp/.hpp`。原有 `StarMobileControls.cpp`、`StarMobilePlatform.cpp` 工作树修改予以保留。

验证结果：

- `git diff --check`：通过，无空白错误。
- 在 `source/` 执行 `CCACHE_DISABLE=1 cmake --build --preset android-arm64-release --target starbound --parallel 4`：退出码 0，成功编译、链接并生成 ARM64 `libmain.so`（验证等级 3）。日志：`/tmp/osbm-android-perf-build.log`。构建有现有的未使用参数/函数等警告，未将这些警告作为本次重构范围。
- 从现有 ARM64 `compile_commands.json` 提取两个修改的 `.cpp` 编译参数，分别用 NDK clang++ 的 `--target=armv7-none-linux-androideabi28 -D_FILE_OFFSET_BITS=64 -fsyntax-only` 和 `--target=x86_64-none-linux-android28 -fsyntax-only` 检查：四次均退出码 0（验证等级 2）。复用了 ARM64 构建中的依赖头文件，不是 ARMv7/x86_64 的完整构建或链接验证。日志：`/tmp/osbm-perf-{armv7,x64}-{StarAssets,StarWorldClient}.cpp.log`。
- Switch、iOS、桌面：只做了共享代码及条件分支的静态检查（等级 1），未编译或运行。未执行单元测试，未构建 APK/IPA/NRO，未做设备运行验证。

剩余风险：线程局部缓存会延长 JSON 对象的驻留时间（每线程最多 8192 个路径）；配置热重载、世界切换和光照选项切换的运行行为仍需设备回归。实际 Android 帧率、帧时间和内存收益尚未测量，不承诺报告上文的毫秒收益或提升比例。
