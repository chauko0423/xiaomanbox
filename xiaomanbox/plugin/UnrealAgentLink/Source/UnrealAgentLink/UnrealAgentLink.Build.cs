// Copyright Epic Games, Inc. All Rights Reserved.
// Rebuild triggered
// Rebuild triggered

using UnrealBuildTool;

public class UnrealAgentLink : ModuleRules
{
	public UnrealAgentLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// 添加子目录到 include 路径
		PublicIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(ModuleDirectory, "Public/Core"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Utils"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Network"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Extensions"),
			}
		);
		
		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"BlueprintGraph",
				"GraphEditor",
				"KismetCompiler",
				"UnrealEd"
			}
		);
		
		PrivateIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(ModuleDirectory, "Private/Core"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Utils"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Network"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Extensions"),
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"JsonUtilities",
				"WebSockets",
				"BlueprintGraph",
				"UnrealEd",
				"KismetCompiler",
				"GraphEditor" // Ensure editor graph headers are available
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",
				"EditorFramework",
				"EditorFramework",
				// "UnrealEd", // Moved to Public
				"Kismet",
				// "KismetCompiler", // Moved to Public
				// "BlueprintGraph", // Moved to Public
				"AssetTools",
				"AnimationBlueprintLibrary",
				"IKRig",
				"IKRigEditor",
				"AssetRegistry",
				"ToolMenus",
				"CoreUObject",
				"Engine",
				"PhysicsCore",
				"PythonScriptPlugin",
				"ContentBrowser",
				"Slate",
				"SlateCore",
				"RenderCore",
				"RHI",
				"GameProjectGeneration",
				"PropertyEditor", // Added for FPropertyEditorModule
				"MaterialEditor", // Added for FMaterialEditorUtilities
				"MediaAssets", // Added for FileMediaSource support
				"ImageWrapper",  // Added for screenshot support
				"UMG", // Added for FWidgetRenderer
				"UMGEditor", // Added for UWidgetBlueprint support
				"LevelEditor", // Added for SLevelViewport and FLevelEditorViewportClient
				"EngineSettings", // Added for UGameMapsSettings
				"MessageLog", // Added for FMessageLogModule (MessageLog commands)

				// 签出预检（UAL_ContentSafetyCommands）：ISourceControlModule / Provider / State。
				// 引擎内建模块（Engine/Source/Developer/SourceControl），5.0–5.8 都在，
				// 不是插件模块 —— 不会像 GameplayTagsEditor 那样引入对插件 DLL 的硬导入
				"SourceControl",

				// Sequencer 命令（UAL_SequencerCommands）。
				//
				// 只取 MovieScene 的**核心**三件套，不碰 SequencerScripting ——
				// 那是 Epic 的 Python 绑定层，5.0–5.8 之间的破坏性改名基本都发生在
				// 那里，而它包装的核心从 4.27 到 5.7 稳得多。
				// 的更正框。
				"MovieScene",        // UMovieScene / FMovieSceneBinding / 通道
				"MovieSceneTracks",  // UMovieSceneCameraCutTrack / UMovieSceneSubTrack / 3D 变换轨道
				"LevelSequence",     // ULevelSequence
				"CinematicCamera",   // ACineCameraActor（写相机关键帧用）

				// A 档工具扩展。
				// 模块名逐版本确认过，9 个引擎的 .Build.cs 都在。
				"AutomationController", // §2.5 自动化测试：IAutomationControllerModule
				"DeveloperSettings",    // §2.4 项目设置 schema：UDeveloperSettings 反射遍历

				// 试玩机器人寻路（UAL_PieBotCommands 的 pie.nav_path）。
				// 引擎运行时模块（Engine/Source/Runtime/NavigationSystem），5.0–5.8 都在，
				// 不是插件模块，不引入对插件 DLL 的硬导入
				"NavigationSystem",

				// 地形与地形 RVT（UAL_LandscapeCommands）。三个都是引擎本体模块
				// （Runtime/Landscape、Editor/LandscapeEditor、Editor/VirtualTexturingEditor），
				// 5.0–5.8 都在、不是插件 —— 新建地形的 Import、高度图读取、
				// RVT 体积边界计算只有这几处导出
				"Landscape",
				"LandscapeEditor",
				"VirtualTexturingEditor",

				// 代理诊断（UAL_ProxyDiagnostics）只用一个函数：Lws 给 WebSocket 套代理时
				// 读的那个代理地址（5.8 与之前不是同一个函数，见该 .cpp 顶部）。
				// 必须问同一个来源，自己另读一份 ini 会和实际行为对不上。
				// 引擎内建模块（Engine/Source/Runtime/Online/HTTP），5.0–5.8 都在，
				// 而且 WebSockets 本身已经依赖它，不引入新的 DLL 硬导入
				"HTTP",

				// 状态监控的本地缓存命中率（UAL_EditorHealth）：GatherDerivedDataCacheSummaryStats。
				// 引擎内建模块（Engine/Source/Developer/DerivedDataCache），5.0–5.8 都在，
				// 不是插件；那个函数 5.3 才有，5.0–5.2 在 .cpp 里按版本编掉
				"DerivedDataCache"

				// GameplayTags / GameplayTagsEditor 两个依赖**故意不加**。
				//
				// 维护者 2026-09-03 拍板：标签先只读，不新增依赖。
				//
				// 写标签要 IGameplayTagsEditorModule 的三个 INI 函数，而那是**插件模块**，
				// 硬链接会让我们的 DLL 硬导入 UnrealEditor-GameplayTagsEditor.dll；
				// 想躲开链接失败就得在 .uplugin 里声明依赖，那等于强制改用户的工程配置 ——
				// 正是 UAL_ReflectCall.h 里为 PCG 明确否决过的做法。
				//
				// 读标签那条路留到第 6 步再定：优先试反射（UBlueprintGameplayTagLibrary
				// 有 BlueprintCallable 的 UFUNCTION），反射走不通再回来谈要不要加依赖。
			}
			);

		/*
		 * Live Coding（UAL_CppCommands 的 cpp.probe 要读它的会话状态）。
		 *
		 * 接法照抄引擎自己的 UnrealEd.Build.cs / LevelEditor.Build.cs：
		 * **只加 include 路径，不进 Dependency**。这个模块只有 Windows 有
		 * （Engine/Source/Developer/Windows/LiveCoding），当依赖链进去会让
		 * mac/linux 直接编不过；正确做法是运行时
		 * FModuleManager::GetModulePtr<ILiveCodingModule>() 取指针，取不到就当没有。
		 *
		 * bWithLiveCoding 从 4.27 一路到 5.8 都在，不用做版本分支。
		 */
		if (Target.bWithLiveCoding)
		{
			PrivateIncludePathModuleNames.Add("LiveCoding");
		}

		// UE 5.1+ 可能需要额外模块，这里预留动态依赖
		// Note: Target.Version 仅在 UE 5.1+ 可用，使用 ReadOnlyBuildVersion 兼容 5.0
		#if UE_5_1_OR_LATER
		{
			// 示例：可在此添加 5.1+ 特有模块
		}
		#endif
	}
}
