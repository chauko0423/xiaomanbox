---
name: ue-cpp-workflow
description_zh: 在 Unreal 工程里写、新增并编译 C++，然后热重载进正在运行的编辑器。当用户要玩法代码、新的 UCLASS/USTRUCT/UENUM、Actor/Component/Subsystem 类、改模块依赖，或说「写个 C++ 类」「加个组件」「这段代码编不过」「帮我改下 Build.cs」时使用；蓝图图编辑、材质图、Sequencer、改引擎自带源码（Engine/Source）不适用于它。
description: Writes, adds and compiles C++ in an Unreal project, then hot-reloads it into the running editor. Use when the user asks for gameplay code, a new UCLASS/USTRUCT/UENUM, an Actor or Component or Subsystem class, a module dependency change, or says things like "写个 C++ 类"、"加个组件"、"这段代码编不过"、"帮我改下 Build.cs". Do not use for Blueprint graph editing, for material graphs, for Sequencer, or for modifying the engine's own source under Engine/Source.
---

# UE C++ 工作流

## Quick Start

**动手写任何 C++ 之前，先 `cpp_probe`。** 它回答两个会改变做法的问题：
这个工程能不能加 C++、编不过时能不能拿到报错。

```
cpp_probe                    → 能怎么编、引擎源码在哪
cpp_list_modules             → 这个类该写进哪个模块
（写代码：read_local_file / edit_local_file）
cpp_compile                  → 编进正在跑的编辑器，不需要用户关编辑器
```

## Workflow

### 改一个已有的类

`read_local_file` 看清楚 → `edit_local_file` 改 → `cpp_compile`。
编不过就读诊断、改、再编。这条路不需要用户做任何事。

### 新建一个类 —— 现在做不了，交给用户

**没有建类工具。** 用户要一个新的 C++ 类时，请他在编辑器里手动建一次：

> 菜单 **工具 > 新建 C++ 类**，选好父类和模块，点创建。

建完告诉我文件在哪，**后面改实现和编译我全都能接**。

原因（2026-09-08 真机实测）：引擎的建类 API 里有一步会把引擎和所有插件的源码
全扫一遍，同步跑在游戏线程上，在真实工程上不返回 —— 编辑器停在
「正在添加代码到项目…… 14%」，10 分钟也等不到结果。所以这个工具已经摘掉了。

**也不要用 `write_local_file` 自己造 `.h`/`.cpp` 来顶替。** 建类要处理类名校验、
`.generated.h`、模块 API 宏、`.uproject` 依赖登记，自己写会漏，产出和用户在
编辑器里建的不一样，而且很容易编不过。

### 改模块依赖

`edit_local_file` 改 `.Build.cs` → **这类改动热重载和 Live Coding 都补不上**，
需要关掉编辑器完整重新链接。`cpp_compile` 的返回里 `needs_full_rebuild` 为真就是这个意思。
把命令递给用户，不要自己去杀编辑器进程：

```
<Engine>/Build/BatchFiles/Build.bat <项目名>Editor Win64 Development -Project="<uproject 绝对路径>"
```

## Constraints

**1. 纯蓝图工程加不了 C++。** `cpp_probe` 回 `has_code: false` 且没有声明模块时，
停下来告诉用户：第一个 C++ 类要他在编辑器里手动加一次（菜单 工具 > 新建 C++ 类）。
这一步工具做不了 —— 引擎那条路在这种工程上会弹模态对话框，把编辑器卡死。

`has_code: false` **但**列得出模块，是另一回事：那是个源码缺失的 C++ 工程
（`.uproject` 声明了模块但 `Source/` 不在），加新类解决不了，让用户检查工程完整性。

**2. Live Coding 路编不过时拿不到报错。** `cpp_probe` 回 `compile_path: livecoding` 时，
编译失败只会告诉你「失败了」，没有文件名和行号 —— 那些只显示在 Live Coding 控制台窗口里，
不写任何日志文件（实测确认过）。这时候：

- **不要猜是哪一行错**，更不要编造一个行号去改；
- 建议用户在编辑器设置里关掉 Live Coding 后重启，那条路（热重载）能给出完整诊断。

**3. 不要凭记忆写 UE API。** UE 的 API 在 5.0–5.8 之间会变（函数改名、参数增减、
导出宏变化）。拿不准某个函数的签名，就用 `cpp_probe` 给的 `engine_source_dir` 去
`grep_local_files` 读用户这台机器上这个版本的头文件。编不过是好结果，
编过了但行为不对才是坏结果。

**4. 模块类型决定能 include 什么。** `cpp_list_modules` 的 `type` 字段：

| 类型 | 能 include | 典型坑 |
|---|---|---|
| `Runtime` | 只能用运行时模块 | include `UnrealEd`/`LevelEditor` 在编辑器里编得过，**打包成游戏时链接失败** |
| `Editor` | 编辑器模块随便用 | 里面的代码打包后不存在，别把游戏逻辑写这儿 |

这是最典型的「开发时全绿、打包时全红」。选模块时先看类型。

**5. UHT 的反射规则**（编译期报错，措辞对不熟 UE 的人很难懂）：

| 现象 | 原因 |
|---|---|
| `Unrecognized type 'FMyStruct'` | 结构体没标 `USTRUCT()`，或者头文件没 include |
| `Missing '*' in Emitting class` | `UPROPERTY` 指向 UObject 派生类时必须用指针 |
| `Type 'TArray<FMyStruct>' is not supported by blueprint` | 元素类型没标 `BlueprintType` |
| `Superclass ... of class ... not found` | 父类头文件没 include，或者父类模块不在依赖里 |
| 改了 `UPROPERTY` 之后热重载崩溃 | 改反射结构不适合热补丁，关编辑器完整重编 |

`.generated.h` 必须是头文件里**最后一个** include。

## Failure Handling

| 情况 | 怎么办 |
|---|---|
| `cpp_compile` 回 `NoChanges` | 确认文件真的保存了，且改的是这个工程正在用的那份源码 |
| `cpp_compile` 回 `Timeout` | 说「不知道成没成」，请用户看一眼编辑器，**不要报成功** |
| 诊断里没有文件名行号 | 走的是 Live Coding 路，见 Constraints 2，不要猜 |
| 编译反复失败且原因不明 | 建议关编辑器在 IDE 里完整编一次，那里的报错最全 |

## Escalation

这几件事工具做不到，必须交回给用户，不要试图绕过：

- 关闭编辑器（`cpp_compile` 不需要，但完整重编需要）
- **新建任何 C++ 类**（不只是第一个）—— 请用户走编辑器菜单 工具 > 新建 C++ 类
- 关闭 / 开启 Live Coding（那是用户的编辑器设置，工具不主动改）
- 修改引擎自身源码、打包、安装 Visual Studio
