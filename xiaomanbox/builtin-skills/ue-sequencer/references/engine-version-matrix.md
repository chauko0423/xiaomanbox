# 引擎版本差异（5.0 – 5.8）

盒子覆盖 UE 5.0–5.8 九个版本。Sequencer 的 Python API 在这个区间里有
**5 处硬破坏性变更**，同一句话在不同版本上能做的事不一样。

**规则：看 `sequence_describe` 返回的 `capabilities`，不要按版本号猜。**
工作室常年跑自定义引擎分支和 hotfix，版本号靠不住。

## 破坏性变更

| 变更 | 从哪个版本起 | 对用户的影响 |
|---|---|---|
| `get_master_tracks` → `get_tracks` | 5.2 | 5.0/5.1 上是另一套方法名 |
| `SequencerBindingProxy` → `MovieSceneBindingProxy` | 5.1 弃用，5.2 全换 | 旧名保留但返回父类实例 |
| `SequenceTimeUnit` → `MovieSceneTimeUnit` | 5.4 | 时间相关调用的写法变了 |
| **Custom Binding 重构** | 5.5 | **spawnable / possessable 的判定语义变了** |
| 加 spawnable 移到 subsystem，且要求序列在编辑器里打开 | 5.6 | 无头批处理会被打断 |

## 用户能感知的能力差异

| 能力 | 有的版本 | 没有的版本怎么办 |
|---|---|---|
| **Dynamic Binding**（根治绑定失效，按逻辑而不是路径解析） | 5.4+ | 5.0–5.3 只能靠重绑。**这正是盒子对这部分用户最有价值的地方** |
| **Binding Tags**（按 tag 解析，不依赖名字） | 5.4+ | 同上 |
| **Motion Blending / Clip Matching**（动画位移自动接续） | 5.6+ | 5.0–5.5 只能手填 Start Location Offset |
| **Animation Mixer**（动画分层，不用回 DCC 改动捕） | 5.8 | 无 |
| **官方 MCP 工具集** | 5.8 | 5.0–5.7 只有盒子 |

## `capabilities` 里会出现什么

`sequence_describe` 返回里带：

- `engine_version` —— 引擎自报的版本串
- `tracks_api` —— 这台引擎用的是 `get_tracks` 还是 `get_master_tracks`
- `spawnable_detection` —— 能不能分出 spawnable / possessable。
  **`false` 时绑定类型会标 `unknown`，要如实告诉用户，不能假装知道**
- `binding_resolution` —— 能不能判定绑定是否失效。
  **`false` 时那项体检根本没跑**，不要说「没有失效绑定」

## 说话的原则

**静默降级是不行的。** 用户在 5.3 上跑出和 5.6 不一样的结果、而工具不说，
他下次就不会再用这个功能。

看到能力受限时，把话说全：
「你这个版本（5.3）没有 Dynamic Binding，所以绑定断了只能重绑，不能根治。
5.4 起有这个功能。」

比「我帮你修好了」有用得多 —— 前者让他知道下次还会断，后者让他以为解决了。
