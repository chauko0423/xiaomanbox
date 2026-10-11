#include "UAL_ContentBrowserCommands.h"
#include "UAL_ContentSafetyCommands.h"
#include "UAL_VersionCompat.h"
#include "UAL_CommandUtils.h"
#include "UAL_RegistryReady.h"
#include "UAL_ScopedLogCapture.h"
#include "UAL_SavablePackage.h"
#include "Utils/UAL_PBRMaterialHelper.h"
#include "Utils/UAL_NormalizedImporter.h"
#include "Utils/UAL_AgentUndo.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "PackageTools.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h" // content.fixup_redirectors：报告框到不到得了人手上
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "AssetImportTask.h"
#include "Factories/FbxImportUI.h"
#include "Animation/Skeleton.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
// GPixelFormats / PF_MAX：content.asset_ranking 用它把「Format 标签是 DXT1」
// 换算成字节数。5.0 在 RHI.h 里，5.5 起搬到 Core 的 PixelFormat.h，RHI.h 两边都能拿到。
#include "RHI.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Async/Async.h"
#include "FileMediaSource.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h" // UAL: 包含 GIsRunningUnattendedScript 用于无人值守模式删除

// 使用独立的 Log Category 名称，避免与 UAL_ContentBrowserExt 冲突
// 放在这里以便静态函数可以使用
DEFINE_LOG_CATEGORY_STATIC(LogUALContentCmd, Log, All);

// 视频文件扩展名集合（这些需要特殊处理：复制到 Movies 目录并创建 FileMediaSource）
static const TSet<FString> VideoFileExtensions = {
	TEXT("mp4"), TEXT("mov"), TEXT("avi"), TEXT("wmv"), TEXT("mkv"), TEXT("webm"),
	TEXT("m4v"), TEXT("flv"), TEXT("3gp"), TEXT("3g2"), TEXT("mxf"), TEXT("ts")
};

/**
 * 检查文件是否是视频文件
 * @param FilePath 文件路径
 * @return 如果是视频文件返回 true
 */
static bool IsVideoFile(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	return VideoFileExtensions.Contains(Extension);
}

/**
 * OBJ 体检 —— 在把文件递给引擎之前，拦掉会**当场崩掉编辑器**的那几种写法。
 *
 * ## 为什么必须在这里挡
 *
 * UE 5.5+ 的 OBJ 走 Interchange，它的翻译器不校验面里的索引，读到什么就直接
 * 拿去下标访问。最常见的写法错误是把 `f 1//1` 写成 `f 1/1` —— OBJ 里斜杠
 * 第二段是贴图坐标 vt，第三段才是法线 vn，只有法线时中间那段必须留空。
 * 少一个斜杠，每个面都在引用一份根本不存在的 UV，引擎在 TArray 里 check 失败：
 *
 *     Assertion failed: (Index >= 0) & (Index < ArrayNum) [Array.h:783]
 *     Array index out of bounds: 0 into an array of size 0
 *
 * check() 不是异常，接不住、也没有返回值可判 —— **整个编辑器当场退出**，
 * 用户没保存的东西全没。所以唯一的办法是别把这种文件递进去。
 * 2026-09-02 真机上连崩两次：第二次是编辑器重启、连接恢复之后，
 * 调用方把同一个文件原样重试的。
 *
 * 这不是一个完整的 OBJ 解析器，只查会导致越界的那几件事。查不出来的写法问题
 * （法线方向反了、面绕序不对）不归这里管，那些顶多是模型难看，不会崩。
 *
 * @param FilePath 待导入的 .obj 绝对路径
 * @param OutError 失败原因（英文，直接回给调用方，写清楚该怎么改）
 * @return 可以安全递给引擎返回 true
 */
static bool ValidateObjForImport(const FString& FilePath, FString& OutError)
{
	OutError.Empty();

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		// 读不出来是另一回事（文件被占用、权限），不是格式问题。
		// 这里不替引擎下判断，放行让它自己去报错
		return true;
	}

	const FString FileName = FPaths::GetCleanFilename(FilePath);

	// 已经定义了多少个 v / vt / vn（负数索引是相对当前位置的，必须边走边算）
	int32 Counts[3] = { 0, 0, 0 };
	// 正数索引要等全文件读完才能判越界（引用后面才定义的顶点是合法的）：
	// 先记下每一档用到的最大下标和它出现的行号
	int32 MaxRefs[3] = { 0, 0, 0 };
	int32 MaxRefLines[3] = { 0, 0, 0 };
	static const TCHAR* SlotNames[3] = { TEXT("vertex"), TEXT("texture coordinate"), TEXT("normal") };
	static const TCHAR* SlotKeywords[3] = { TEXT("v"), TEXT("vt"), TEXT("vn") };

	int32 FaceCount = 0;
	TArray<FString> Tokens;
	TArray<FString> Refs;

	for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
	{
		Tokens.Reset();
		Lines[LineIndex].ParseIntoArrayWS(Tokens);
		if (Tokens.Num() == 0 || Tokens[0].StartsWith(TEXT("#")))
		{
			continue;
		}

		const FString& Keyword = Tokens[0];
		if (Keyword == TEXT("v")) { ++Counts[0]; continue; }
		if (Keyword == TEXT("vt")) { ++Counts[1]; continue; }
		if (Keyword == TEXT("vn")) { ++Counts[2]; continue; }
		if (Keyword != TEXT("f")) { continue; }

		++FaceCount;
		const int32 LineNumber = LineIndex + 1;

		if (Tokens.Num() < 4)
		{
			OutError = FString::Printf(
				TEXT("%s: line %d defines a face with %d vertices; a face needs at least 3."),
				*FileName, LineNumber, Tokens.Num() - 1);
			return false;
		}

		for (int32 TokenIndex = 1; TokenIndex < Tokens.Num(); ++TokenIndex)
		{
			Refs.Reset();
			// 不能剔空段：`1//1` 中间那一段的「空」正是「没有贴图坐标」的意思，
			// 剔掉的话法线会被当成 UV 读，白白误报
			Tokens[TokenIndex].ParseIntoArray(Refs, TEXT("/"), /*InCullEmpty=*/false);

			for (int32 Slot = 0; Slot < Refs.Num() && Slot < 3; ++Slot)
			{
				if (Refs[Slot].IsEmpty())
				{
					continue;
				}

				const int32 Ref = FCString::Atoi(*Refs[Slot]);
				if (Ref == 0)
				{
					OutError = FString::Printf(
						TEXT("%s: line %d has an invalid %s index '%s'. OBJ indices start at 1."),
						*FileName, LineNumber, SlotNames[Slot], *Refs[Slot]);
					return false;
				}

				if (Ref < 0)
				{
					// 负数是「从当前已定义的最后一个往回数」，此刻就能判
					if (Counts[Slot] + Ref < 0)
					{
						OutError = FString::Printf(
							TEXT("%s: line %d references %s %d, counting back past the start of the file (only %d '%s' defined so far)."),
							*FileName, LineNumber, SlotNames[Slot], Ref, Counts[Slot], SlotKeywords[Slot]);
						return false;
					}
					continue;
				}

				if (Ref > MaxRefs[Slot])
				{
					MaxRefs[Slot] = Ref;
					MaxRefLines[Slot] = LineNumber;
				}
			}
		}
	}

	if (FaceCount > 0 && Counts[0] == 0)
	{
		OutError = FString::Printf(
			TEXT("%s: has %d faces but no 'v' vertex lines."), *FileName, FaceCount);
		return false;
	}

	for (int32 Slot = 0; Slot < 3; ++Slot)
	{
		if (MaxRefs[Slot] <= Counts[Slot])
		{
			continue;
		}

		if (Slot == 1 && Counts[1] == 0)
		{
			// 单独说这一档：它是最常见的那个错，而且原因和改法都很具体
			OutError = FString::Printf(
				TEXT("%s: line %d references texture coordinate %d, but the file has no 'vt' lines at all. ")
				TEXT("If those numbers are normals, faces must use a double slash - 'f 1//1', not 'f 1/1' ")
				TEXT("(the second slot is the UV index, the third is the normal). Importing this as-is crashes the editor."),
				*FileName, MaxRefLines[1], MaxRefs[1]);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("%s: line %d references %s %d, but the file only defines %d '%s' line(s)."),
				*FileName, MaxRefLines[Slot], SlotNames[Slot], MaxRefs[Slot], Counts[Slot], SlotKeywords[Slot]);
		}
		return false;
	}

	return true;
}

/**
 * 导入视频文件：复制到 Content/Movies 目录并创建 FileMediaSource 资产
 *
 * @param SourceFilePath 源视频文件的绝对路径
 * @param DestinationPath UE 资产路径（如 /Game/Imported/Media/Video）
 * @param bOverwrite 是否覆盖已存在的文件
 * @param NormalizedAssetName 可选的规范化资产名称（如果为空则使用原始文件名）
 * @param OutImportedAsset 输出：创建的 FileMediaSource 资产
 * @param OutError 输出：错误信息（如果失败）
 * @return 如果成功返回 true
 */
static bool ImportVideoFile(
	const FString& SourceFilePath,
	const FString& DestinationPath,
	bool bOverwrite,
	const FString& NormalizedAssetName,
	UFileMediaSource*& OutImportedAsset,
	FString& OutError)
{
	OutImportedAsset = nullptr;
	OutError.Empty();
	
	// 1. 验证源文件存在
	if (!FPaths::FileExists(SourceFilePath))
	{
		OutError = FString::Printf(TEXT("Source video file not found: %s"), *SourceFilePath);
		return false;
	}
	
	// 2. 获取项目的 Content/Movies 目录
	// 注意：使用 FPaths::ProjectDir() 而不是 FPaths::ProjectContentDir()
	// 因为 ProjectContentDir() 在某些情况下可能返回相对路径导致解析错误
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString MoviesDir = FPaths::Combine(ProjectDir, TEXT("Content"), TEXT("Movies"));
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Video import - ProjectDir: %s, MoviesDir: %s"), *ProjectDir, *MoviesDir);
	
	// 确保 Movies 目录存在
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*MoviesDir))
	{
		if (!FileManager.MakeDirectory(*MoviesDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create Movies directory: %s"), *MoviesDir);
			return false;
		}
		UE_LOG(LogUALContentCmd, Log, TEXT("Created Movies directory: %s"), *MoviesDir);
	}
	
	// 3. 确定资产名称（使用规范化名称或默认生成）
	FString AssetName;
	if (!NormalizedAssetName.IsEmpty())
	{
		// 使用前端提供的规范化名称
		AssetName = NormalizedAssetName;
	}
	else
	{
		// 默认使用 MS_ + 原始文件名
		AssetName = TEXT("MS_") + FPaths::GetBaseFilename(SourceFilePath);
	}
	
	// 4. 构建目标文件路径（使用规范化名称作为文件名）
	const FString Extension = FPaths::GetExtension(SourceFilePath);
	FString TargetFilePath = FPaths::Combine(MoviesDir, AssetName + TEXT(".") + Extension);
	
	// 检查目标文件是否已存在
	if (FPaths::FileExists(TargetFilePath))
	{
		if (bOverwrite)
		{
			// 删除已存在的文件
			if (!FileManager.Delete(*TargetFilePath))
			{
				OutError = FString::Printf(TEXT("Failed to delete existing file: %s"), *TargetFilePath);
				return false;
			}
		}
		else
		{
			// 生成唯一文件名
			int32 Counter = 1;
			FString BaseAssetName = AssetName;
			do
			{
				AssetName = FString::Printf(TEXT("%s_%d"), *BaseAssetName, Counter++);
				TargetFilePath = FPaths::Combine(MoviesDir, AssetName + TEXT(".") + Extension);
			} while (FPaths::FileExists(TargetFilePath) && Counter < 1000);
		}
	}
	
	// 5. 复制视频文件到 Movies 目录
	UE_LOG(LogUALContentCmd, Log, TEXT("Copying video file: %s -> %s"), *SourceFilePath, *TargetFilePath);
	
	const uint32 CopyResult = FileManager.Copy(*TargetFilePath, *SourceFilePath, true);
	if (CopyResult != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Failed to copy video file. Error code: %d"), CopyResult);
		return false;
	}
	
	// 6. 创建 FileMediaSource 资产
	// 构建资产包路径
	FString PackagePath = DestinationPath;
	if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/Game")))
	{
		PackagePath = TEXT("/Game/Imported/Media/Video");
	}
	
	const FString FullPackageName = PackagePath / AssetName;
	
	// 检查资产是否已存在
	UPackage* Package = CreatePackage(*FullPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *FullPackageName);
		return false;
	}
	
	// 创建 FileMediaSource 对象
	UFileMediaSource* MediaSource = NewObject<UFileMediaSource>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!MediaSource)
	{
		OutError = TEXT("Failed to create FileMediaSource object");
		return false;
	}
	
	// 6. 设置 FilePath 属性
	// 注意：SetFilePath() 会将相对路径解析为绝对路径，但使用的是当前工作目录（引擎 Binaries）作为基准
	// 所以我们需要直接设置完整的绝对路径 TargetFilePath
	// UE 在打包时会自动处理路径，将 Content/Movies 下的文件包含进包中
	MediaSource->SetFilePath(TargetFilePath);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Created FileMediaSource: %s with FilePath: %s"), 
		*FullPackageName, *TargetFilePath);
	
	// 7. 标记包为脏并保存
	Package->MarkPackageDirty();
	
	// 注册到 Asset Registry
	FAssetRegistryModule::AssetCreated(MediaSource);
	
	// 保存资产
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		FullPackageName, 
		FPackageName::GetAssetPackageExtension()
	);
	
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, MediaSource, *PackageFileName, SaveArgs);
	if (SaveResult.Result != ESavePackageResult::Success)
#else
	if (!UPackage::SavePackage(Package, MediaSource, *PackageFileName, SaveArgs))
#endif
	{
		UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to save FileMediaSource: %s"), *PackageFileName);
		// 即使保存失败，资产仍然存在于内存中，用户可以稍后手动保存
	}
	
	OutImportedAsset = MediaSource;
	return true;
}

/**
 * 注册所有内容浏览器命令
 */
void FUAL_ContentBrowserCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("content.search"), &Handle_SearchAssets);
	CommandMap.Add(TEXT("content.import"), &Handle_ImportAssets);
	CommandMap.Add(TEXT("content.move"), &Handle_MoveAsset);
	CommandMap.Add(TEXT("content.delete"), &Handle_DeleteAssets);
	CommandMap.Add(TEXT("content.describe"), &Handle_DescribeAsset);
	CommandMap.Add(TEXT("content.normalized_import"), &Handle_NormalizedImport);
	CommandMap.Add(TEXT("content.audit_optimization"), &Handle_AuditOptimization);
	// 全工程资源占用排行（只读注册表标签，不加载资产）
	CommandMap.Add(TEXT("content.asset_ranking"), &Handle_AssetRanking);
	// 一个资产连同依赖一共多大（编辑器 Size Map 的等价物）
	CommandMap.Add(TEXT("content.size_map"), &Handle_SizeMap);
	// 清理移动/改名留下的重定向器
	CommandMap.Add(TEXT("content.fixup_redirectors"), &Handle_FixupRedirectors);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("ContentBrowser commands registered: content.search, content.import, content.move, content.delete, content.describe, content.normalized_import, content.audit_optimization"));
}

// ============================================================================
// Handler 实现
// ============================================================================

/**
 * content.search - 搜索资产
 * 支持模糊匹配、类型过滤和目录限制
 * 
 * 参数:
 * - query: 搜索关键词（可选，默认 "*" 列出所有资产，使用 "*" 作为通配符）
 * - path: 目录路径限制，如 /Game/Blueprints（可选）
 * - filter_class: 类型过滤（可选）
 * - include_folders: 是否返回文件夹信息（可选，默认 false）
 * - limit: 返回数量限制（可选，默认 100，最大 500）
 */
void FUAL_ContentBrowserCommands::Handle_SearchAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析参数
	FString Query;
	Payload->TryGetStringField(TEXT("query"), Query);
	
	// 目录路径限制（新增参数）
	FString SearchPath;
	Payload->TryGetStringField(TEXT("path"), SearchPath);
	
	FString FilterClass;
	Payload->TryGetStringField(TEXT("filter_class"), FilterClass);
	
	// 是否返回文件夹信息（新增参数）
	bool bIncludeFolders = false;
	Payload->TryGetBoolField(TEXT("include_folders"), bIncludeFolders);
	
	// limit 默认 100，最大 500（提升上限）
	int32 Limit = 100;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 500);
	
	// 支持通配符：如果 query 为空或为 "*"，则匹配所有资产
	bool bMatchAll = Query.IsEmpty() || Query == TEXT("*");
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.search: query=%s, path=%s, filter_class=%s, include_folders=%d, limit=%d, match_all=%d"),
		*Query, *SearchPath, *FilterClass, bIncludeFolders, Limit, bMatchAll);
	
	// 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 构建过滤器
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	
	/**
	 * 路径限制：优先使用 path 参数，否则默认 /Game。
	 *
	 * 原来的判据是 `StartsWith("/Game")`，不满足就**静默退回 /Game** ——
	 * 于是搜 `/Engine/...` 或插件内容目录时，调用方拿到的是一份 /Game 的结果
	 * 却以为搜的是自己给的目录。任何以 `/` 开头的挂载路径都照收，
	 * 目录不存在时下面会照实回 0 条并说清楚。
	 */
	if (!SearchPath.IsEmpty() && SearchPath.StartsWith(TEXT("/")))
	{
		while (SearchPath.Len() > 1 && SearchPath.EndsWith(TEXT("/")))
		{
			SearchPath.LeftChopInline(1);
		}
		Filter.PackagePaths.Add(FName(*SearchPath));
	}
	else
	{
		SearchPath = TEXT("/Game");
		Filter.PackagePaths.Add(TEXT("/Game"));
	}
	
	// 类型过滤 - UE 5.1+ 使用 ClassPaths，5.0 使用 ClassNames
	if (!FilterClass.IsEmpty())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *FilterClass));
#else
		Filter.ClassNames.Add(FName(*FilterClass));
#endif
	}
	
	// 执行搜索
	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);
	
	// 收集文件夹信息（如果需要）
	TSet<FString> FolderPaths;
	
	/**
	 * 过滤匹配结果。
	 *
	 * ## 遍历不能在 Limit 处 break
	 *
	 * 原来匹配够 Limit 条就 `break`，响应里的 `count` 是**返回条数**，
	 * 没有任何字段告诉调用方「后面还有」。于是拿 limit=200 盘点一个工程的人，
	 * 会把「前 200 条」当成「全部」——2026-09-09 真机上就是这么漏掉了
	 * `/Game/LakeVilla`（164 个资产，工程的主内容）和 `/Game/EasyFog`（33 个），
	 * 报告写完了才在别的命令的排行榜里发现它们。
	 *
	 * 现在照样只**返回** Limit 条，但匹配数、目录清单都按**全量**统计：
	 * `total` 是真实匹配数，`truncated` 明说被截了，
	 * `folders` 带每个目录的资产数 —— 那正好就是「项目目录总览」。
	 * 多出来的成本只是把注册表里已有的 FAssetData 多走一遍，不加载任何东西。
	 */
	TArray<TSharedPtr<FJsonValue>> Results;
	TMap<FString, int32> FolderCounts;
	int32 TotalMatches = 0;
	for (const FAssetData& Asset : AssetList)
	{
		const FString AssetName = Asset.AssetName.ToString();
		const FString PackagePath = Asset.PackageName.ToString();

		// 通配符匹配或模糊匹配: 名称或路径包含查询字符串
		bool bMatches = bMatchAll ||
			AssetName.Contains(Query, ESearchCase::IgnoreCase) ||
			PackagePath.Contains(Query, ESearchCase::IgnoreCase);

		if (!bMatches)
		{
			continue;
		}

		++TotalMatches;

		// 目录统计按全量走，截断不影响它 —— 盘点靠的就是这份清单
		if (bIncludeFolders)
		{
			const FString FolderPath = FPackageName::GetLongPackagePath(PackagePath);
			FolderPaths.Add(FolderPath);
			FolderCounts.FindOrAdd(FolderPath)++;
		}

		if (Results.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), AssetName);
		Item->SetStringField(TEXT("path"), PackagePath);

		// UE 5.1+ 使用 AssetClassPath，5.0 使用 AssetClass
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
#else
		Item->SetStringField(TEXT("class"), Asset.AssetClass.ToString());
#endif

		Results.Add(MakeShared<FJsonValueObject>(Item));
	}

	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetNumberField(TEXT("count"), Results.Num());
	Response->SetNumberField(TEXT("total"), TotalMatches);
	Response->SetBoolField(TEXT("truncated"), TotalMatches > Results.Num());
	Response->SetStringField(TEXT("searched_path"), SearchPath);
	Response->SetArrayField(TEXT("results"), Results);

	if (TotalMatches == 0 && !AssetRegistry.PathExists(SearchPath) && SearchPath != TEXT("/Game"))
	{
		// 目录压根不存在和「目录存在但没东西」是两件事，别让调用方去猜
		Response->SetStringField(
			TEXT("note"),
			FString::Printf(TEXT("No asset registry path '%s' - check the mount point (project content is /Game, plugin content is /<PluginName>)."), *SearchPath));
	}

	// 如果需要，添加文件夹信息
	if (bIncludeFolders && FolderPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FolderArray;
		TArray<FString> SortedFolders;
		FolderCounts.GetKeys(SortedFolders);
		SortedFolders.Sort();
		TSharedPtr<FJsonObject> CountsJson = MakeShared<FJsonObject>();
		for (const FString& Folder : SortedFolders)
		{
			FolderArray.Add(MakeShared<FJsonValueString>(Folder));
			CountsJson->SetNumberField(Folder, FolderCounts[Folder]);
		}
		Response->SetArrayField(TEXT("folders"), FolderArray);
		Response->SetNumberField(TEXT("folder_count"), SortedFolders.Num());
		// 每个目录装了多少个 —— 整理工程的第一步问的就是这个
		Response->SetObjectField(TEXT("folder_counts"), CountsJson);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}


/**
 * content.import - 导入外部文件
 * 将磁盘上的文件导入到 UE 项目中
 * 使用 UAssetImportTask 实现无弹窗自动化导入（类似 Quixel Bridge）
 */
void FUAL_ContentBrowserCommands::Handle_ImportAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析 files 数组
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'files' array"));
		return;
	}
	
	// 解析目标路径
	FString DestinationPath = TEXT("/Game/Imported");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);
	
	// 是否覆盖
	bool bOverwrite = false;
	Payload->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	
	// 缩放参数：只补引擎不做的那部分单位差异（详见下面应用处的长注释）
	// OBJ 默认 100.0（格式无单位定义，按米假设）；glTF/GLB 和 FBX 默认 1.0（引擎自己已换算）
	// 可通过 JSON 显式指定覆盖默认值
	double ScaleOverride = -1.0; // -1 表示使用格式默认值
	Payload->TryGetNumberField(TEXT("scale"), ScaleOverride);
	
	// 验证 scale 参数：必须 > 0 或为 -1（自动）
	if (ScaleOverride != -1.0 && ScaleOverride <= 0.0)
	{
		UE_LOG(LogUALContentCmd, Warning, TEXT("Invalid scale value %.4f, must be > 0. Using format default."), ScaleOverride);
		ScaleOverride = -1.0;
	}
	
	// 目标骨架：只对 FBX 生效。给了就按「动画导入到这个骨架」处理。
	// 只有动画没有网格的 FBX（动捕、数字人动作包）自动化导入时引擎必须知道骨架，
	// 不给的话导入器什么都不产出、也不写日志 —— 调用方只能看到一句「没导进来」
	USkeleton* TargetSkeleton = nullptr;
	FString SkeletonPath;
	if (Payload->TryGetStringField(TEXT("skeleton"), SkeletonPath) && !SkeletonPath.IsEmpty())
	{
		// 允许只给包路径（/Game/X/SK_Foo），补成对象路径（/Game/X/SK_Foo.SK_Foo）
		FString SkeletonObjectPath = SkeletonPath;
		if (!SkeletonObjectPath.Contains(TEXT(".")))
		{
			SkeletonObjectPath += TEXT(".") + FPackageName::GetShortName(SkeletonObjectPath);
		}
		TargetSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonObjectPath);
		if (!TargetSkeleton)
		{
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("skeleton not found or not a Skeleton asset: %s (give the Skeleton asset path, e.g. /Game/Characters/Mannequin/SK_Mannequin_Skeleton)"),
				*SkeletonPath));
			return;
		}
	}
	
	// FBX 按什么导：auto | static_mesh | skeletal_mesh | animation。
	// 不给时：给了 skeleton 就是 animation（最常见：动作包挂到现有骨架），否则 auto。
	// skeletal_mesh + skeleton = 网格绑到现有骨架（换装、同骨架角色）
	FString FbxImportAs;
	Payload->TryGetStringField(TEXT("fbx_import_as"), FbxImportAs);
	FbxImportAs = FbxImportAs.ToLower();
	if (FbxImportAs.IsEmpty())
	{
		FbxImportAs = TargetSkeleton ? TEXT("animation") : TEXT("auto");
	}
	if (FbxImportAs != TEXT("auto") && FbxImportAs != TEXT("static_mesh")
		&& FbxImportAs != TEXT("skeletal_mesh") && FbxImportAs != TEXT("animation"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("invalid fbx_import_as '%s' (use auto / static_mesh / skeletal_mesh / animation)"), *FbxImportAs));
		return;
	}
	if (FbxImportAs == TEXT("animation") && !TargetSkeleton)
	{
		// 自动化导入纯动画必须有骨架，不拦的话引擎静默产出空
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("fbx_import_as=animation needs 'skeleton' (the target Skeleton asset path)"));
		return;
	}
	
	// FBX 动画重采样帧率。不给用引擎默认（按源文件帧率）
	int32 AnimFrameRate = 0;
	Payload->TryGetNumberField(TEXT("anim_frame_rate"), AnimFrameRate);
	
	// 解析 normalized_names 数组，建立文件名到规范化名称的映射
	// 格式: [{ "original": "原始文件名.ext", "normalized": "规范化名称" }, ...]
	TMap<FString, FString> NormalizedNameMap;
	const TArray<TSharedPtr<FJsonValue>>* NormalizedNamesArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("normalized_names"), NormalizedNamesArray) && NormalizedNamesArray)
	{
		for (const TSharedPtr<FJsonValue>& Item : *NormalizedNamesArray)
		{
			const TSharedPtr<FJsonObject>* ItemObj = nullptr;
			if (Item->TryGetObject(ItemObj) && ItemObj)
			{
				FString Original, Normalized;
				(*ItemObj)->TryGetStringField(TEXT("original"), Original);
				(*ItemObj)->TryGetStringField(TEXT("normalized"), Normalized);
				if (!Original.IsEmpty() && !Normalized.IsEmpty())
				{
					// 移除扩展名，用文件名（不含扩展名）作为键
					FString OriginalBaseName = FPaths::GetBaseFilename(Original);
					NormalizedNameMap.Add(OriginalBaseName, Normalized);
					UE_LOG(LogUALContentCmd, Log, TEXT("Name mapping: %s -> %s"), *OriginalBaseName, *Normalized);
				}
			}
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.import: %d files -> %s, overwrite=%d, name_mappings=%d"),
		FilesArray->Num(), *DestinationPath, bOverwrite, NormalizedNameMap.Num());
	
	// 收集导入结果（提前声明，用于汇总视频和普通文件的导入结果）
	TArray<TSharedPtr<FJsonValue>> ImportedResults;
	int32 SuccessCount = 0;
	/**
	 * 按**输入文件**数的成败，和上面按资产数的 SuccessCount 分开。
	 *
	 * 一个 FBX 能导出网格 + 材质 + 贴图好几个资产，自动生成的 PBR 材质也会进
	 * ImportedResults —— 拿资产数当「成功了几个文件」，5 个文件挂了 4 个照样能
	 * 数出 ok=true、imported 6/5。AGENTS.md §5 第 14 条：回执按调用方给的那份清单
	 * 逐条交代，每个输入文件要么在成功里，要么在 failed 里带着原因。
	 */
	int32 SucceededFileCount = 0;
	const int32 TotalRequestCount = FilesArray->Num();
	TArray<TSharedPtr<FJsonValue>> FailedFiles;
	FString FirstFailReason;
	auto AddFailedFile = [&FailedFiles, &FirstFailReason](const FString& File, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Failed = MakeShared<FJsonObject>();
		Failed->SetStringField(TEXT("file"), File);
		Failed->SetStringField(TEXT("reason"), Reason);
		FailedFiles.Add(MakeShared<FJsonValueObject>(Failed));
		if (FirstFailReason.IsEmpty())
		{
			FirstFailReason = FString::Printf(TEXT("%s: %s"), *File, *Reason);
		}
	};

	// === 第一阶段：分离视频文件和其他文件 ===
	TArray<FString> VideoFiles;
	TArray<FString> OtherFiles;
	
	int32 EntryIndex = -1;
	for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
	{
		++EntryIndex;
		FString FilePath;
		if (!FileValue.IsValid() || !FileValue->TryGetString(FilePath) || FilePath.IsEmpty())
		{
			// 空条目以前直接跳过，却仍算在调用方的清单里 —— 数不上号，照实记一笔
			AddFailedFile(FString::Printf(TEXT("#%d"), EntryIndex), TEXT("empty or non-string entry in 'files'"));
			continue;
		}
		{
			// 验证文件存在
			if (!FPaths::FileExists(FilePath))
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("File not found: %s"), *FilePath);
				// 以前只进日志、也不算进 requested_count，调用方看到的是「全部成功」
				AddFailedFile(FilePath, TEXT("file not found on disk"));
				continue;
			}

			// 检查是否是视频文件
			if (IsVideoFile(FilePath))
			{
				VideoFiles.Add(FilePath);
				UE_LOG(LogUALContentCmd, Log, TEXT("Detected video file: %s"), *FilePath);
			}
			else
			{
				OtherFiles.Add(FilePath);
			}
		}
	}
	
	// === 第二阶段：导入视频文件（特殊处理） ===
	if (VideoFiles.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, TEXT("Processing %d video file(s) with special import logic..."), VideoFiles.Num());
		
		for (const FString& VideoFilePath : VideoFiles)
		{
			UFileMediaSource* ImportedMediaSource = nullptr;
			FString ImportError;
			
			// 查找该视频文件的规范化名称
			FString VideoBaseName = FPaths::GetBaseFilename(VideoFilePath);
			const FString* NormalizedVideoName = NormalizedNameMap.Find(VideoBaseName);
			FString FinalVideoAssetName = NormalizedVideoName ? *NormalizedVideoName : FString();
			
			if (!FinalVideoAssetName.IsEmpty())
			{
				UE_LOG(LogUALContentCmd, Log, TEXT("Video file normalized name: %s -> %s"), *VideoBaseName, *FinalVideoAssetName);
			}
			
			if (ImportVideoFile(VideoFilePath, DestinationPath, bOverwrite, FinalVideoAssetName, ImportedMediaSource, ImportError))
			{
				if (ImportedMediaSource)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), ImportedMediaSource->GetName());
					Item->SetStringField(TEXT("path"), ImportedMediaSource->GetPathName());
					Item->SetStringField(TEXT("class"), TEXT("FileMediaSource"));
					Item->SetStringField(TEXT("source_file"), VideoFilePath);
					ImportedResults.Add(MakeShared<FJsonValueObject>(Item));
					SuccessCount++;
					SucceededFileCount++;
					
					UE_LOG(LogUALContentCmd, Log, TEXT("Successfully imported video: %s -> %s"), 
						*VideoFilePath, *ImportedMediaSource->GetPathName());
				}
				else
				{
					AddFailedFile(VideoFilePath, TEXT("video import reported success but returned no FileMediaSource"));
				}
			}
			else
			{
				UE_LOG(LogUALContentCmd, Error, TEXT("Failed to import video file: %s - %s"), 
					*VideoFilePath, *ImportError);
				AddFailedFile(VideoFilePath, ImportError.IsEmpty() ? FString(TEXT("video import failed (no reason given)")) : ImportError);
			}
		}
	}
	
	// === 第三阶段：导入其他文件（标准导入流程） ===
	TArray<UAssetImportTask*> ImportTasks;
	// 体检没过、根本没递给引擎的文件。逐个记原因，最后一起回给调用方 ——
	// 只回一句「导入失败」的话，调用方唯一能做的就是原样重试
	TArray<TSharedPtr<FJsonValue>> RejectedFiles;
	FString FirstRejectReason;

	for (const FString& FilePath : OtherFiles)
	{
		// 递给引擎之前先体检：某些格式错误会让引擎 check 失败，
		// 那是**整个编辑器退出**，不是一次失败的导入（见 ValidateObjForImport）
		if (FPaths::GetExtension(FilePath).ToLower() == TEXT("obj"))
		{
			FString ObjError;
			if (!ValidateObjForImport(FilePath, ObjError))
			{
				UE_LOG(LogUALContentCmd, Error, TEXT("Refusing to import malformed OBJ: %s"), *ObjError);

				TSharedPtr<FJsonObject> Rejected = MakeShared<FJsonObject>();
				Rejected->SetStringField(TEXT("file"), FilePath);
				Rejected->SetStringField(TEXT("reason"), ObjError);
				RejectedFiles.Add(MakeShared<FJsonValueObject>(Rejected));
				if (FirstRejectReason.IsEmpty())
				{
					FirstRejectReason = ObjError;
				}
				// rejected[] 留着给老调用方；failed[] 是这批每个没办成的输入文件的全集
				AddFailedFile(FilePath, ObjError);
				continue;
			}
		}

		// 创建导入任务
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = FilePath;
		Task->DestinationPath = DestinationPath;
		
		// 关键设置：禁用所有UI，实现无弹窗导入
		Task->bAutomated = true;
		// 不自动保存，避免触发源码管理检出对话框
		// 资产将保持未保存状态，用户可稍后手动保存
		Task->bSave = false;
		Task->bReplaceExisting = bOverwrite;
		
		// 覆盖 + 点名：直接以目标名导入，引擎就地替换现有资产（引用不断）。
		// 走「先按文件名导、再改名」的话，目标名已存在 → 改名被跳过，
		// 等于新建了一份、旧的一个字没动 —— 「替换项目里现有的动画」就是这样没做成的
		if (bOverwrite)
		{
			if (const FString* WantedName = NormalizedNameMap.Find(FPaths::GetBaseFilename(FilePath)))
			{
				Task->DestinationName = *WantedName;
			}
		}
		
		// 获取文件扩展名
		FString Extension = FPaths::GetExtension(FilePath).ToLower();
		
		// 为 FBX 文件配置自动导入选项
		if (Extension == TEXT("fbx"))
		{
			UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
			ImportUI->bImportMaterials = true;
			ImportUI->bImportTextures = true;
			
			// 两条导入管线读的字段不一样，两边都得照顾到：
			// - 旧 FbxFactory 看 bAutomatedImportShouldDetectType，自己探测后覆盖 MeshTypeToImport
			// - Interchange（5.5+ 默认）经 InterchangeFbxAssetImportDataConverter 转换，
			//   **只看** MeshTypeToImport / bImportAsSkeletal / bImportMesh，探测开关不管用。
			//   MeshTypeToImport 默认值是 FBXIT_StaticMesh，不改的话照样全压成静态网格
			if (FbxImportAs == TEXT("animation"))
			{
				// 只要 AnimSequence，不建网格/材质
				ImportUI->bAutomatedImportShouldDetectType = false;
				ImportUI->MeshTypeToImport = FBXIT_Animation;
				ImportUI->bImportAsSkeletal = true;
				ImportUI->bImportMesh = false;
				ImportUI->bImportAnimations = true;
				ImportUI->bImportMaterials = false;
				ImportUI->bImportTextures = false;
			}
			else if (FbxImportAs == TEXT("skeletal_mesh"))
			{
				ImportUI->bAutomatedImportShouldDetectType = false;
				ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
				ImportUI->bImportAsSkeletal = true;
				ImportUI->bImportMesh = true;
				ImportUI->bImportAnimations = true;
			}
			else if (FbxImportAs == TEXT("static_mesh"))
			{
				ImportUI->bAutomatedImportShouldDetectType = false;
				ImportUI->MeshTypeToImport = FBXIT_StaticMesh;
			}
			else
			{
				// auto。以前写死静态网格：骨骼网格被拍扁、纯动画 FBX 什么都导不出来。
				// FBXIT_MAX 让 Interchange 走「各类型都导」分支；旧管线会探测后覆盖掉它
				ImportUI->bAutomatedImportShouldDetectType = true;
				ImportUI->MeshTypeToImport = FBXIT_MAX;
				ImportUI->bImportAnimations = true;
			}
			if (AnimFrameRate > 0)
			{
				ImportUI->AnimSequenceImportData->bUseDefaultSampleRate = false;
				ImportUI->AnimSequenceImportData->CustomSampleRate = AnimFrameRate;
			}
			// 挂到现有骨架（skeletal_mesh / animation）；null 就新建骨架
			ImportUI->Skeleton = TargetSkeleton;
			
			// scale 对静态网格在导入后改 BuildScale3D（见下面），骨骼网格和动画没有
			// 那个口子，只能导入时给 —— 以前这两类给了 scale 也被静默忽略
			if (ScaleOverride > 0)
			{
				ImportUI->SkeletalMeshImportData->ImportUniformScale = ScaleOverride;
				ImportUI->AnimSequenceImportData->ImportUniformScale = ScaleOverride;
			}
			
			// 应用到任务
			Task->Options = ImportUI;
			
			UE_LOG(LogUALContentCmd, Log, TEXT("Configured FBX import for: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Using default import settings for: %s (Extension: %s)"), *FilePath, *Extension);
		}
		
		ImportTasks.Add(Task);
	}
	
	if (ImportTasks.Num() == 0 && VideoFiles.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			!FirstRejectReason.IsEmpty() ? FirstRejectReason
			: !FirstFailReason.IsEmpty() ? FirstFailReason
			: FString(TEXT("No valid files to import")));
		return;
	}
	// === 第四阶段：执行标准导入任务 ===
	if (ImportTasks.Num() > 0)
	{
		// 获取 AssetTools
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();
		
		// 执行批量导入任务（无弹窗）
		UE_LOG(LogUALContentCmd, Log, TEXT("Executing %d automated import tasks..."), ImportTasks.Num());
		AssetTools.ImportAssetTasks(ImportTasks);
	}
	
	// 收集标准导入的结果（追加到 ImportedResults）
	TArray<UTexture2D*> ImportedTextures;
	TArray<UStaticMesh*> ImportedMeshes;
	
	for (UAssetImportTask* Task : ImportTasks)
	{
		// 检查任务是否成功（通过ImportedObjectPaths检查）
		if (Task->ImportedObjectPaths.Num() > 0)
		{
			// 获取源文件名（不含扩展名），用于查找规范化名称
			const FString SourceBaseName = FPaths::GetBaseFilename(Task->Filename);
			
			// 这个文件至少落下一个能加载的资产才算这个输入文件成功
			bool bAnyAssetLoaded = false;

			// 复制数组以避免在重命名操作中修改原数组导致崩溃
			// ("Array has changed during ranged-for iteration" bug fix)
			TArray<FString> ObjectPathsCopy = Task->ImportedObjectPaths;
			for (const FString& ObjectPath : ObjectPathsCopy)
			{
				// 检查路径是否为空
				if (ObjectPath.IsEmpty())
				{
					UE_LOG(LogUALContentCmd, Warning, TEXT("Skipping empty ObjectPath in import task for: %s"), *Task->Filename);
					continue;
				}
				
				// 加载导入的资产
				UObject* ImportedAsset = LoadObject<UObject>(nullptr, *ObjectPath);
				if (ImportedAsset)
				{
					FString FinalAssetName = ImportedAsset->GetName();
					FString FinalAssetPath = ImportedAsset->GetPathName();
					
					// 检查是否需要重命名（如果有规范化名称映射）
					// 首先尝试用当前资产名查找，然后尝试用源文件名查找
					const FString* NormalizedName = NormalizedNameMap.Find(ImportedAsset->GetName());
					if (!NormalizedName)
					{
						NormalizedName = NormalizedNameMap.Find(SourceBaseName);
					}
					
					if (NormalizedName && !NormalizedName->IsEmpty() && *NormalizedName != ImportedAsset->GetName())
					{
						// 获取资产所在的包路径
						FString PackagePath = FPackageName::GetLongPackagePath(ImportedAsset->GetOutermost()->GetName());
						
						// 冲突检测：检查目标名称是否已存在（内存中或磁盘上）
						FString TargetPackagePath = PackagePath / *NormalizedName;
						bool bTargetExists = false;
						
						// 优先用 AssetRegistry 检查（能发现磁盘上未加载的资产）
						FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
						TArray<FAssetData> ExistingAssets;
						ARModule.Get().GetAssetsByPackageName(FName(*TargetPackagePath), ExistingAssets);
						bTargetExists = ExistingAssets.Num() > 0;
						
						// 回退：也检查内存中刚创建但未注册的对象
						if (!bTargetExists)
						{
							FString TargetObjectPath = TargetPackagePath + TEXT(".") + *NormalizedName;
							bTargetExists = StaticFindObject(UObject::StaticClass(), nullptr, *TargetObjectPath) != nullptr;
						}
						
						if (bTargetExists)
						{
							UE_LOG(LogUALContentCmd, Log, TEXT("Skipping rename: target already exists: %s"), *TargetPackagePath);
						}
						else
						{
							UE_LOG(LogUALContentCmd, Log, TEXT("Renaming asset: %s -> %s"), 
								*ImportedAsset->GetName(), **NormalizedName);
							
							// 使用 AssetTools 重命名资产
							FAssetToolsModule& AssetToolsMod = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
							IAssetTools& AssetToolsRef = AssetToolsMod.Get();
							
							TArray<FAssetRenameData> RenameData;
							// 使用 TWeakObjectPtr<UObject> 构造函数，兼容所有 UE5 版本
							RenameData.Add(FAssetRenameData(ImportedAsset, PackagePath, *NormalizedName));
							
							bool bRenameSuccess = AssetToolsRef.RenameAssets(RenameData);
							if (bRenameSuccess)
							{
								// 改名后从对象本身回读。原来拼的是 PackagePath/Name —— 那是包路径，
								// 不是对象路径（少了 .Name），和改名前、视频分支给的格式都对不上
								FinalAssetName = ImportedAsset->GetName();
								FinalAssetPath = ImportedAsset->GetPathName();
								UE_LOG(LogUALContentCmd, Log, TEXT("Successfully renamed asset to: %s"), *FinalAssetPath);
							}
							else
							{
								UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to rename asset: %s -> %s"), 
									*ImportedAsset->GetName(), **NormalizedName);
							}
						}
					}
					
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), FinalAssetName);
					Item->SetStringField(TEXT("path"), FinalAssetPath);
					Item->SetStringField(TEXT("class"), ImportedAsset->GetClass()->GetName());
					ImportedResults.Add(MakeShared<FJsonValueObject>(Item));
					SuccessCount++;
					bAnyAssetLoaded = true;
					
					// 🎨 收集纹理和网格体，用于PBR材质生成
					if (UTexture2D* Texture = Cast<UTexture2D>(ImportedAsset))
					{
						ImportedTextures.Add(Texture);
					}
					else if (UStaticMesh* Mesh = Cast<UStaticMesh>(ImportedAsset))
					{
						ImportedMeshes.Add(Mesh);
						
						// 缩放补偿：只补引擎**不管**的那一类，别去补引擎已经补过的。
						//
						// glTF/GLB：引擎自己就做 米→厘米。5.5+ 的 Interchange 走
						//   InterchangeGltfPrivate.h 的 `GltfUnitConversionMultiplier = 100.f`
						//   （InterchangeGltfMesh.cpp 里 MeshFactory.SetUniformScale 掉它），
						//   5.0/5.1/5.2 的旧 GLTFImporter 走 `ImportScale(100.f)`。
						//   我们再乘 100 就是 10000 倍 —— 一只 1 米的皮卡丘会长到 100 米。
						//   所以这里必须是 1.0。
						// OBJ：格式本身没有单位定义，Interchange 的 OBJ 翻译器一个换算都不做
						//   （整个 InterchangeOBJTranslator.cpp 里没有 SetUniformScale）。
						//   而 DCC / AIGC 导出的 OBJ 事实上按米，所以这里替用户假设米 → ×100。
						//   这是**约定**不是换算，用户给了 scale 就以用户为准。
						// FBX：文件内自带单位元数据，引擎自己处理，1.0。
						FString SourceExt = FPaths::GetExtension(Task->Filename).ToLower();
						double MeshScale = ScaleOverride;
						if (MeshScale < 0) // 未显式指定，使用格式默认值
						{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
							// OBJ 的静态网格导入是 5.5 的 Interchange 才有的，旧版进不来，不用管
							MeshScale = (SourceExt == TEXT("obj")) ? 100.0 : 1.0;
#else
							MeshScale = 1.0;
#endif
						}
						
						if (!FMath::IsNearlyEqual(MeshScale, 1.0))
						{
							if (Mesh->GetNumSourceModels() > 0)
							{
								// 对所有 LOD 应用缩放，避免 LOD 间尺寸不一致
								for (int32 LODIdx = 0; LODIdx < Mesh->GetNumSourceModels(); ++LODIdx)
								{
									FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(LODIdx);
									SourceModel.BuildSettings.BuildScale3D = FVector(MeshScale);
								}
								Mesh->Build();
								Mesh->MarkPackageDirty();
								
								UE_LOG(LogUALContentCmd, Log, TEXT("Applied scale %.1f to mesh: %s (%d LODs, format: %s)"),
									MeshScale, *Mesh->GetName(), Mesh->GetNumSourceModels(), *SourceExt);
							}
						}
					}
				}
			}

			if (bAnyAssetLoaded)
			{
				SucceededFileCount++;
			}
			else
			{
				AddFailedFile(Task->Filename, TEXT("the engine reported imported object paths but none of them could be loaded"));
			}
		}
		else
		{
			UE_LOG(LogUALContentCmd, Warning, TEXT("No assets imported from: %s"), *Task->Filename);
			const bool bIsFbx = FPaths::GetExtension(Task->Filename).ToLower() == TEXT("fbx");
			if (bIsFbx && TargetSkeleton)
			{
				// 骨架给了还是空的：几乎都是骨骼名对不上
				AddFailedFile(Task->Filename, FString::Printf(
					TEXT("animation import onto skeleton %s produced nothing - the FBX's bone names/hierarchy most likely don't match this skeleton, or the file has no animation take"),
					*TargetSkeleton->GetPathName()));
			}
			else if (bIsFbx)
			{
				// 最常见的是只有动画没有网格的 FBX：自动化导入不知道挂哪个骨架，就什么都不建
				AddFailedFile(Task->Filename, TEXT("the engine imported nothing from this FBX. If it contains only animation (no mesh), import again with 'skeleton' set to the target Skeleton asset path"));
			}
			else
			{
				// 引擎一个资产都没产出：格式没有对应的导入器，或导入器自己报错了（原因在 Output Log）
				AddFailedFile(Task->Filename, TEXT("the engine imported nothing from this file (no importer for this format, or the importer failed - see Output Log)"));
			}
		}
	}
	
	// 🚀 自动生成PBR材质（如果导入了纹理）
	TArray<UMaterialInstanceConstant*> CreatedMaterials;
	// 自动生成的附带资产单独数，不算进「输入文件成功了几个」
	int32 AutoGeneratedCount = 0;
	if (ImportedTextures.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, 
			TEXT("Starting automatic PBR material generation for %d textures..."), 
			ImportedTextures.Num());
		
		// 配置PBR处理选项
		FUAL_PBRMaterialOptions PBROptions;
		PBROptions.bApplyToMesh = true;           // 自动应用到网格体
		PBROptions.bUseStandardNaming = true;     // 使用标准命名（MI_前缀）
		PBROptions.bAutoConfigureTextures = true;  // 自动配置纹理设置
		
		// 批量处理PBR资产
		int32 MaterialCount = FUAL_PBRMaterialHelper::BatchProcessPBRAssets(
			ImportedTextures,
			ImportedMeshes,
			DestinationPath,
			PBROptions,
			CreatedMaterials);
		
		if (MaterialCount > 0)
		{
			UE_LOG(LogUALContentCmd, Log, 
				TEXT("✨ Successfully created %d PBR material(s) automatically!"), 
				MaterialCount);
			
			// 将创建的材质也添加到返回结果中
			for (UMaterialInstanceConstant* Material : CreatedMaterials)
			{
				if (Material)
				{
					TSharedPtr<FJsonObject> MatItem = MakeShared<FJsonObject>();
					MatItem->SetStringField(TEXT("name"), Material->GetName());
					MatItem->SetStringField(TEXT("path"), Material->GetPathName());
					MatItem->SetStringField(TEXT("class"), TEXT("MaterialInstanceConstant"));
					MatItem->SetBoolField(TEXT("auto_generated"), true);
					ImportedResults.Add(MakeShared<FJsonValueObject>(MatItem));
					SuccessCount++;
					AutoGeneratedCount++;
				}
			}
		}
	}
	
	// 落盘。
	//
	// 不存的话导入的资产只活在**当前编辑器会话**里：调用方收到「导入成功」，
	// 用户关掉 UE（不手动保存），资产就没了。真机上验到过 —— 重启编辑器后
	// 刚导入的贴图全部消失，而重命名过的那张（rename 内部会保存）还在。
	// 「导入」这件事对用户的含义就是文件进了工程，不落盘等于没做。
	int32 ImportSavedCount = 0;
	int32 ImportUnsavedCount = 0;
	{
		TSet<UPackage*> PackagesToSave;
		for (const TSharedPtr<FJsonValue>& Value : ImportedResults)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Item) || !Item) continue;

			FString ObjectPath;
			if (!(*Item)->TryGetStringField(TEXT("path"), ObjectPath) || ObjectPath.IsEmpty()) continue;

			if (UObject* Imported = LoadObject<UObject>(nullptr, *ObjectPath))
			{
				if (UPackage* Package = Imported->GetOutermost())
				{
					PackagesToSave.Add(Package);
				}
			}
		}

		int32 SavedCount = 0;
		for (UPackage* Package : PackagesToSave)
		{
			const FString FileName = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			if (UPackage::Save(Package, nullptr, *FileName, SaveArgs).Result == ESavePackageResult::Success)
#else
			if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
#endif
			{
				SavedCount++;
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Handle_ImportAssets: failed to save %s"), *Package->GetName());
			}
		}

		// 结果留给下面的 Response 用（Response 在通知逻辑之后才创建）
		ImportSavedCount = SavedCount;
		ImportUnsavedCount = PackagesToSave.Num() - SavedCount;
	}

	// Show notification (Ensure logic runs on GameThread)
	if (SucceededFileCount > 0)
	{
		// Capture by value
		AsyncTask(ENamedThreads::GameThread, [SuccessCount]()
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Handle_ImportAssets: Attempting to show success notification for %d assets"), SuccessCount);

			FString Title = UAL_CommandUtils::LStr(TEXT("导入成功"), TEXT("Import Successful"));
			FString Msg = FString::Printf(TEXT("%s: %d"), *UAL_CommandUtils::LStr(TEXT("成功导入资产数"), TEXT("Assets imported")), SuccessCount);
			
			FNotificationInfo Info(FText::FromString(Title));
			Info.SubText = FText::FromString(Msg);
			Info.ExpireDuration = 3.0f;
			Info.bFireAndForget = true;
			Info.bUseLargeFont = false;
			
			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem.IsValid())
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Handle_ImportAssets: Failed to create notification item"));
			}
		});
	}

	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	// ok 只看输入文件：自动生成的 PBR 材质是附带产物，不能把一批全挂的导入撑成 ok
	Response->SetBoolField(TEXT("ok"), SucceededFileCount > 0);
	if (SucceededFileCount == 0)
	{
		// 体检拦下来的，原因是具体的，别用那句什么都没说的通用错误盖掉
		Response->SetStringField(TEXT("error"),
			!FirstRejectReason.IsEmpty() ? FirstRejectReason
			: !FirstFailReason.IsEmpty() ? FirstFailReason
			: FString(TEXT("Failed to import assets. Possible reasons: 1) File type not supported by installed plugins, 2) Invalid file path. Check Output Log for details.")));
	}
	if (RejectedFiles.Num() > 0)
	{
		Response->SetArrayField(TEXT("rejected"), RejectedFiles);
		Response->SetNumberField(TEXT("rejected_count"), RejectedFiles.Num());
	}
	// failed_count 按输入文件数，含 rejected 里那些；有失败才出现，和 rejected 一个规矩
	if (FailedFiles.Num() > 0)
	{
		Response->SetArrayField(TEXT("failed"), FailedFiles);
		Response->SetNumberField(TEXT("failed_count"), FailedFiles.Num());
	}
	// imported_count 是 imported[] 里的资产数（一个文件可能出好几个资产），
	// succeeded_count 才是「几个输入文件导成了」，和 requested_count 同一把尺子
	Response->SetNumberField(TEXT("imported_count"), SuccessCount);
	Response->SetNumberField(TEXT("succeeded_count"), SucceededFileCount);
	Response->SetNumberField(TEXT("auto_generated_count"), AutoGeneratedCount);
	Response->SetNumberField(TEXT("requested_count"), TotalRequestCount);
	Response->SetArrayField(TEXT("imported"), ImportedResults);
	Response->SetNumberField(TEXT("saved_count"), ImportSavedCount);
	if (ImportUnsavedCount > 0)
	{
		Response->SetStringField(TEXT("save_warning"),
			FString::Printf(TEXT("%d 个资产已导入但未能写入磁盘，关闭编辑器后会丢失，请在编辑器里手动保存。"),
				ImportUnsavedCount));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.move - 移动/重命名资产
 * 移动资产或通过修改目标路径实现重命名
 */
void FUAL_ContentBrowserCommands::Handle_MoveAsset(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析参数
	FString SourcePath, DestinationPath;
	
	if (!Payload->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: source_path"));
		return;
	}
	
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: destination_path"));
		return;
	}
	
	bool bAutoRename = false;
	Payload->TryGetBoolField(TEXT("auto_rename"), bAutoRename);

	UE_LOG(LogUALContentCmd, Log, TEXT("content.move: %s -> %s, auto_rename=%d"), 
		*SourcePath, *DestinationPath, bAutoRename);
	
	// 加载源资产 - 支持 PackageName 和 ObjectPath 两种格式
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FAssetData SourceAsset;

	// **先补全再解析**，顺序不能反。
	//
	// 原来是「先按原样试，失败再补 .AssetName」。问题在于 UE 5.1+ 的
	// FSoftObjectPath 拿到一个只有包名的路径（/Game/X/Foo）会解析成**包**，
	// 返回一个 IsValid() 为真、但 GetAsset() 拿不到对象的 FAssetData ——
	// 于是第二次尝试被跳过，最后报 "Failed to load source asset object"。
	//
	// 这不是边角情况：`content.search` 返回的就是不带 .Object 的短路径，
	// 用户/模型拿搜索结果直接来重命名必踩，而错误信息完全看不出是格式问题。
	// content.describe 报 assetClass="Package" 也是同一个根因。
	FString ResolvedSourcePath = SourcePath;
	if (!SourcePath.Contains(TEXT(".")))
	{
		ResolvedSourcePath = SourcePath + TEXT(".") + FPaths::GetBaseFilename(SourcePath);
		UE_LOG(LogUALContentCmd, Log, TEXT("Normalised package path to object path: %s"), *ResolvedSourcePath);
	}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	SourceAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ResolvedSourcePath));
#else
	SourceAsset = AssetRegistry.GetAssetByObjectPath(FName(*ResolvedSourcePath));
#endif

	// 兜底：原样再试一次（调用方给的本来就是完整 ObjectPath 的情况）
	if (!SourceAsset.IsValid() && ResolvedSourcePath != SourcePath)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		SourceAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourcePath));
#else
		SourceAsset = AssetRegistry.GetAssetByObjectPath(FName(*SourcePath));
#endif
	}
	
	// 尝试3: 通过 PackageName 查找
	if (!SourceAsset.IsValid())
	{
		TArray<FAssetData> AssetList;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByPackageName(FName(*SourcePath), AssetList);
#else
		AssetRegistry.GetAssetsByPackageName(FName(*SourcePath), AssetList);
#endif
		if (AssetList.Num() > 0)
		{
			SourceAsset = AssetList[0];
			UE_LOG(LogUALContentCmd, Log, TEXT("Found via PackageName: %s"), *SourceAsset.PackageName.ToString());
		}
	}
	
	if (!SourceAsset.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Source asset not found: %s (tried ObjectPath, FullObjectPath, and PackageName)"), *SourcePath));
		return;
	}
	
	// 获取 AssetTools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();
	
	// 解析目标路径
	FString DestPackagePath, DestAssetName;
	DestinationPath.Split(TEXT("/"), &DestPackagePath, &DestAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	
	if (DestPackagePath.IsEmpty() || DestAssetName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Invalid destination_path format"));
		return;
	}
	
	// 检查目标是否存在，处理自动重命名
	FString FinalDestAssetName = DestAssetName;
	bool bRenamed = false;
	
	auto CheckAssetExists = [&](const FString& PackagePath, const FString& AssetName) -> bool {
		FString FullPath = PackagePath / AssetName + TEXT(".") + AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullPath));
		if (!ExistingAsset.IsValid())
		{
			ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(PackagePath / AssetName));
		}
#else
		FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FName(*FullPath));
		if (!ExistingAsset.IsValid())
		{
			ExistingAsset = AssetRegistry.GetAssetByObjectPath(FName(*(PackagePath / AssetName)));
		}
#endif
		// 还要检查是否只是包存在但没有资产
		if (!ExistingAsset.IsValid())
		{
			TArray<FAssetData> PkgAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*(PackagePath / AssetName)), PkgAssets);
			return PkgAssets.Num() > 0;
		}
		
		return ExistingAsset.IsValid();
	};
	
	if (CheckAssetExists(DestPackagePath, FinalDestAssetName))
	{
		if (bAutoRename)
		{
			int32 Suffix = 1;
			FString BaseName = DestAssetName;
			while (CheckAssetExists(DestPackagePath, FinalDestAssetName))
			{
				FinalDestAssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
				if (Suffix > 1000) break;
			}
			bRenamed = true;
			UE_LOG(LogUALContentCmd, Log, TEXT("Auto-renamed collision: %s -> %s"), *DestAssetName, *FinalDestAssetName);
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 409, 
				FString::Printf(TEXT("Asset already exists at destination: %s/%s"), *DestPackagePath, *DestAssetName));
			return;
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Move asset: %s -> %s/%s"), 
		*SourcePath, *DestPackagePath, *FinalDestAssetName);
	
	// 加载源资产对象
	UObject* SourceObject = SourceAsset.GetAsset();
	if (!SourceObject)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to load source asset object"));
		return;
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Source object loaded: %s (Class: %s)"), 
		*SourceObject->GetPathName(), *SourceObject->GetClass()->GetName());
	
	// 构建完整的新路径
	FString NewPackageName = DestPackagePath / FinalDestAssetName;
	
	UE_LOG(LogUALContentCmd, Log, TEXT("New package path: %s, New asset name: %s, Full new path: %s"), 
		*DestPackagePath, *FinalDestAssetName, *NewPackageName);
	
	// 构建重命名数据
	TArray<FAssetRenameData> RenameData;
	
	// 确保资产在内存中被正确标记
	SourceObject->MarkPackageDirty();
	
// 使用 TWeakObjectPtr<UObject> 构造函数，兼容所有 UE5 版本
	FAssetRenameData RenameItem(SourceObject, DestPackagePath, FinalDestAssetName);
	RenameData.Add(RenameItem);
	UE_LOG(LogUALContentCmd, Log, TEXT("FAssetRenameData: Object=%s, NewPath=%s, NewName=%s"), 
		*SourceObject->GetPathName(), *DestPackagePath, *FinalDestAssetName);
	
	// 执行移动/重命名
	bool bSuccess = AssetTools.RenameAssets(RenameData);
	
	// 验证移动是否真正成功（检查目标位置是否存在资产）
	FString NewAssetPath = DestPackagePath / FinalDestAssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewAssetPath + TEXT(".") + FinalDestAssetName));
#else
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FName(*(NewAssetPath + TEXT(".") + FinalDestAssetName)));
#endif
	
	// 如果标准路径找不到，尝试直接路径
	if (!NewAssetData.IsValid())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewAssetPath));
#else
		NewAssetData = AssetRegistry.GetAssetByObjectPath(FName(*NewAssetPath));
#endif
	}
	
	bool bActuallyMoved = NewAssetData.IsValid();
	
	// 如果移动成功，保存新位置的资产包
	bool bSaved = false;
	if (bSuccess && bActuallyMoved)
	{
		UObject* MovedAsset = NewAssetData.GetAsset();
		if (MovedAsset)
		{
			UPackage* Package = MovedAsset->GetOutermost();
			if (Package)
			{
			FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				
				// 使用新的 SavePackageArgs API（UE 5.0+ 统一使用）
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
				FSavePackageResultStruct Result = UPackage::Save(Package, MovedAsset, *PackageFileName, SaveArgs);
				bSaved = Result.Result == ESavePackageResult::Success;
#else
				bSaved = UPackage::SavePackage(Package, MovedAsset, *PackageFileName, SaveArgs);
#endif
				UE_LOG(LogUALContentCmd, Log, TEXT("Saved moved asset: %s (Success: %s)"), *PackageFileName, bSaved ? TEXT("true") : TEXT("false"));
			}
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("RenameAssets returned: %s, Asset at new location: %s, Saved: %s"), 
		bSuccess ? TEXT("true") : TEXT("false"),
		bActuallyMoved ? TEXT("found") : TEXT("not found"),
		bSaved ? TEXT("true") : TEXT("false"));
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), bSuccess && bActuallyMoved);
	Response->SetStringField(TEXT("source_path"), SourcePath);
	Response->SetStringField(TEXT("destination_path"), DestPackagePath / FinalDestAssetName);
	
	if (bRenamed)
	{
		Response->SetBoolField(TEXT("renamed"), true);
		Response->SetStringField(TEXT("original_destination"), DestinationPath);
	}
	
	Response->SetBoolField(TEXT("saved"), bSaved);
	
	if (bSuccess && bActuallyMoved)
	{
		FString Msg = bRenamed 
			? FString::Printf(TEXT("Asset moved and auto-renamed: %s -> %s"), *SourcePath, *FinalDestAssetName)
			: TEXT("Asset moved/renamed successfully");
		Response->SetStringField(TEXT("message"), Msg);
	}
	else if (bSuccess && !bActuallyMoved)
	{
		Response->SetBoolField(TEXT("ok"), false);
		Response->SetStringField(TEXT("error"), TEXT("RenameAssets returned success but asset was not found at new location. Check if target folder exists."));
	}
	else
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to move/rename asset"));
	}
	
	UAL_CommandUtils::SendResponse(RequestId, (bSuccess && bActuallyMoved) ? 200 : 500, Response);
}

/**
 * 删完之后还活着的那个资产，到底是谁拽着它。
 *
 * 引擎自己知道答案 —— `GatherObjectReferencersForDeletion` 把「真外部引用」和
 * 「撤销缓冲引用」分得清清楚楚 —— 但它只把答案写进一个无人值守下自动关掉的
 * 对话框里。这里重新问一次，把名字列出来回给调用方。
 */
static FString UAL_DescribeDeleteBlockers(UObject* Object)
{
	if (!IsValid(Object))
	{
		return TEXT("the engine refused to delete it, and the asset can no longer be inspected");
	}

	TArray<FString> Parts;

	const UPackage* Package = Object->GetOutermost();
	const int32 AgentUndoSteps = Package
		? FUAL_AgentUndo::CountStepsTouching({ Package->GetName() })
		: 0;
	if (AgentUndoSteps > 0)
	{
		Parts.Add(FString::Printf(
			TEXT("%d step(s) of this agent's own undo history still hold it ")
			TEXT("(retry with drop_agent_undo=true after the user agrees to lose those undo steps)"),
			AgentUndoSteps));
	}

	bool bIsReferenced = false;
	bool bIsReferencedByUndo = false;
	FReferencerInformationList References;
	ObjectTools::GatherObjectReferencersForDeletion(Object, bIsReferenced, bIsReferencedByUndo, &References, true);

	if (bIsReferenced)
	{
		TArray<FString> Names;
		for (const FReferencerInformation& Info : References.ExternalReferences)
		{
			if (Info.Referencer)
			{
				Names.AddUnique(Info.Referencer->GetPathName());
			}
			// 五个足够定位问题了。全列的话一个撤销缓冲就能刷出几十行
			if (Names.Num() >= 5)
			{
				break;
			}
		}

		Parts.Add(Names.Num() > 0
			? FString::Printf(TEXT("still referenced in memory by: %s"), *FString::Join(Names, TEXT(", ")))
			: TEXT("still referenced in memory (the engine did not name the referencer)"));
	}

	if (bIsReferencedByUndo)
	{
		// 用户自己那条撤销历史。**不碰** —— 那是他的东西，不是我们的
		Parts.Add(TEXT("the editor's own undo history holds it (that is the user's undo stack; ")
			TEXT("clearing it is the user's call, not the agent's)"));
	}

	if (Parts.Num() == 0)
	{
		Parts.Add(TEXT("the engine refused to delete it and reported no referencer - it may still be open ")
			TEXT("in an editor tab; check the Unreal log for details"));
	}

	return FString::Join(Parts, TEXT("; "));
}

/**
 * content.delete - 删除资产
 * 彻底删除资产或文件夹（无对话框，使用 ForceDeleteObjects）
 *
 * ## 删之前必须先问三件事
 *
 * 无人值守模式下引擎遇到障碍**一声不吭**：只读的包被
 * `ObjectTools::MakeReadOnlyPackageWritable` 里那个「文件只读，仍要删吗」的对话框
 * 挡下（无人值守时默认答「否」），被引用的资产则在 `DeleteSingleObject` 里弹
 * 「正在使用中」然后放弃。两种情况 `ForceDeleteObjects` 都只是返回 0，
 * 调用方拿不到任何原因，只能自己猜 —— 猜出来的三条（标签页/被引用/重启编辑器）
 * 2026-09-16 那次真机上一条都不对，模型照着猜的原因白折腾了七八个调用。
 *
 * 所以现在删之前逐条预检，把**真原因**回出去：
 *
 *   1. **包文件是只读的** —— 删不掉，说清楚是谁设的（版本控制 / 盒子的资产锁 / 用户自己）；
 *   2. **agent 自己的撤销栈拽着它** —— 见 `FUAL_AgentUndo::CountStepsTouching`，
 *      默认不动它，只如实回报；带上 `drop_agent_undo=true` 才摘掉那几步；
 *   3. 其余删不掉的，事后把**真实的引用者名字**列出来，不再笼统地说「多半是……」。
 */
void FUAL_ContentBrowserCommands::Handle_DeleteAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 🔑 关键：进入无人值守模式，跳过所有交互式对话框
	// 这与 UE5.3 的 UEditorAssetSubsystem::DeleteAsset 使用相同的策略
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

	// 解析 paths 数组
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("paths"), PathsArray) || !PathsArray || PathsArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'paths' array"));
		return;
	}

	/**
	 * 摘掉 agent 自己那几步撤销记录再删。**默认 false**。
	 *
	 * 默认值只能是 false：丢撤销记录是用户的损失，不该由模型顺手替他决定。
	 * 没这个开关时的正确行为是把情况说清楚，让模型去问用户。
	 */
	bool bDropAgentUndo = false;
	Payload->TryGetBoolField(TEXT("drop_agent_undo"), bDropAgentUndo);

	UE_LOG(LogUALContentCmd, Log, TEXT("content.delete: %d paths (Unattended mode enabled, drop_agent_undo=%s)"),
		PathsArray->Num(), bDropAgentUndo ? TEXT("true") : TEXT("false"));

	// 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 收集要删除的资产
	TArray<UObject*> ObjectsToDelete;
	TArray<FString> DeletedPaths;
	TArray<FString> FailedPaths;
	/**
	 * 每条失败的原因。
	 *
	 * 以前这些原因只进 `UE_LOG` —— 那行日志在编辑器的输出窗口里，
	 * 调用方拿到的只有一句「无响应或 ok=false（错误码 200）」，
	 * 既不知道是没找到、加载不了，还是被引用着删不掉。2026-09-16 的用户反馈
	 * 就卡在这一句上，最后改用 Python 才删掉。
	 */
	TMap<FString, FString> FailureReasons;

	int32 EntryIndex = -1;
	for (const TSharedPtr<FJsonValue>& PathValue : *PathsArray)
	{
		++EntryIndex;
		FString AssetPath;
		if (!PathValue.IsValid() || !PathValue->TryGetString(AssetPath) || AssetPath.IsEmpty())
		{
			// 空条目以前静默跳过，却照样算在 requested_count 里 —— 调用方数 3 条、
			// 回执里只交代了 2 条。用序号占位记进 failed，每条输入都有下落
			const FString Placeholder = FString::Printf(TEXT("#%d"), EntryIndex);
			FailedPaths.Add(Placeholder);
			FailureReasons.Add(Placeholder, TEXT("empty or non-string entry in 'paths'"));
			continue;
		}
		
		// 尝试多种方式查找资产
		FAssetData AssetData;
		
		// 尝试1: 直接作为 ObjectPath
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
#else
		AssetData = AssetRegistry.GetAssetByObjectPath(FName(*AssetPath));
#endif
		
		// 尝试2: 构造完整 ObjectPath
		if (!AssetData.IsValid())
		{
			FString AssetName = FPaths::GetBaseFilename(AssetPath);
			FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
#else
			AssetData = AssetRegistry.GetAssetByObjectPath(FName(*FullObjectPath));
#endif
		}
		
		// 尝试3: 通过 PackageName 查找
		if (!AssetData.IsValid())
		{
			TArray<FAssetData> AssetList;
			AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), AssetList);
			if (AssetList.Num() > 0)
			{
				AssetData = AssetList[0];
			}
		}
		
		if (AssetData.IsValid())
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Found valid AssetData for: %s, PackageName: %s"), 
				*AssetPath, *AssetData.PackageName.ToString());
			
			/**
			 * `FAssetData::GetAsset()` 拿不到时**再直接 LoadObject 一次**，别直接判死。
			 *
			 * 这两条路不等价。`FastGetAsset` 是先 `FindObjectFast` 找包、包没
			 * `IsFullyLoaded()` 就把它当没有、再 `LoadPackage`，最后按注册表缓存的
			 * AssetName 在包里 `FindObjectFast`（引擎 AssetData.h:513-553）。中间任何
			 * 一环和注册表缓存对不上，它就返回空 —— 而资产本身好端端的。
			 *
			 * 2026-09-18 真机撞到：一个 60KB、6 节点、`load_asset` 读得好好的 PCGGraph，
			 * 在这里被判成「包可能损坏」，重试两次同一句话。
			 */
			UObject* Asset = AssetData.GetAsset();
			FString ObjectPath;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			ObjectPath = AssetData.GetObjectPathString();
#else
			ObjectPath = AssetData.ObjectPath.ToString();
#endif
			if (!Asset && !ObjectPath.IsEmpty())
			{
				Asset = LoadObject<UObject>(nullptr, *ObjectPath);
				if (Asset)
				{
					UE_LOG(LogUALContentCmd, Log,
						TEXT("GetAsset() returned null for %s but LoadObject succeeded - registry cache is stale"),
						*AssetPath);
				}
			}

			if (Asset)
			{
				UE_LOG(LogUALContentCmd, Log, TEXT("Successfully loaded asset: %s"), *Asset->GetPathName());
				ObjectsToDelete.Add(Asset);
				DeletedPaths.Add(AssetPath);
			}
			else
			{
				/**
				 * 两条路都失败了才报错，而且**报观察到的事实，不报猜测**。
				 *
				 * 原来这里一律说「the package may be corrupt」。那是从「GetAsset 返回空」
				 * 猜出来的因果，而那个函数返回空的原因至少有四种。用户拿着一句错的
				 * 结论去查磁盘、查文件大小，什么也查不出来。
				 *
				 * 现在把当场能测到的三件事直接摆出来：盘上有没有文件、内存里有没有这个包、
				 * 那个包是不是 fully loaded。下次再撞上，光看这句话就知道是哪一种。
				 */
				const FString PackageName = AssetData.PackageName.ToString();
				const bool bOnDisk = FPackageName::DoesPackageExist(PackageName);
				const UPackage* InMemory = FindObject<UPackage>(nullptr, *PackageName);

				const FString Reason = FString::Printf(
					TEXT("the asset is listed in the registry but neither FAssetData::GetAsset() nor ")
					TEXT("LoadObject could return it. Observed right now: package file on disk = %s; ")
					TEXT("package in memory = %s; fully loaded = %s; registry says the object is '%s'. ")
					TEXT("A stale registry entry is the usual cause - run content.registry_scan on the ")
					TEXT("folder and retry. Nothing was deleted."),
					bOnDisk ? TEXT("yes") : TEXT("no"),
					InMemory ? TEXT("yes") : TEXT("no"),
					InMemory ? (InMemory->IsFullyLoaded() ? TEXT("yes") : TEXT("no")) : TEXT("n/a"),
					*ObjectPath);

				UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to load asset object for %s: %s"),
					*AssetPath, *Reason);
				FailedPaths.Add(AssetPath);
				FailureReasons.Add(AssetPath, Reason);
			}
		}
		else
		{
			// 可能是文件夹路径，记录失败
			FailedPaths.Add(AssetPath);
			UE_LOG(LogUALContentCmd, Warning, TEXT("Asset not found: %s"), *AssetPath);
			FailureReasons.Add(AssetPath, TEXT("no asset at this path (content.delete does not delete folders; check the path with content.search)"));
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Collected %d objects to delete, %d failed paths"),
		ObjectsToDelete.Num(), FailedPaths.Num());

	// ========================================================================
	// 预检：只读位 / agent 自己的撤销栈
	// ========================================================================

	/** 要摘掉撤销记录的包（只有 drop_agent_undo=true 时才会有东西） */
	TArray<FString> PackagesToFreeFromUndo;
	/** 摘掉的撤销步骤标题，回给调用方 —— 丢了什么必须说清楚 */
	TArray<FString> DroppedUndoTitles;

	for (int32 Index = ObjectsToDelete.Num() - 1; Index >= 0; --Index)
	{
		UObject* Object = ObjectsToDelete[Index];
		const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
		if (!Package)
		{
			continue;
		}

		const FString PackageName = Package->GetName();
		// 取副本：下面可能把这一条从 DeletedPaths 里摘掉，引用会跟着失效
		const FString AssetPath = DeletedPaths[Index];

		FString BlockedReason;

		// 1) 只读的包，引擎会在无人值守下静默跳过（MakeReadOnlyPackageWritable
		//    里那个对话框默认答「否」）。删之前就说出来，别让调用方对着 0 猜
		FString PackageFilename;
		if (FPackageName::DoesPackageExist(PackageName, &PackageFilename)
			&& IFileManager::Get().IsReadOnly(*PackageFilename))
		{
			BlockedReason = FString::Printf(
				TEXT("the package file is read-only on disk (%s), so the engine skips it silently. ")
				TEXT("It is either version control (check out the file first), Unreal Box's own asset lock ")
				TEXT("(wait for the other session to finish), or a read-only flag set by hand. ")
				TEXT("Nothing was deleted."),
				*PackageFilename);
		}

		// 2) agent 自己的撤销栈攥着它。**引擎认不出这是撤销缓冲**（它只认当前
		//    装着的那一条），所以会当成普通外部引用直接判「正在使用中」
		if (BlockedReason.IsEmpty())
		{
			const int32 UndoSteps = FUAL_AgentUndo::CountStepsTouching({ PackageName });
			if (UndoSteps > 0)
			{
				if (bDropAgentUndo)
				{
					PackagesToFreeFromUndo.AddUnique(PackageName);
				}
				else
				{
					BlockedReason = FString::Printf(
						TEXT("%d step(s) of THIS agent's own undo history still reference it, which makes the ")
						TEXT("engine treat the asset as in use and refuse to delete it. Nothing was deleted. ")
						TEXT("Tell the user those %d undo step(s) must be thrown away to delete this asset ")
						TEXT("(the user's own editor undo history is NOT affected), and only after they agree, ")
						TEXT("call this tool again with drop_agent_undo=true."),
						UndoSteps, UndoSteps);
				}
			}
		}

		if (!BlockedReason.IsEmpty())
		{
			UE_LOG(LogUALContentCmd, Warning, TEXT("Blocked before delete: %s - %s"), *AssetPath, *BlockedReason);
			FailedPaths.Add(AssetPath);
			FailureReasons.Add(AssetPath, BlockedReason);
			ObjectsToDelete.RemoveAt(Index);
			DeletedPaths.RemoveAt(Index);
		}
	}

	if (PackagesToFreeFromUndo.Num() > 0)
	{
		FUAL_AgentUndo::DropStepsTouching(PackagesToFreeFromUndo, DroppedUndoTitles);
	}

	// 删之前先记下弱引用：删完之后**逐个**看谁还活着，才能按路径给出准确结果。
	// 原先是「返回 0 就全算失败」，一批里删掉一半的情况会把成功的那几条也报成失败
	TArray<TWeakObjectPtr<UObject>> WatchedObjects;
	WatchedObjects.Reserve(ObjectsToDelete.Num());
	for (UObject* Object : ObjectsToDelete)
	{
		WatchedObjects.Add(Object);
	}
	const TArray<FString> AttemptedPaths = DeletedPaths;

	// 使用 ForceDeleteObjects 执行删除（配合 UnattendedScriptGuard 完全无对话框）
	int32 DeletedCount = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, TEXT("Calling ForceDeleteObjects with %d objects..."), ObjectsToDelete.Num());
		const bool bShowConfirmation = false; // 不显示确认对话框
		DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, bShowConfirmation);
		UE_LOG(LogUALContentCmd, Log, TEXT("ForceDeleteObjects returned: %d deleted"), DeletedCount);

		// 逐条核对：还活着的就是没删掉的。ForceDeleteObjects 内部会跑 GC，
		// 所以这里只能问弱引用，裸指针这时候可能已经是野的了
		DeletedPaths.Reset();
		for (int32 Index = 0; Index < AttemptedPaths.Num(); ++Index)
		{
			UObject* Survivor = WatchedObjects[Index].Get();
			if (!IsValid(Survivor))
			{
				DeletedPaths.Add(AttemptedPaths[Index]);
				continue;
			}

			FailedPaths.Add(AttemptedPaths[Index]);
			FailureReasons.Add(AttemptedPaths[Index], UAL_DescribeDeleteBlockers(Survivor));
		}
	}
	else
	{
		UE_LOG(LogUALContentCmd, Warning, TEXT("No valid objects collected for deletion"));
		DeletedPaths.Reset();
	}

	// 返回结果。**按路径数，不用 ForceDeleteObjects 的返回值** —— 它数的是对象
	// （删一个蓝图会连带删掉它的实例），和调用方给的路径对不上号
	const int32 DeletedPathCount = DeletedPaths.Num();

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), DeletedPathCount > 0);
	Response->SetNumberField(TEXT("deleted_count"), DeletedPathCount);
	// 顶层 failed_count 一律给（0 也给）：部分删除时 ok 仍是 true，
	// 调用方不该靠「failed 数组存不存在」去猜有没有失败（AGENTS.md §5 第 14 条）
	Response->SetNumberField(TEXT("failed_count"), FailedPaths.Num());
	Response->SetNumberField(TEXT("requested_count"), PathsArray->Num());
	Response->SetNumberField(TEXT("engine_deleted_objects"), DeletedCount);

	// 丢掉了哪几步撤销，必须原样回去 —— 用户损失的东西不能只留在日志里
	if (DroppedUndoTitles.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> DroppedArray;
		for (const FString& Title : DroppedUndoTitles)
		{
			DroppedArray.Add(MakeShared<FJsonValueString>(Title));
		}
		Response->SetArrayField(TEXT("dropped_agent_undo_steps"), DroppedArray);
	}

	// 添加删除的路径列表
	TArray<TSharedPtr<FJsonValue>> DeletedArray;
	for (const FString& Path : DeletedPaths)
	{
		DeletedArray.Add(MakeShared<FJsonValueString>(Path));
	}
	Response->SetArrayField(TEXT("deleted"), DeletedArray);
	
	// 失败的路径连原因一起回 —— 没有原因的失败，调用方只能换个工具再试一遍
	if (FailedPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FailedArray;
		for (const FString& Path : FailedPaths)
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("path"), Path);
			if (const FString* Reason = FailureReasons.Find(Path))
			{
				FailObj->SetStringField(TEXT("reason"), *Reason);
			}
			FailedArray.Add(MakeShared<FJsonValueObject>(FailObj));
		}
		Response->SetArrayField(TEXT("failed"), FailedArray);

		// 一个都没删掉时给一句顶层的 error，调用方不必自己拼
		if (DeletedPathCount == 0)
		{
			const FString* FirstReason = FailureReasons.Find(FailedPaths[0]);
			Response->SetStringField(
				TEXT("error"),
				FString::Printf(TEXT("Deleted nothing. %s: %s"),
					*FailedPaths[0],
					FirstReason ? **FirstReason : TEXT("unknown reason")));
		}
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.describe - 获取资产详情
 * 返回资产的完整信息，包括依赖项和被引用项
 * 
 * 请求参数:
 *   - path: 资产路径（必填）
 *   - include_dependencies: 是否包含依赖项（可选，默认 true）
 *   - include_referencers: 是否包含被引用项（可选，默认 true）
 * 
 * 响应:
 *   - ok: 是否成功
 *   - name: 资产名称
 *   - path: 资产完整路径
 *   - class: 资产类型
 *   - package_size: 资产包大小（字节）
 *   - dependencies: 依赖的资产列表
 *   - referencers: 引用此资产的资产列表
 */
void FUAL_ContentBrowserCommands::Handle_DescribeAsset(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString AssetPath;
	if (!Payload->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: path"));
		return;
	}
	
	bool bIncludeDependencies = true;
	bool bIncludeReferencers = true;
	Payload->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
	Payload->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.describe: path=%s, deps=%d, refs=%d"),
		*AssetPath, bIncludeDependencies, bIncludeReferencers);
	
	// 2. 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 3. 查找资产
	FAssetData AssetData;

	// **先补全再解析**（同 Handle_MoveAsset）。UE 5.1+ 的 FSoftObjectPath
	// 拿到只有包名的路径会解析成**包**，IsValid() 为真但拿到的是 Package 而不是
	// 资产本身 —— 于是下面的尝试全被跳过，describe 回给调用方
	// `assetClass: "Package"`、`packageSizeBytes: 0`，等于什么都没说。
	// 而 content.search 返回的正是这种不带 .Object 的短路径。
	FString ResolvedPath = AssetPath;
	if (!AssetPath.Contains(TEXT(".")))
	{
		ResolvedPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
	}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ResolvedPath));
#else
	AssetData = AssetRegistry.GetAssetByObjectPath(FName(*ResolvedPath));
#endif

	// 兜底：调用方本来给的就是完整 ObjectPath
	if (!AssetData.IsValid() && ResolvedPath != AssetPath)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
#else
		AssetData = AssetRegistry.GetAssetByObjectPath(FName(*AssetPath));
#endif
	}
	
	// 尝试3: 通过 PackageName 查找
	if (!AssetData.IsValid())
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), AssetList);
		if (AssetList.Num() > 0)
		{
			AssetData = AssetList[0];
		}
	}
	
	if (!AssetData.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		return;
	}
	
	// 4. 构建响应
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	
	// 获取完整路径
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Response->SetStringField(TEXT("path"), UALCompat::GetObjectPathString(AssetData));
	Response->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
#else
	Response->SetStringField(TEXT("path"), AssetData.ObjectPath.ToString());
	Response->SetStringField(TEXT("class"), AssetData.AssetClass.ToString());
#endif
	
	Response->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
	
	// 5. 获取包大小（估算）
	int64 PackageSize = 0;
	FString PackageFileName;
	if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFileName))
	{
		PackageSize = IFileManager::Get().FileSize(*PackageFileName);
	}
	Response->SetNumberField(TEXT("package_size_bytes"), (double)PackageSize);
	
	// 6. 获取依赖项
	if (bIncludeDependencies)
	{
		TArray<TSharedPtr<FJsonValue>> DepsArray;
		TArray<FName> Dependencies;
		
		AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies);
		
		for (const FName& DepName : Dependencies)
		{
			FString DepPath = DepName.ToString();
			// 过滤掉引擎内置资产和脚本
			if (DepPath.StartsWith(TEXT("/Game/")) || DepPath.StartsWith(TEXT("/Content/")))
			{
				TSharedPtr<FJsonObject> DepObj = MakeShared<FJsonObject>();
				DepObj->SetStringField(TEXT("path"), DepPath);
				
				// 尝试获取依赖资产的类型
				TArray<FAssetData> DepAssets;
				AssetRegistry.GetAssetsByPackageName(DepName, DepAssets);
				if (DepAssets.Num() > 0)
				{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
					DepObj->SetStringField(TEXT("class"), DepAssets[0].AssetClassPath.GetAssetName().ToString());
#else
					DepObj->SetStringField(TEXT("class"), DepAssets[0].AssetClass.ToString());
#endif
					DepObj->SetStringField(TEXT("name"), DepAssets[0].AssetName.ToString());
				}
				
				DepsArray.Add(MakeShared<FJsonValueObject>(DepObj));
			}
		}
		
		Response->SetArrayField(TEXT("dependencies"), DepsArray);
		Response->SetNumberField(TEXT("dependencies_count"), DepsArray.Num());
		
		UE_LOG(LogUALContentCmd, Log, TEXT("Found %d dependencies for %s"), DepsArray.Num(), *AssetPath);
	}
	
	// 7. 获取被引用项（哪些资产引用了这个资产）
	if (bIncludeReferencers)
	{
		TArray<TSharedPtr<FJsonValue>> RefsArray;
		TArray<FName> Referencers;
		
		AssetRegistry.GetReferencers(AssetData.PackageName, Referencers);
		
		for (const FName& RefName : Referencers)
		{
			FString RefPath = RefName.ToString();
			// 过滤掉引擎内置资产
			if (RefPath.StartsWith(TEXT("/Game/")) || RefPath.StartsWith(TEXT("/Content/")))
			{
				TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
				RefObj->SetStringField(TEXT("path"), RefPath);
				
				// 尝试获取引用资产的类型
				TArray<FAssetData> RefAssets;
				AssetRegistry.GetAssetsByPackageName(RefName, RefAssets);
				if (RefAssets.Num() > 0)
				{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
					RefObj->SetStringField(TEXT("class"), RefAssets[0].AssetClassPath.GetAssetName().ToString());
#else
					RefObj->SetStringField(TEXT("class"), RefAssets[0].AssetClass.ToString());
#endif
					RefObj->SetStringField(TEXT("name"), RefAssets[0].AssetName.ToString());
				}
				
				RefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
			}
		}
		
		Response->SetArrayField(TEXT("referencers"), RefsArray);
		Response->SetNumberField(TEXT("referencers_count"), RefsArray.Num());
		
		UE_LOG(LogUALContentCmd, Log, TEXT("Found %d referencers for %s"), RefsArray.Num(), *AssetPath);
	}
	
	// 8. 添加迁移提示
	bool bHasDependencies = Response->HasField(TEXT("dependencies")) && 
		Response->GetArrayField(TEXT("dependencies")).Num() > 0;
	bool bHasReferencers = Response->HasField(TEXT("referencers")) && 
		Response->GetArrayField(TEXT("referencers")).Num() > 0;
	
	FString MigrationHint;
	if (bHasDependencies && bHasReferencers)
	{
		MigrationHint = TEXT("This asset has both dependencies and referencers. To migrate safely, include all dependencies. Referencers may need to be updated.");
	}
	else if (bHasDependencies)
	{
		MigrationHint = TEXT("This asset has dependencies. Include all listed dependencies when migrating.");
	}
	else if (bHasReferencers)
	{
		MigrationHint = TEXT("This asset is referenced by other assets. Deleting or moving may break references.");
	}
	else
	{
		MigrationHint = TEXT("This asset is self-contained with no dependencies or referencers.");
	}
	Response->SetStringField(TEXT("migration_hint"), MigrationHint);

	// 按类型补上「用户真正想知道的那几项」。
	//
	// 原来 describe 只回依赖和被引用计数 —— 那是给资产迁移用的。
	// 但用户问得最多的是「这张贴图多大」「这个模型多少面」，
	// 光有依赖关系答不了，只能让人去编辑器里自己点开看。
	if (UObject* Asset = AssetData.GetAsset())
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		bool bHasDetails = false;

		if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
		{
			Details->SetNumberField(TEXT("width"), Texture->GetSizeX());
			Details->SetNumberField(TEXT("height"), Texture->GetSizeY());
			Details->SetStringField(TEXT("pixel_format"),
				GPixelFormats[Texture->GetPixelFormat()].Name);
			Details->SetStringField(TEXT("compression"),
				StaticEnum<TextureCompressionSettings>()
					? StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(Texture->CompressionSettings)
					: TEXT("Unknown"));
			Details->SetBoolField(TEXT("srgb"), Texture->SRGB);
			bHasDetails = true;
		}
		else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
		{
			Details->SetNumberField(TEXT("lod_count"), Mesh->GetNumLODs());
			Details->SetNumberField(TEXT("material_slots"), Mesh->GetStaticMaterials().Num());
			if (Mesh->GetNumLODs() > 0)
			{
				Details->SetNumberField(TEXT("triangles_lod0"), Mesh->GetNumTriangles(0));
				Details->SetNumberField(TEXT("vertices_lod0"), Mesh->GetNumVertices(0));
			}
			Details->SetBoolField(TEXT("nanite_enabled"), UALCompat::IsNaniteEnabled(Mesh));

			// 包围盒连 min/max 一起给（资产局部空间，厘米）。
			//
			// size 只回答「这东西多大」，min/max 回答「原点在几何体的哪儿」——
			// 后者是拼模块化套件的前提，而这里是调用方问一个网格时最先到的地方。
			// 拿不到它的话，唯一的出路是 viewport.focus，那会把用户的镜头飞走。
			const FBox LocalBox = Mesh->GetBoundingBox();
			TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
			Bounds->SetObjectField(TEXT("size"), UAL_CommandUtils::MakeVectorJson(LocalBox.GetSize()));
			Bounds->SetObjectField(TEXT("min"), UAL_CommandUtils::MakeVectorJson(LocalBox.Min));
			Bounds->SetObjectField(TEXT("max"), UAL_CommandUtils::MakeVectorJson(LocalBox.Max));
			Details->SetObjectField(TEXT("bounds"), Bounds);
			bHasDetails = true;
		}

		if (bHasDetails)
		{
			Response->SetObjectField(TEXT("details"), Details);
		}
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.normalized_import - 规范化导入 uasset/umap 资产
 * 将外部工程的资产导入到规范化的目录结构中
 * 自动处理依赖闭包、包名重映射和引用修复
 */
void FUAL_ContentBrowserCommands::Handle_NormalizedImport(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析 files 数组
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'files' array"));
		return;
	}
	
	// 解析可选参数
	FString TargetRoot = TEXT("/Game/Imported");
	Payload->TryGetStringField(TEXT("target_root"), TargetRoot);
	
	bool bUsePascalCase = true;
	Payload->TryGetBoolField(TEXT("use_pascal_case"), bUsePascalCase);
	
	bool bAutoRenameOnConflict = true;
	Payload->TryGetBoolField(TEXT("auto_rename_on_conflict"), bAutoRenameOnConflict);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.normalized_import: %d files -> %s"),
		FilesArray->Num(), *TargetRoot);
	
	// 收集文件路径
	TArray<FString> FilePaths;
	/**
	 * 没递给导入器的输入条目（文件不存在 / 空条目），每条一句原因。
	 *
	 * 以前只进日志，total_files / failed_count 都不含它们 —— 给 5 个文件、
	 * 2 个路径写错，回执是「3/3 成功」，调用方以为全进来了。
	 */
	TArray<FString> MissingFileErrors;
	for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
	{
		FString FilePath;
		if (FileValue.IsValid() && FileValue->TryGetString(FilePath) && !FilePath.IsEmpty())
		{
			// 验证文件存在
			if (FPaths::FileExists(FilePath))
			{
				FilePaths.Add(FilePath);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("File not found: %s"), *FilePath);
				MissingFileErrors.Add(FString::Printf(TEXT("File not found: %s"), *FilePath));
			}
		}
		else
		{
			MissingFileErrors.Add(TEXT("Empty or non-string entry in 'files'"));
		}
	}
	
	if (FilePaths.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, MissingFileErrors.Num() > 0
			? FString::Printf(TEXT("No valid files to import. %s"), *FString::Join(MissingFileErrors, TEXT("; ")))
			: FString(TEXT("No valid files to import")));
		return;
	}
	
	// 解析语义后缀选项
	bool bUseSemanticSuffix = true;
	Payload->TryGetBoolField(TEXT("use_semantic_suffix"), bUseSemanticSuffix);
	
	// 配置导入规则
	FUALImportRuleSet RuleSet;
	RuleSet.InitDefaults();
	RuleSet.TargetRoot = TargetRoot;
	RuleSet.bUsePascalCase = bUsePascalCase;
	RuleSet.bAutoRenameOnConflict = bAutoRenameOnConflict;
	RuleSet.bUseSemanticSuffix = bUseSemanticSuffix;
	
	// 执行规范化导入
	FUALNormalizedImporter Importer;
	FUALNormalizedImportSession Session;
	
	bool bSuccess = Importer.ExecuteNormalizedImport(FilePaths, RuleSet, Session);

	// Show notification (Ensure logic runs on GameThread)
	if (bSuccess && Session.SuccessCount > 0)
	{
		int32 Count = Session.SuccessCount; // Capture by value
		AsyncTask(ENamedThreads::GameThread, [Count]()
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Handle_NormalizedImport: Attempting to show success notification for %d assets"), Count);

			FString Title = UAL_CommandUtils::LStr(TEXT("规范化导入成功"), TEXT("Normalized Import Successful"));
			FString Msg = FString::Printf(TEXT("%s: %d"), *UAL_CommandUtils::LStr(TEXT("成功处理"), TEXT("Processed")), Count);

			FNotificationInfo Info(FText::FromString(Title));
			Info.SubText = FText::FromString(Msg);
			Info.ExpireDuration = 3.0f;
			Info.bFireAndForget = true;
			Info.bUseLargeFont = false;
			
			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem.IsValid())
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Handle_NormalizedImport: Failed to create notification item"));
			}
		});
	}
	
	// 构建响应
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	// ok 仍只看导入器本身（调用方 ok=false 时会丢掉 imported[]，已经进来的就看不到了）；
	// 找不到的文件照实算进 total_files / failed_count / errors，数字和调用方给的清单对得上
	Response->SetBoolField(TEXT("ok"), bSuccess);
	Response->SetNumberField(TEXT("total_files"), Session.TotalFiles + MissingFileErrors.Num());
	Response->SetNumberField(TEXT("success_count"), Session.SuccessCount);
	Response->SetNumberField(TEXT("failed_count"), Session.FailedCount + MissingFileErrors.Num());
	Session.Errors.Append(MissingFileErrors);
	
	// 添加导入的资产信息
	TArray<TSharedPtr<FJsonValue>> ImportedArray;
	for (const FUALImportTargetInfo& Info : Session.TargetInfos)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("original_name"), Info.OriginalAssetName);
		Item->SetStringField(TEXT("normalized_name"), Info.NormalizedAssetName);
		Item->SetStringField(TEXT("old_path"), Info.OldPackageName.ToString());
		Item->SetStringField(TEXT("new_path"), Info.NewPackageName.ToString());
		Item->SetStringField(TEXT("class"), Info.AssetClass);
		ImportedArray.Add(MakeShared<FJsonValueObject>(Item));
	}
	Response->SetArrayField(TEXT("imported"), ImportedArray);
	
	// 添加重定向映射
	TArray<TSharedPtr<FJsonValue>> RedirectArray;
	for (const auto& Pair : Session.RedirectMap)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("from"), Pair.Key.ToString());
		Item->SetStringField(TEXT("to"), Pair.Value.ToString());
		RedirectArray.Add(MakeShared<FJsonValueObject>(Item));
	}
	Response->SetArrayField(TEXT("redirects"), RedirectArray);
	
	// 添加错误和警告
	if (Session.Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorArray;
		for (const FString& Error : Session.Errors)
		{
			ErrorArray.Add(MakeShared<FJsonValueString>(Error));
		}
		Response->SetArrayField(TEXT("errors"), ErrorArray);
	}
	
	if (Session.Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningArray;
		for (const FString& Warning : Session.Warnings)
		{
			WarningArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Response->SetArrayField(TEXT("warnings"), WarningArray);
	}
	
	UAL_CommandUtils::SendResponse(RequestId, bSuccess ? 200 : 500, Response);
}

void FUAL_ContentBrowserCommands::Handle_AuditOptimization(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString CheckType = TEXT("All");
	Payload->TryGetStringField(TEXT("check_type"), CheckType);

	// 三段扫描都读注册表，一道门管到底：没扫完就回 503 + 进度，不在游戏线程上死等
	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Nanite 使用情况检测
	if (CheckType.Equals(TEXT("NaniteUsage"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
#if ENGINE_MAJOR_VERSION >= 5
		TSharedPtr<FJsonObject> NaniteData = MakeShared<FJsonObject>();
		
		// 检查配置
		FString NaniteEnabled;
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Nanite.ProjectEnabled"), NaniteEnabled, GEngineIni);
		bool bNaniteEnabledInConfig = NaniteEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || NaniteEnabled.Equals(TEXT("1"), ESearchCase::IgnoreCase);
		NaniteData->SetBoolField(TEXT("enabled_in_config"), bNaniteEnabledInConfig);

		// 扫描资产
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> MeshAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(UStaticMesh::StaticClass()->GetClassPathName(), MeshAssets, true);
#else
		AssetRegistry.GetAssetsByClass(UStaticMesh::StaticClass()->GetFName(), MeshAssets, true);
#endif

		int32 MeshesWithNanite = 0;
		for (const FAssetData& AssetData : MeshAssets)
		{
			// 加载资产并检查 Nanite 设置
			UStaticMesh* Mesh = Cast<UStaticMesh>(AssetData.GetAsset());
			if (Mesh && Mesh->HasValidNaniteData())
			{
				MeshesWithNanite++;
			}
		}

		NaniteData->SetNumberField(TEXT("mesh_count"), MeshAssets.Num());
		NaniteData->SetNumberField(TEXT("meshes_with_nanite"), MeshesWithNanite);

		if (bNaniteEnabledInConfig && MeshesWithNanite == 0)
		{
			NaniteData->SetStringField(TEXT("suggestion"), TEXT("检测到您开启了 Nanite 支持，但场景中没有任何模型使用了 Nanite。建议在 Project Settings 中关闭 Nanite 以剔除相关着色器变体，可显著提升构建速度。"));
		}
		else if (bNaniteEnabledInConfig && MeshesWithNanite > 0)
		{
			NaniteData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("检测到 %d 个模型使用了 Nanite，Nanite 功能正在被使用。"), MeshesWithNanite));
		}

		Result->SetObjectField(TEXT("nanite_usage"), NaniteData);
#endif
	}

	// Lumen 使用情况检测
	if (CheckType.Equals(TEXT("LumenMaterials"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
#if ENGINE_MAJOR_VERSION >= 5
		TSharedPtr<FJsonObject> LumenData = MakeShared<FJsonObject>();
		
		// 检查配置
		FString LumenEnabled;
		FString DynamicGI;
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Lumen.Enabled"), LumenEnabled, GEngineIni);
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), DynamicGI, GEngineIni);
		
		bool bLumenEnabledInConfig = LumenEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || LumenEnabled.Equals(TEXT("1"), ESearchCase::IgnoreCase);
		bool bUsingLumenGI = DynamicGI.Contains(TEXT("Lumen"), ESearchCase::IgnoreCase);
		
		LumenData->SetBoolField(TEXT("enabled_in_config"), bLumenEnabledInConfig);
		LumenData->SetBoolField(TEXT("using_lumen_gi"), bUsingLumenGI);

		// 扫描材质中的自发光使用（Lumen 特征）
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> MaterialAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FARFilter MaterialFilter;
		MaterialFilter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
		MaterialFilter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		AssetRegistry.GetAssets(MaterialFilter, MaterialAssets);
#else
		FARFilter MaterialFilter;
		MaterialFilter.ClassNames.Add(UMaterial::StaticClass()->GetFName());
		MaterialFilter.ClassNames.Add(UMaterialInstanceConstant::StaticClass()->GetFName());
		AssetRegistry.GetAssets(MaterialFilter, MaterialAssets);
#endif

		int32 MaterialsWithEmissive = 0;
		for (const FAssetData& AssetData : MaterialAssets)
		{
			// 检查 TagsAndValues 中是否有 Emissive 相关的标签
			// 或者尝试加载材质检查
			UMaterialInterface* Material = Cast<UMaterialInterface>(AssetData.GetAsset());
			if (Material)
			{
				// 简单检查：如果有自发光颜色或强度不为零，则认为使用了自发光
				// 注意：更精确的检查需要遍历材质节点图，这里使用简化方法
				FLinearColor EmissiveColor;
				float EmissiveStrength = 0.0f;
				
				if (Material->GetVectorParameterValue(TEXT("EmissiveColor"), EmissiveColor) ||
					Material->GetScalarParameterValue(TEXT("EmissiveStrength"), EmissiveStrength))
				{
					if (EmissiveColor.R > 0.01f || EmissiveColor.G > 0.01f || EmissiveColor.B > 0.01f || EmissiveStrength > 0.01f)
					{
						MaterialsWithEmissive++;
					}
				}
			}
		}

		LumenData->SetNumberField(TEXT("materials_with_emissive"), MaterialsWithEmissive);

		if (bLumenEnabledInConfig || bUsingLumenGI)
		{
			if (MaterialsWithEmissive > 0)
			{
				LumenData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("检测到 %d 个材质使用了自发光，Lumen 功能正在被使用。"), MaterialsWithEmissive));
			}
			else
			{
				LumenData->SetStringField(TEXT("suggestion"), TEXT("Lumen 已启用，但未检测到使用自发光的材质。如果不需要全局光照，可以考虑禁用 Lumen 以减小包体。"));
			}
		}

		Result->SetObjectField(TEXT("lumen_usage"), LumenData);
#endif
	}

	// 纹理大小分析
	if (CheckType.Equals(TEXT("TextureSize"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> TextureData = MakeShared<FJsonObject>();
		
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> AllTextureAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(UTexture2D::StaticClass()->GetClassPathName(), AllTextureAssets, true);
#else
		AssetRegistry.GetAssetsByClass(UTexture2D::StaticClass()->GetFName(), AllTextureAssets, true);
#endif

		// 默认只看 /Game 下的用户资产。
		//
		// 不收窄的话，「帮我看看有没有超大贴图」的回答里会混进引擎自带的
		// 噪声图和编辑器资源 —— 用户既动不了它们，也不该为它们操心。
		// 真机上未收窄时 4 个「超大贴图」有 3 个是引擎内容。
		// 传 path 可以指定别的范围（如刚导入的那个目录）。
		FString ScopePath = TEXT("/Game");
		Payload->TryGetStringField(TEXT("path"), ScopePath);

		TArray<FAssetData> TextureAssets;
		for (const FAssetData& Candidate : AllTextureAssets)
		{
			if (Candidate.PackageName.ToString().StartsWith(ScopePath))
			{
				TextureAssets.Add(Candidate);
			}
		}

		int32 TotalTextures = TextureAssets.Num();
		int32 LargeTextures4K = 0;
		int64 TotalTextureMemory = 0;

		// 光给个数字没法据此干活：调用方问的是「哪些贴图太大」，
		// 回一句「有 4 个」等于没回答 —— 它还得自己去把几千个贴图翻一遍。
		// 按面积从大到小列出具体路径和尺寸，多了截断（列表是给人和模型看的，
		// 不是全量导出；真要全量应该走 content.search）。
		const int32 MaxListed = 20;
		TArray<TPair<int64, TSharedPtr<FJsonObject>>> LargeList;

		for (const FAssetData& AssetData : TextureAssets)
		{
			UTexture2D* Texture = Cast<UTexture2D>(AssetData.GetAsset());
			if (Texture)
			{
				int32 Width = Texture->GetSizeX();
				int32 Height = Texture->GetSizeY();

				// 按**像素总量**判定，不是按单边。
				// `Width >= 4096 || Height >= 4096` 会把 4096x1 的分隔条
				// 和 128x8192 的噪声图都算成「超大贴图」—— 它们其实很小。
				// 门槛取 4096x2048：真正吃显存的那一档从这里开始。
				if ((int64)Width * Height >= 4096LL * 2048LL)
				{
					LargeTextures4K++;

					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("path"), UALCompat::GetObjectPathString(AssetData));
					Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
					Entry->SetNumberField(TEXT("width"), Width);
					Entry->SetNumberField(TEXT("height"), Height);
					LargeList.Add(TPair<int64, TSharedPtr<FJsonObject>>((int64)Width * Height, Entry));
				}

				// 估算纹理内存（简化计算：RGBA8 = 4 bytes per pixel）
				int64 TextureMemory = (int64)Width * Height * 4;
				TotalTextureMemory += TextureMemory;
			}
		}

		LargeList.Sort([](const TPair<int64, TSharedPtr<FJsonObject>>& A,
		                  const TPair<int64, TSharedPtr<FJsonObject>>& B)
		{
			return A.Key > B.Key;
		});

		TArray<TSharedPtr<FJsonValue>> LargeJson;
		for (int32 i = 0; i < LargeList.Num() && i < MaxListed; ++i)
		{
			LargeJson.Add(MakeShared<FJsonValueObject>(LargeList[i].Value));
		}

		TextureData->SetNumberField(TEXT("total_textures"), TotalTextures);
		TextureData->SetNumberField(TEXT("large_textures_4k"), LargeTextures4K);
		TextureData->SetArrayField(TEXT("large_textures"), LargeJson);
		if (LargeList.Num() > MaxListed)
		{
			TextureData->SetNumberField(TEXT("large_textures_omitted"), LargeList.Num() - MaxListed);
		}
		TextureData->SetNumberField(TEXT("estimated_memory_bytes"), TotalTextureMemory);
		TextureData->SetNumberField(TEXT("estimated_memory_mb"), TotalTextureMemory / (1024 * 1024));

		if (LargeTextures4K > 0)
		{
			TextureData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("发现 %d 个 4K 或更大的纹理，考虑压缩或降低分辨率以减少包体大小。"), LargeTextures4K));
		}

		Result->SetObjectField(TEXT("texture_analysis"), TextureData);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// content.fixup_redirectors —— 清理移动/重命名留下的重定向器
// ============================================================================

void FUAL_ContentBrowserCommands::Handle_FixupRedirectors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SearchPath = TEXT("/Game");
	Payload->TryGetStringField(TEXT("path"), SearchPath);
	SearchPath.TrimStartAndEndInline();
	// 先把尾部的 / 去掉再判根：`//` 在注册表里会被归一成 `/`
	while (SearchPath.Len() > 1 && SearchPath.EndsWith(TEXT("/")))
	{
		SearchPath.LeftChopInline(1);
	}
	// 根目录不接：`/` 会把 /Engine 和所有插件挂载点下的重定向器一并处理掉，远超默认的 /Game
	if (SearchPath.IsEmpty() || SearchPath == TEXT("/"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("path must be a mount root or folder such as /Game or /Game/Old - the registry root '/' is not accepted."));
		return;
	}

	bool bDryRun = false;
	Payload->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// 坏掉的重定向器（目标已经不存在）FixupReferencers 修不了。默认只报不删 ——
	// 删了之后引用它的资产会从「跟着断链走」变成「找不到对象」，那是另一种错。
	bool bDeleteBroken = false;
	Payload->TryGetBoolField(TEXT("delete_broken"), bDeleteBroken);

	// 指定清哪几个：重定向器本身的包路径，或者目录。给了它就不扫整个 path。
	// batch_move 之后只想清自己留下的那几个，全 /Game 扫一遍要几分钟。
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	// 给了 paths 就只看它，哪怕是空的：空清单的意思是「一个都不处理」，不是「全扫」
	const bool bPathsGiven = Payload->TryGetArrayField(TEXT("paths"), PathsArray) && PathsArray;
	if (bPathsGiven)
	{
		for (const TSharedPtr<FJsonValue>& Value : *PathsArray)
		{
			FString Str;
			if (Value.IsValid() && Value->TryGetString(Str) && !Str.IsEmpty())
			{
				Paths.Add(Str);
			}
		}
	}

	// 资产注册表还在扫的时候查出来的是残缺结果 —— 会漏掉一部分重定向器，
	// 然后报「清理完成」。但不在这里死等（更不催它全盘同步扫一遍）：
	// 没扫完就回 503 + 进度，盒子侧轮询重发
	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	auto IsRedirectorClass = [](const FAssetData& Data) -> bool
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return Data.AssetClassPath.GetAssetName() == UObjectRedirector::StaticClass()->GetFName();
#else
		return Data.AssetClass == UObjectRedirector::StaticClass()->GetFName();
#endif
	};

	auto CollectUnderPath = [&](const FString& Folder, TArray<FAssetData>& Out)
	{
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*Folder));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UObjectRedirector::StaticClass()->GetFName());
#endif
		AssetRegistry.GetAssets(Filter, Out);
	};

	TArray<FAssetData> RedirectorAssets;
	TArray<FString> NotRedirectors;
	if (!bPathsGiven)
	{
		CollectUnderPath(SearchPath, RedirectorAssets);
	}
	else
	{
		TSet<FName> Seen;
		for (const FString& Raw : Paths)
		{
			FString PackageName = Raw.TrimStartAndEnd();
			// 尾部带 / 的是明确指目录，不先当包名去撞：关卡 `/Game/Maps/Main` 旁边
			// 常有同名文件夹 `/Game/Maps/Main/`，先撞包会把目录里的重定向器整个漏掉
			const bool bFolderIntent = PackageName.Len() > 1 && PackageName.EndsWith(TEXT("/"));
			// 对象路径（/Game/A.A）才去掉点后面那段；目录里不会有点，带点的目录写法
			// 照原样去查，查不到就是不存在，别截成隔壁那个更短的目录
			int32 DotIndex = INDEX_NONE;
			if (!bFolderIntent && PackageName.FindChar(TEXT('.'), DotIndex))
			{
				PackageName = PackageName.Left(DotIndex);
			}
			// 截完点再去尾部的 /：`//.` 截出来是 `//`，也要归一成 `/` 才拦得住
			while (PackageName.Len() > 1 && PackageName.EndsWith(TEXT("/")))
			{
				PackageName.LeftChopInline(1);
			}
			// 根目录不接（同 path）：`/` 在注册表里是一个存在的目录，会扫遍 /Engine 和所有插件。
			// 放在归一之后判，`//`、`/.` 这类写法也拦得住
			if (PackageName.IsEmpty() || PackageName == TEXT("/"))
			{
				NotRedirectors.Add(Raw);
				continue;
			}

			TArray<FAssetData> InPackage;
			if (!bFolderIntent)
			{
				AssetRegistry.GetAssetsByPackageName(FName(*PackageName), InPackage);
			}
			if (InPackage.Num() > 0)
			{
				bool bAny = false;
				for (const FAssetData& Data : InPackage)
				{
					if (!IsRedirectorClass(Data))
					{
						continue;
					}
					// 「是不是重定向器」和「有没有重复收录」是两回事：先给了目录再给
					// 目录里的某一条，那一条已经收过了，但它仍然是重定向器
					bAny = true;
					if (!Seen.Contains(Data.PackageName))
					{
						Seen.Add(Data.PackageName);
						RedirectorAssets.Add(Data);
					}
				}
				if (!bAny)
				{
					NotRedirectors.Add(Raw);
				}
				continue;
			}
			if (AssetRegistry.PathExists(PackageName))
			{
				TArray<FAssetData> Under;
				CollectUnderPath(PackageName, Under);
				for (const FAssetData& Data : Under)
				{
					if (!Seen.Contains(Data.PackageName))
					{
						Seen.Add(Data.PackageName);
						RedirectorAssets.Add(Data);
					}
				}
				continue;
			}
			NotRedirectors.Add(Raw);
		}
	}

	// 每个重定向器指向哪、目标还在不在 —— 全部来自注册表标签，不加载
	struct FRedirectorInfo
	{
		FAssetData Data;
		FString Target;
		bool bBroken = false;
	};
	TArray<FRedirectorInfo> Infos;
	Infos.Reserve(RedirectorAssets.Num());
	int32 BrokenCount = 0;
	for (const FAssetData& Asset : RedirectorAssets)
	{
		FRedirectorInfo Info;
		Info.Data = Asset;
		FString DestTag;
		if (Asset.GetTagValue(FName(TEXT("DestinationObject")), DestTag) && !DestTag.IsEmpty())
		{
			// 标签值形如 StaticMesh'/Game/New/SM_Rock.SM_Rock'
			Info.Target = FPackageName::ExportTextPathToObjectPath(DestTag);
			FString TargetPackage = Info.Target;
			int32 DotIndex = INDEX_NONE;
			if (TargetPackage.FindChar(TEXT('.'), DotIndex))
			{
				TargetPackage = TargetPackage.Left(DotIndex);
			}
			TArray<FAssetData> TargetAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*TargetPackage), TargetAssets);
			// 蓝图搬迁会在旧包里多留两条重定向器：`BP_X_C` 指向生成类、`Default__BP_X_C`
			// 指向 CDO。类和 CDO 的 `IsAsset()` 永远是 false，注册表里根本没有它们的对象路径，
			// 这两种只能退回按包判断（包在就算好），否则每次搬蓝图都会被报成断链
			const FString TargetObjectName = FPackageName::ObjectPathToObjectName(Info.Target);
			const bool bTargetIsClassOrCdo =
				TargetObjectName.EndsWith(TEXT("_C")) || TargetObjectName.StartsWith(TEXT("Default__"));
			if (bTargetIsClassOrCdo)
			{
				Info.bBroken = TargetAssets.Num() == 0;
			}
			else
			{
				// 按对象路径比，不按包：包还在、里面那个对象没了的，加载后 DestinationObject
				// 一样是空 —— 预演不这么判，执行时就会冒出预演没警告过的「坏」
				Info.bBroken = !TargetAssets.ContainsByPredicate([&Info](const FAssetData& Candidate)
				{
					return UALCompat::GetObjectPathString(Candidate).Equals(Info.Target, ESearchCase::IgnoreCase);
				});
			}
		}
		else
		{
			Info.bBroken = true;
		}
		if (Info.bBroken)
		{
			++BrokenCount;
		}
		Infos.Add(Info);
	}

	TArray<TSharedPtr<FJsonValue>> Listed;
	TArray<TSharedPtr<FJsonValue>> Details;
	for (const FRedirectorInfo& Info : Infos)
	{
		// 最多列 200 条：几千个重定向器的工程是有的，全塞进响应只会把上下文
		// 撑爆。调用方需要的是「有多少、要不要清」，不是每一条的名字。
		if (Listed.Num() >= 200)
		{
			break;
		}
		const FString PackageName = Info.Data.PackageName.ToString();
		Listed.Add(MakeShared<FJsonValueString>(PackageName));
		TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("path"), PackageName);
		if (!Info.Target.IsEmpty())
		{
			Detail->SetStringField(TEXT("target"), Info.Target);
		}
		if (Info.bBroken)
		{
			Detail->SetBoolField(TEXT("broken"), true);
		}
		Details.Add(MakeShared<FJsonValueObject>(Detail));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	// 名字清单最多列 200 条（同 details），超出的只报总数 <field>_total：
	// 几千条路径全塞进响应会把调用方的上下文撑爆
	auto SetStringArray = [&Result](const TCHAR* Field, const TArray<FString>& Values)
	{
		constexpr int32 MaxListed = 200;
		TArray<TSharedPtr<FJsonValue>> Json;
		for (int32 Index = 0; Index < Values.Num() && Index < MaxListed; ++Index)
		{
			Json.Add(MakeShared<FJsonValueString>(Values[Index]));
		}
		Result->SetArrayField(Field, Json);
		if (Values.Num() > MaxListed)
		{
			Result->SetNumberField(FString(Field) + TEXT("_total"), Values.Num());
		}
	};
	Result->SetStringField(TEXT("path"), bPathsGiven ? TEXT("(paths)") : SearchPath);
	Result->SetNumberField(TEXT("found"), RedirectorAssets.Num());
	// broken_count 每条路各写一次：预演和「都加载失败」用标签口径，执行用加载确认后的口径
	Result->SetArrayField(TEXT("redirectors"), Listed);
	Result->SetArrayField(TEXT("details"), Details);
	if (RedirectorAssets.Num() > Listed.Num())
	{
		Result->SetStringField(
			TEXT("listed_note"),
			FString::Printf(TEXT("Listing %d of %d."), Listed.Num(), RedirectorAssets.Num()));
	}
	if (NotRedirectors.Num() > 0)
	{
		SetStringArray(TEXT("not_redirectors"), NotRedirectors);
	}

	if (RedirectorAssets.Num() == 0)
	{
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetNumberField(TEXT("fixed"), 0);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetStringField(TEXT("note"), NotRedirectors.Num() > 0
			? TEXT("None of the given paths is a redirector or a folder containing redirectors - nothing to clean up.")
			: TEXT("No redirectors found - nothing to clean up."));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// 引用者能不能写：FixupReferencers 要改写每个引用者（签出 / 只读），改不了的
	// 那个重定向器就清不掉。判定和 batch_move 同一套（照抄引擎 AutoCheckOut），
	// 只读注册表和文件属性，不加载
	int32 BlockedReferencers = 0;
	// FixupReferencers 会把每个引用者**原样落盘**（bCheckDirty=false、不问）：用户改到
	// 一半还没存的引用者也一起存了，"丢弃改动"这条路就没了。事前点名，预演就报
	TArray<FString> DirtyReferencers;
	{
		TArray<FUAL_PackageWriteState> In;
		TMap<FString, int32> IndexByPackage;
		// 坏的重定向器的引用者：不进签出预检，但脏不脏还要查 —— 标签说坏、加载却好的
		// 会交给 FixupReferencers，它照样把这些引用者原样落盘
		TArray<FString> BrokenReferencers;
		for (const FRedirectorInfo& Info : Infos)
		{
			// 执行时会跳过这些（标签说坏、又不打算删）：它们的包和引用者不会被写，
			// 进了预检只会把「谁挡住了」算错，还白跑一轮 SCC 状态查询
			if (Info.bBroken && !bDeleteBroken)
			{
				continue;
			}
			const FString RedirectorPackage = Info.Data.PackageName.ToString();
			const FString RedirectorKey = RedirectorPackage.ToLower();
			// 重定向器自己的包也要能写：引擎删它之前会看它的签出状态（5.5 甚至看完
			// 照删，文件却留在磁盘上），被别人签出 / 只读的事前就得报出来。
			// 先作为别人的引用者进来过的（链式重定向），角色也要改成 redirector
			if (int32* Existing = IndexByPackage.Find(RedirectorKey))
			{
				In[*Existing].Role = TEXT("redirector");
			}
			else
			{
				FUAL_PackageWriteState Self;
				Self.Package = RedirectorPackage;
				Self.Role = TEXT("redirector");
				IndexByPackage.Add(RedirectorKey, In.Add(Self));
			}
			TArray<FString> Referencers;
			UAL_CollectReferencers(AssetRegistry, Info.Data.PackageName, Referencers);
			// 坏的（打算删的）只走 ForceDeleteObjects：它不签出、不存引用者，只在内存里
			// 把引用置空。它的引用者写不写得了和删不删得掉无关，不进预检，只记下来查脏
			if (Info.bBroken)
			{
				for (const FString& Referencer : Referencers)
				{
					BrokenReferencers.AddUnique(Referencer);
				}
				continue;
			}
			for (const FString& Referencer : Referencers)
			{
				const FString ReferencerKey = Referencer.ToLower();
				if (int32* Existing = IndexByPackage.Find(ReferencerKey))
				{
					In[*Existing].For.AddUnique(RedirectorPackage);
					continue;
				}
				FUAL_PackageWriteState State;
				State.Package = Referencer;
				State.Role = TEXT("referencer");
				State.For.Add(RedirectorPackage);
				IndexByPackage.Add(ReferencerKey, In.Add(State));
			}
		}
		// 脏不脏按**最终角色**查，和 Infos 的顺序无关：链式重定向里自己也要被删的包
		// 不算「会被原样落盘的引用者」
		TArray<FString> ToProbe;
		for (const FUAL_PackageWriteState& State : In)
		{
			if (State.Role == TEXT("referencer"))
			{
				ToProbe.Add(State.Package);
			}
		}
		for (const FString& Referencer : BrokenReferencers)
		{
			// 已经在预检里的按它的最终角色算过了（要被删的重定向器包不算）
			if (!IndexByPackage.Contains(Referencer.ToLower()))
			{
				ToProbe.Add(Referencer);
			}
		}
		for (const FString& Package : ToProbe)
		{
			if (const UPackage* Loaded = FindPackage(nullptr, *Package))
			{
				if (Loaded->IsDirty())
				{
					DirtyReferencers.Add(Package);
				}
			}
		}
		FUAL_PreflightResult Preflight;
		UAL_PreflightPackages(In, Preflight);
		BlockedReferencers = Preflight.BlockedCount;
		Result->SetObjectField(TEXT("checkout"), UAL_PreflightJson(Preflight));
	}
	if (DirtyReferencers.Num() > 0)
	{
		SetStringArray(TEXT("dirty_referencers"), DirtyReferencers);
	}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	// 判据见下面拒绝那一段的注释；提前算好，预演和「原地未动」的提示也要用
	const bool bReportWindowUnreachable =
		GIsRunningUnattendedScript
		|| IsRunningCommandlet()
		|| !FSlateApplication::IsInitialized()
		|| !FSlateApplication::Get().CanAddModalWindow();
#else
	const bool bReportWindowUnreachable = false;
#endif
	const bool bFixupRefusedHere = bReportWindowUnreachable || FApp::IsUnattended();
	// 这个编辑器里带 delete_broken=true 的真跑会被 409 拒掉，别再劝调用方这么做
	const FString DeleteBrokenAdvice = bFixupRefusedHere
		? TEXT("delete_broken=true is refused in this editor (-unattended, script mode or no renderer) - remove them from an interactive editor.")
		: TEXT("pass delete_broken=true to remove them.");
	const FString DirtyReferencersNote = DirtyReferencers.Num() > 0
		? FString::Printf(TEXT(" %d referencer package(s) have unsaved edits in the editor (dirty_referencers); FixupReferencers saves referencers to disk as-is, so those edits get committed too - save or revert them first if that is not wanted."), DirtyReferencers.Num())
		: FString();

	if (bDryRun)
	{
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetNumberField(TEXT("fixed"), 0);
		Result->SetNumberField(TEXT("broken_count"), BrokenCount);
		Result->SetBoolField(TEXT("dry_run"), true);
		FString Note = TEXT("Dry run - nothing was changed. Re-run with dry_run=false to rewrite referencers and delete these redirectors.");
		if (BrokenCount > 0)
		{
			Note += FString::Printf(TEXT(" %d redirector(s) are broken (target no longer exists) and cannot be fixed up; %s"), BrokenCount, *DeleteBrokenAdvice);
		}
		if (bFixupRefusedHere && (bDeleteBroken || BrokenCount < Infos.Num()))
		{
			Note += TEXT(" This editor cannot run the real fixup (-unattended, script mode or no renderer) - dry_run=false will be refused with 409.");
		}
		if (BlockedReferencers > 0)
		{
			Note += FString::Printf(TEXT(" %d package(s) cannot be written (checkout.blocking; role tells whether it is a referencer or the redirector itself) - the redirectors involved will not be cleaned up until that is resolved; the rest still will."), BlockedReferencers);
		}
		Note += DirtyReferencersNote;
		Result->SetStringField(TEXT("note"), Note);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	/**
	 * 报告框到不了人手上时直接拒绝，别去碰 `FixupReferencers`。
	 *
	 * 它收尾时无条件弹「Redirector Update Report」模态框（UE 5.4 引进）。框被自动
	 * 取消时，5.4 只是当作「保留重定向器」；5.5 起 `SModalEditorDialog::ShowModalDialog`
	 * 去读一个从没被设置的 `TOptional` —— 编辑器当场崩，用户没保存的东西一起没
	 * （真机三次：5.5 一次、5.8 两次）。详见下面 `FixupReferencers` 调用点上方的注释。
	 *
	 * 什么时候框会被取消，照 `FSlateApplication::AddModalWindow` 的判据来：
	 * `GIsRunningUnattendedScript`（脚本模式，窗口直接被取消）或
	 * `!CanAddModalWindow()`（渲染器没起来，窗口根本建不出来）。5.4 以前没有这个框。
	 *
	 * `-unattended`（`FApp::IsUnattended`）不会取消这个框，但照样要拒：所有版本的
	 * `PromptForCheckoutAndSave` 在 -unattended 下直接返回 PR_Cancelled 且不填失败清单，
	 * 引用者一个都存不了，随后引擎的删除闸又因为磁盘上还有引用而整批拒删 ——
	 * 跑完等于什么都没做，还没有任何日志说明原因。
	 *
	 * 报 409 而不是静默跳过：调用方点名要清理，得知道为什么没清。
	 *
	 * 例外：标签全说坏、又不打算删（默认）—— 一个都不会加载，`FixupReferencers`
	 * 根本不会被调用，没有框也没有存盘，直接按「原地未动」回。打算删的不能这么绕：
	 * 标签说坏但加载后目标还在的会进 FixupReferencers。
	 */
	const bool bNothingWouldReachFixup = !bDeleteBroken && BrokenCount == Infos.Num();
	if (bFixupRefusedHere && !bNothingWouldReachFixup)
	{
		TSharedPtr<FJsonObject> RefusalDetails = MakeShared<FJsonObject>();
		RefusalDetails->SetNumberField(TEXT("found"), RedirectorAssets.Num());
		RefusalDetails->SetNumberField(TEXT("broken_count"), BrokenCount);
		RefusalDetails->SetStringField(
			TEXT("how_to"),
			TEXT("Open the Content Browser, right-click the folder and pick 'Fix Up Redirectors' - or re-run this command with the editor in normal interactive mode (no -unattended) and click the report window when it appears."));
		UAL_CommandUtils::SendError(
			RequestId, 409,
			bReportWindowUnreachable
				? TEXT("Refusing to fix up redirectors: IAssetTools::FixupReferencers opens a modal report window on UE 5.4+, and this editor cannot show one (script mode or no renderer) - the window would be auto-cancelled, which on UE 5.5+ crashes the editor. Redirectors are harmless in the meantime; the engine follows them.")
				: TEXT("Refusing to fix up redirectors: the editor runs -unattended, where the engine cancels the referencer save (PromptForCheckoutAndSave returns PR_Cancelled) and then refuses to delete redirectors that are still referenced on disk - the fixup would silently do nothing. Redirectors are harmless in the meantime; the engine follows them."),
			RefusalDetails);
		return;
	}

	// FixupReferencers 要的是加载好的对象，不是 FAssetData。坏掉的单独处理。
	//
	// 删不删只信「预演和加载**都**说坏」：注册表落后于磁盘时标签说坏、加载却好的，
	// 按能修的走；标签说好、加载后为空的，预演从没警告过，不删，只报出来。
	// 每条重定向器这次是怎么处置的。结果按**跑完之后各自还在不在**来数，不做减法：
	// 引擎修完一个包会把这个包里的重定向器全删（蓝图的 BP_X_C 就是这么跟着没的），
	// 靠计数器相减会把已经没了的说成「原地未动」
	enum class EOutcome : uint8 { Handed, Skipped, Broken, BrokenAfterLoad, LoadFailed };
	TArray<EOutcome> Outcomes;
	Outcomes.Init(EOutcome::Skipped, Infos.Num());
	TArray<UObjectRedirector*> Redirectors;
	TArray<UObject*> BrokenObjects;
	TArray<FString> BrokenAfterLoad;
	TArray<FString> LoadFailures;
	int32 RecoveredByLoad = 0;
	for (int32 Index = 0; Index < Infos.Num(); ++Index)
	{
		const FRedirectorInfo& Info = Infos[Index];
		// 标签说坏了的、又不打算删的，不用加载：加载一个目标缺失的重定向器只会
		// 刷一屏 LogLinker 警告，然后原地不动
		if (Info.bBroken && !bDeleteBroken)
		{
			continue;
		}
		UObjectRedirector* Redirector = Cast<UObjectRedirector>(Info.Data.GetAsset());
		if (!Redirector)
		{
			Outcomes[Index] = EOutcome::LoadFailed;
			LoadFailures.Add(Info.Data.PackageName.ToString());
			continue;
		}
		if (Redirector->DestinationObject != nullptr)
		{
			if (Info.bBroken)
			{
				++RecoveredByLoad;
			}
			Outcomes[Index] = EOutcome::Handed;
			Redirectors.Add(Redirector);
		}
		else if (Info.bBroken)
		{
			Outcomes[Index] = EOutcome::Broken;
			BrokenObjects.Add(Redirector);
		}
		else
		{
			Outcomes[Index] = EOutcome::BrokenAfterLoad;
			BrokenAfterLoad.Add(Info.Data.PackageName.ToString());
		}
	}

	if (LoadFailures.Num() > 0 && LoadFailures.Num() == Infos.Num())
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetNumberField(TEXT("fixed"), 0);
		Result->SetNumberField(TEXT("broken_count"), BrokenCount);
		Result->SetBoolField(TEXT("dry_run"), false);
		Result->SetStringField(
			TEXT("error"),
			TEXT("Found redirectors in the asset registry but none of them could be loaded."));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	/**
	 * `bCheckoutDialogPrompt=false` 是必须的。
	 *
	 * 默认那个 true 会在需要签出文件时弹一个模态框 —— 无人值守场景下没人点，
	 * 编辑器就卡死在那儿。`FEditorFileUtils::SaveLevelAs` 是同一个坑。
	 *
	 * FixupMode 用默认的 DeleteFixedUpRedirectors：改完引用就把重定向器删掉，
	 * 这正是这个命令的目的 —— 只改引用不删，垃圾还在原地。
	 *
	 * ## 报告框被自动取消时这一句会崩（5.5+），所以上面先挡了一道
	 *
	 * `AssetFixUpRedirectors.cpp` 的 `ExecuteFixUp` 收尾时无条件弹一个
	 * 「Redirector Update Report」模态框（5.4 引进）。5.4 的 `ShowModalDialog`
	 * 在窗口被取消时返回默认构造的 false（保留重定向器，不崩）；5.5 起改成
	 * `TOptional`，窗口被取消后 `Result.GetValue()` 直接断言，**编辑器当场崩掉**。
	 * `bCheckoutDialogPrompt` 管不到它，它在那个分支之外。
	 *
	 * 所以：这条命令**只在框弹得出来的时候跑**（由人点一下），
	 * 弹不出来的前面已经返回 409 了。`content.batch_move` 里那一步则彻底删掉。
	 *
	 * 引擎的失败原因不走 LogAssetTools：≤5.3 走 FMessageLog("EditorErrors")、Perforce
	 * 走 FMessageLog("SourceControl")、存盘失败走 LogFileHelpers、删不掉走 LogObjectTools；
	 * 5.4+ 的失败只显示在报告框里，日志里什么都没有。
	 */
	// 真要动东西才做前后快照：全量遍历 UPackage 两遍不便宜，什么都不会跑的调用别付这笔账
	const bool bWillMutate = Redirectors.Num() > 0 || (bDeleteBroken && BrokenObjects.Num() > 0);
	const TSet<FName> DirtyBefore = bWillMutate ? UAL_DirtySavablePackageNames() : TSet<FName>();
	TArray<TSharedPtr<FJsonValue>> EngineLog;
	if (bWillMutate)
	{
		// 捕获范围要盖住删除那一步：SCC 拒删、OnAssetsCanDelete 否决都只写日志。
		// ≤5.3 整批被引擎删除闸拒掉时那句「Could not delete」在 LogUObjectGlobals
		FUAL_ScopedLogCapture LogCapture(
			{ TEXT("LogAssetTools"), TEXT("LogSourceControl"), TEXT("EditorErrors"), TEXT("SourceControl"), TEXT("LogFileHelpers"), TEXT("LogObjectTools"), TEXT("LogUObjectGlobals") },
			ELogVerbosity::Warning);
		// 引擎修完一个包会把包里**所有**重定向器删掉并做一次完整 GC：同包里坏的那条
		// 可能已经被释放了。先换成弱指针，跑完只删还活着的，别把野指针交给 ForceDeleteObjects
		TArray<TWeakObjectPtr<UObject>> BrokenWeak;
		for (UObject* Object : BrokenObjects)
		{
			BrokenWeak.Add(Object);
		}
		if (Redirectors.Num() > 0)
		{
			AssetToolsModule.Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt=*/false);
		}
		TArray<UObject*> BrokenAlive;
		for (const TWeakObjectPtr<UObject>& Weak : BrokenWeak)
		{
			if (UObject* Object = Weak.Get())
			{
				BrokenAlive.Add(Object);
			}
		}
		if (bDeleteBroken && BrokenAlive.Num() > 0)
		{
			TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
			ObjectTools::ForceDeleteObjects(BrokenAlive, /*bShowConfirmation=*/false);
		}
		EngineLog = LogCapture.ToJson();
	}

	// 改完逐条回读，用**实际还在不在**报结果，而不是拿请求数当成功数。
	// 注册表说没了还要看一眼磁盘：引擎是先删注册表条目再去删文件的，SCC 拒绝
	// （被别人签出、不是最新）或只读文件让删除失败时，文件还在、注册表却已经没了 ——
	// 按注册表数会把它报成「清掉了」，下次扫描它又回来。这种要算回「还在」，并把
	// 注册表条目补回去。
	int32 RemainingCount = 0;
	int32 FixedCount = 0;
	int32 Unfixed = 0;
	int32 BrokenLeft = 0;
	int32 DeletedBroken = 0;
	TArray<FString> LeftOnDisk;
	for (int32 Index = 0; Index < Infos.Num(); ++Index)
	{
		const FRedirectorInfo& Info = Infos[Index];
		TArray<FAssetData> Still;
		AssetRegistry.GetAssetsByPackageName(Info.Data.PackageName, Still);
		bool bPresent = Still.ContainsByPredicate(IsRedirectorClass);
		if (!bPresent && Outcomes[Index] != EOutcome::LoadFailed)
		{
			FString Filename;
			if (FPackageName::DoesPackageExist(Info.Data.PackageName.ToString(), &Filename)
				&& IFileManager::Get().FileExists(*Filename))
			{
				bPresent = true;
				LeftOnDisk.Add(Filename);
			}
		}
		if (bPresent)
		{
			++RemainingCount;
		}
		switch (Outcomes[Index])
		{
		case EOutcome::Handed:
			(bPresent ? Unfixed : FixedCount)++;
			break;
		case EOutcome::Broken:
			(bPresent ? BrokenLeft : DeletedBroken)++;
			break;
		case EOutcome::Skipped:
		case EOutcome::BrokenAfterLoad:
			// 没交给引擎的，也可能作为同一个包里的兄弟被顺手删了（蓝图的 BP_X_C）
			(bPresent ? BrokenLeft : FixedCount)++;
			break;
		case EOutcome::LoadFailed:
			break;
		}
	}
	if (LeftOnDisk.Num() > 0)
	{
		AssetRegistry.ScanFilesSynchronous(LeftOnDisk, /*bForceRescan=*/true);
		SetStringArray(TEXT("left_on_disk"), LeftOnDisk);
	}

	// 引用者：跑之前点名的那些，现在存了没有。FixupReferencers 只存交给它的那些
	// 重定向器的引用者，跳过的、加载失败的、删除路径上的都不会存 —— 事后按 IsDirty 回读，
	// 「存了」和「还没存」分两栏说，别拿跑之前的预测当已经发生的事
	TArray<FString> SavedReferencers;
	TArray<FString> StillDirtyReferencers;
	for (const FString& Name : DirtyReferencers)
	{
		const UPackage* Loaded = FindPackage(nullptr, *Name);
		((Loaded && Loaded->IsDirty()) ? StillDirtyReferencers : SavedReferencers).Add(Name);
	}

	// 改引用会把引用者标脏；但引擎在删重定向器之前就把它们存了，正常跑完什么都不剩。
	// 只报**这条命令新弄脏的**（引擎没存成的那几个），跑之前就脏的是用户自己改到一半
	// 的东西，不能算在这里，更不能劝调用方去存 —— editor.save 默认 scope=touched
	// 也不会存它们，两个入口会给出两个答案。
	// 判据仍和 editor.save 同一个（`UAL_SavablePackage.h`）。
	const int32 DirtyCount = bWillMutate ? UAL_NewlyDirtySavablePackageNames(DirtyBefore).Num() : 0;

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("fixed"), FixedCount);
	Result->SetNumberField(TEXT("remaining"), RemainingCount);
	Result->SetNumberField(TEXT("deleted_broken"), DeletedBroken);
	// 执行后用加载确认过的口径，免得一份响应里两个「坏」的数
	Result->SetNumberField(TEXT("broken_count"), BrokenLeft + DeletedBroken);
	Result->SetNumberField(TEXT("broken_left"), BrokenLeft);
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetNumberField(TEXT("dirty_after"), DirtyCount);
	Result->SetArrayField(TEXT("engine_log"), EngineLog);

	if (LoadFailures.Num() > 0)
	{
		SetStringArray(TEXT("load_failed"), LoadFailures);
	}
	if (BrokenAfterLoad.Num() > 0)
	{
		SetStringArray(TEXT("broken_after_load"), BrokenAfterLoad);
	}
	// 执行后 dirty_referencers 只留「还没存」的，存了的单独一栏
	Result->RemoveField(TEXT("dirty_referencers"));
	Result->RemoveField(TEXT("dirty_referencers_total"));
	if (StillDirtyReferencers.Num() > 0)
	{
		SetStringArray(TEXT("dirty_referencers"), StillDirtyReferencers);
	}
	if (SavedReferencers.Num() > 0)
	{
		SetStringArray(TEXT("saved_referencers"), SavedReferencers);
	}

	FString Note = FString::Printf(
		TEXT("Rewrote referencers and removed %d of %d redirector(s)."),
		FixedCount, RedirectorAssets.Num());
	if (DeletedBroken > 0)
	{
		Note += FString::Printf(TEXT(" Deleted %d broken redirector(s) on top of that."), DeletedBroken);
	}
	if (RecoveredByLoad > 0)
	{
		Note += FString::Printf(
			TEXT(" %d flagged broken by the asset registry turned out to load fine, so they were handed to the fixup instead of being deleted (whether each one is gone is counted above)."),
			RecoveredByLoad);
	}
	if (BrokenLeft > 0)
	{
		Note += bDeleteBroken
			? FString::Printf(
				TEXT(" %d broken redirector(s) are still there - the delete was vetoed, an asset editor still holds them, or the file is read-only / locked (see left_on_disk and engine_log)."),
				BrokenLeft)
			: FString::Printf(
				TEXT(" %d broken redirector(s) (target missing) were left in place - %s"),
				BrokenLeft, *DeleteBrokenAdvice);
	}
	if (BrokenAfterLoad.Num() > 0)
	{
		Note += FString::Printf(
			TEXT(" %d redirector(s) whose target exists in the registry came back with a null destination after loading (broken_after_load); they were not deleted. The asset registry still lists their target, so a dry run will keep calling them healthy until the registry catches up - rescan the target's folder or restart the editor before re-running."),
			BrokenAfterLoad.Num());
	}
	if (LoadFailures.Num() > 0)
	{
		Note += FString::Printf(TEXT(" %d redirector package(s) failed to load and were skipped (load_failed)."), LoadFailures.Num());
	}
	if (LeftOnDisk.Num() > 0)
	{
		Note += FString::Printf(
			TEXT(" %d redirector file(s) were dropped from the asset registry but still exist on disk (source control or a read-only file refused the delete; left_on_disk, rescanned) - they count as remaining."),
			LeftOnDisk.Num());
	}
	if (Unfixed > 0)
	{
		bool bExplained = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		if (BlockedReferencers == 0 && LeftOnDisk.Num() == 0 && LoadFailures.Num() == 0 && Unfixed == Redirectors.Num())
		{
			Note += FString::Printf(
				TEXT(" %d were not removed although every package was writable - that usually means the report window was closed with 'Keep Redirectors' (the referencers were still rewritten and saved); re-run and click 'Delete Unreferenced Redirectors'. engine_log has the engine's own lines if something else went wrong."),
				Unfixed);
			bExplained = true;
		}
#endif
		if (!bExplained)
		{
			Note += FString::Printf(
				TEXT(" %d could not be removed - a referencer or the redirector package itself is not writable, a referencer failed to load or save, or the engine refused the whole delete batch because a redirector is still referenced on disk (see checkout.blocking, load_failed, left_on_disk and engine_log)."),
				Unfixed);
		}
	}
	if (SavedReferencers.Num() > 0)
	{
		Note += FString::Printf(
			TEXT(" %d referencer package(s) that had unsaved edits were saved to disk as-is by the engine (saved_referencers)."),
			SavedReferencers.Num());
	}
	if (StillDirtyReferencers.Num() > 0)
	{
		Note += FString::Printf(
			TEXT(" %d referencer package(s) with unsaved edits were not written (dirty_referencers) - their redirectors were not handed to the engine or the save failed."),
			StillDirtyReferencers.Num());
	}
	if (DirtyCount > 0)
	{
		Note += FString::Printf(
			TEXT(" %d package(s) were newly dirtied by this command and the engine did not save them - call editor.save."),
			DirtyCount);
	}
	Result->SetStringField(TEXT("note"), Note);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// content.asset_ranking —— 全工程资源占用排行
// ============================================================================
//
// ## 和另外两个体检命令的分工
//
// - `content.audit_optimization`：整体什么状况（Nanite 启用率、贴图总量）
// - `level.query_assets`：**当前关卡**里哪几个网格重
// - 本命令：**整个工程**里哪几个资产占地方，任意类型，逐条列出来
//
// ## 为什么它不加载资产
//
// audit_optimization 是 `AssetData.GetAsset()` 逐个加载的。一万个资产的工程
// 要跑几分钟，还会把整个工程拽进内存 —— 而它要的只是几个数字。
//
// 这里的数据有两个来源，都不需要加载：
//
//   1. **资产注册表标签**（Triangles / Vertices / LODs / NaniteEnabled /
//      Dimensions / Format）。这些是**保存资产时**由引擎写进 uasset 头里的，
//      注册表启动时就读好了。
//   2. **包文件在磁盘上的字节数**。问文件系统，一次 stat。
//
// ## 标签缺失是常态，不能当成零
//
// 标签在保存那一刻写入。用老引擎存过、之后没再存过的资产可能没有某个标签
// （比如 5.0 时期的网格没有 `NaniteEnabled`）。缺了就**不输出这个字段**，
// 而不是补个 0 —— 「这个网格 0 个三角形」会让调用方得出完全相反的结论。
// 缺了多少个会在 notes 里说明。

namespace
{
	/** 包在磁盘上的真实字节数。找不到文件回 0 —— 不猜 */
	int64 UAL_PackageFileSize(const FString& PackageName)
	{
		auto SizeWithExtension = [&PackageName](const FString& Extension) -> int64
		{
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Extension))
			{
				return -1;
			}
			return IFileManager::Get().FileSize(*Filename);
		};

		const int64 AssetSize = SizeWithExtension(FPackageName::GetAssetPackageExtension());
		if (AssetSize >= 0)
		{
			return AssetSize;
		}
		// 关卡是 .umap，不是 .uasset
		const int64 MapSize = SizeWithExtension(FPackageName::GetMapPackageExtension());
		return MapSize >= 0 ? MapSize : 0;
	}

	/** "2048x1024" → 2048 / 1024。解析不出来返回 false */
	bool UAL_ParseDimensions(const FString& Text, int32& OutWidth, int32& OutHeight)
	{
		FString Left;
		FString Right;
		if (!Text.Split(TEXT("x"), &Left, &Right))
		{
			return false;
		}
		OutWidth = FCString::Atoi(*Left.TrimStartAndEnd());
		OutHeight = FCString::Atoi(*Right.TrimStartAndEnd());
		return OutWidth > 0 && OutHeight > 0;
	}

	/**
	 * 按像素格式名估算贴图字节数。
	 *
	 * 格式表直接问引擎的 GPixelFormats（`Format` 标签存的就是那张表里的 Name），
	 * 不自己维护一份对照表 —— 新格式是引擎加的，自己维护的表只会越来越旧。
	 *
	 * @return 认不出格式时返回 -1。**调用方不要把它当 0** ——
	 *         「这张 8K 贴图占 0 字节」比没有这个数字更糟。
	 */
	int64 UAL_TextureBytesFromFormat(const FString& FormatName, int32 Width, int32 Height)
	{
		if (Width <= 0 || Height <= 0 || FormatName.IsEmpty())
		{
			return -1;
		}

		for (int32 Index = 0; Index < PF_MAX; ++Index)
		{
			const FPixelFormatInfo& Info = GPixelFormats[Index];
			if (!Info.Name || !FormatName.Equals(Info.Name, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const int64 BlockX = FMath::Max<int64>(Info.BlockSizeX, 1);
			const int64 BlockY = FMath::Max<int64>(Info.BlockSizeY, 1);
			const int64 Blocks = FMath::DivideAndRoundUp((int64)Width, BlockX) * FMath::DivideAndRoundUp((int64)Height, BlockY);
			const int64 BaseBytes = Blocks * FMath::Max<int64>(Info.BlockBytes, 1);
			// mip 链把总量抬到约 4/3
			return BaseBytes * 4 / 3;
		}
		return -1;
	}

	/** 排行榜里的一条 */
	struct FUAL_RankedAsset
	{
		FString Name;
		FString Path;
		FString Class;
		int64 DiskSize = 0;
		int32 Triangles = -1;
		int32 Vertices = -1;
		int32 Lods = -1;
		int32 NaniteState = -1; // -1 没有这个标签，0 关，1 开
		int32 Width = 0;
		int32 Height = 0;
		FString Format;
		int64 TextureBytes = -1;
	};

	/** 一个可选的整数标签。没有这个标签就保持 -1 */
	void UAL_ReadIntTag(const FAssetData& AssetData, const TCHAR* Tag, int32& OutValue)
	{
		int32 Value = 0;
		if (AssetData.GetTagValue<int32>(FName(Tag), Value))
		{
			OutValue = Value;
		}
	}
}

void FUAL_ContentBrowserCommands::Handle_AssetRanking(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ScopePath = TEXT("/Game");
	Payload->TryGetStringField(TEXT("path"), ScopePath);
	if (ScopePath.IsEmpty())
	{
		ScopePath = TEXT("/Game");
	}

	FString ClassFilter;
	Payload->TryGetStringField(TEXT("class_filter"), ClassFilter);

	FString SortBy = TEXT("DiskSize");
	Payload->TryGetStringField(TEXT("sort_by"), SortBy);

	int32 Limit = 30;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 200);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*ScopePath));
	if (!ClassFilter.IsEmpty())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *ClassFilter));
#else
		Filter.ClassNames.Add(FName(*ClassFilter));
#endif
	}

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<FUAL_RankedAsset> Items;
	Items.Reserve(AssetList.Num());

	// 按类型的汇总：整个工程的大头在哪一类资产上，这一条比逐个资产更早有用
	TMap<FString, int32> ClassCount;
	TMap<FString, int64> ClassBytes;

	int64 TotalDiskSize = 0;
	int32 MissingSizeTag = 0;

	for (const FAssetData& AssetData : AssetList)
	{
		FUAL_RankedAsset Item;
		Item.Name = AssetData.AssetName.ToString();
		Item.Path = AssetData.PackageName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Item.Class = AssetData.AssetClassPath.GetAssetName().ToString();
#else
		Item.Class = AssetData.AssetClass.ToString();
#endif
		Item.DiskSize = UAL_PackageFileSize(Item.Path);

		UAL_ReadIntTag(AssetData, TEXT("Triangles"), Item.Triangles);
		UAL_ReadIntTag(AssetData, TEXT("Vertices"), Item.Vertices);
		UAL_ReadIntTag(AssetData, TEXT("LODs"), Item.Lods);

		FString NaniteTag;
		if (AssetData.GetTagValue(FName(TEXT("NaniteEnabled")), NaniteTag))
		{
			Item.NaniteState = NaniteTag.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? 1 : 0;
		}

		FString Dimensions;
		if (AssetData.GetTagValue(FName(TEXT("Dimensions")), Dimensions) && UAL_ParseDimensions(Dimensions, Item.Width, Item.Height))
		{
			AssetData.GetTagValue(FName(TEXT("Format")), Item.Format);
			Item.TextureBytes = UAL_TextureBytesFromFormat(Item.Format, Item.Width, Item.Height);
		}

		if (Item.DiskSize == 0)
		{
			MissingSizeTag++;
		}
		TotalDiskSize += Item.DiskSize;
		ClassCount.FindOrAdd(Item.Class)++;
		ClassBytes.FindOrAdd(Item.Class) += Item.DiskSize;

		Items.Add(MoveTemp(Item));
	}

	const int32 ScannedCount = Items.Num();

	Items.StableSort([&SortBy](const FUAL_RankedAsset& A, const FUAL_RankedAsset& B)
	{
		auto GetKey = [&SortBy](const FUAL_RankedAsset& X) -> int64
		{
			if (SortBy.Equals(TEXT("Triangles"), ESearchCase::IgnoreCase))
			{
				return X.Triangles;
			}
			if (SortBy.Equals(TEXT("Vertices"), ESearchCase::IgnoreCase))
			{
				return X.Vertices;
			}
			if (SortBy.Equals(TEXT("TextureMemory"), ESearchCase::IgnoreCase))
			{
				// 认不出格式的 -1 排在最后，而不是被当成 0 混进中间
				return X.TextureBytes;
			}
			return X.DiskSize;
		};
		return GetKey(A) > GetKey(B);
	});

	const bool bTruncated = Items.Num() > Limit;
	if (bTruncated)
	{
		Items.SetNum(Limit);
	}

	TArray<TSharedPtr<FJsonValue>> AssetsJson;
	for (const FUAL_RankedAsset& Item : Items)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Item.Name);
		Obj->SetStringField(TEXT("path"), Item.Path);
		Obj->SetStringField(TEXT("class"), Item.Class);
		Obj->SetNumberField(TEXT("disk_size"), Item.DiskSize);

		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		if (Item.Triangles >= 0) Stats->SetNumberField(TEXT("triangles"), Item.Triangles);
		if (Item.Vertices >= 0) Stats->SetNumberField(TEXT("vertices"), Item.Vertices);
		if (Item.Lods >= 0) Stats->SetNumberField(TEXT("lod_count"), Item.Lods);
		if (Item.NaniteState >= 0) Stats->SetBoolField(TEXT("nanite"), Item.NaniteState == 1);
		if (Item.Width > 0 && Item.Height > 0)
		{
			Stats->SetNumberField(TEXT("width"), Item.Width);
			Stats->SetNumberField(TEXT("height"), Item.Height);
		}
		if (!Item.Format.IsEmpty()) Stats->SetStringField(TEXT("format"), Item.Format);
		if (Item.TextureBytes >= 0) Stats->SetNumberField(TEXT("texture_bytes"), Item.TextureBytes);
		Obj->SetObjectField(TEXT("stats"), Stats);

		AssetsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> ByClassJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCount)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("count"), Pair.Value);
		Entry->SetNumberField(TEXT("disk_size"), ClassBytes.FindRef(Pair.Key));
		ByClassJson->SetObjectField(Pair.Key, Entry);
	}

	TArray<TSharedPtr<FJsonValue>> Notes;
	if (MissingSizeTag > 0)
	{
		Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("%d asset(s) have no package file on disk (unsaved or in-memory only); their size counts as 0."),
			MissingSizeTag)));
	}
	Notes.Add(MakeShared<FJsonValueString>(
		TEXT("disk_size is the compressed editor package on disk. It is not runtime memory - a Nanite mesh or a BC7 texture expands well beyond it when loaded.")));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetNumberField(TEXT("scanned"), ScannedCount);
	Data->SetNumberField(TEXT("returned"), AssetsJson.Num());
	Data->SetNumberField(TEXT("total_disk_size"), TotalDiskSize);
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	Data->SetStringField(TEXT("scope_path"), ScopePath);
	Data->SetArrayField(TEXT("assets"), AssetsJson);
	Data->SetObjectField(TEXT("by_class"), ByClassJson);
	Data->SetArrayField(TEXT("notes"), Notes);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// content.size_map —— 一个资产连同依赖一共有多大
// ============================================================================
//
// 编辑器里 Window → Developer Tools → Size Map 的等价物。
//
// `content.describe` 给的是**一层**直接依赖，回答不了「这个蓝图拖了多少东西
// 进来」—— 真正吃地方的常常在第三层：蓝图引材质、材质引贴图、贴图是 8K。
// 这里做广度优先展开，去重后把包体加起来，并列出贡献最大的那几个。
//
// 上限是节点数不是层数：一个引了三千个资产的关卡，限层数会在第一层就爆掉，
// 限节点数至少能给出「已经数了五千个，还没数完」这种可解释的结果。

void FUAL_ContentBrowserCommands::Handle_SizeMap(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString AssetPath;
	if (!Payload->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}

	int32 MaxNodes = 5000;
	Payload->TryGetNumberField(TEXT("max_nodes"), MaxNodes);
	MaxNodes = FMath::Clamp(MaxNodes, 50, 20000);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}

	// 传进来的可能是对象路径（/Game/A/B.B），也可能已经是包名（/Game/A/B）
	const FName RootPackage = FName(*FPackageName::ObjectPathToPackageName(AssetPath));

	if (UAL_PackageFileSize(RootPackage.ToString()) == 0)
	{
		// 包名换算不出磁盘文件，多半是路径写错了。
		// 早点说，比返回一份「总共 0 字节」的报告好 —— 后者看起来像个结论。
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Package not found on disk: %s"), *RootPackage.ToString()));
		return;
	}

	TSet<FName> Visited;
	TArray<FName> Frontier;
	Visited.Add(RootPackage);
	Frontier.Add(RootPackage);

	struct FSizeEntry
	{
		FString Package;
		FString Class;
		int64 Size = 0;
	};
	TArray<FSizeEntry> Entries;

	int64 TotalSize = 0;
	bool bTruncated = false;

	while (Frontier.Num() > 0)
	{
		const FName Current = Frontier.Pop();

		FSizeEntry Entry;
		Entry.Package = Current.ToString();
		Entry.Size = UAL_PackageFileSize(Entry.Package);

		TArray<FAssetData> InPackage;
		AssetRegistry.GetAssetsByPackageName(Current, InPackage, true);
		if (InPackage.Num() > 0)
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Entry.Class = InPackage[0].AssetClassPath.GetAssetName().ToString();
#else
			Entry.Class = InPackage[0].AssetClass.ToString();
#endif
		}

		TotalSize += Entry.Size;
		Entries.Add(MoveTemp(Entry));

		if (Visited.Num() >= MaxNodes)
		{
			bTruncated = true;
			break;
		}

		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(Current, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dependency : Dependencies)
		{
			// 引擎和脚本包不算进用户工程的体积 —— 用户既动不了也不该为它们操心
			const FString Name = Dependency.ToString();
			if (Name.StartsWith(TEXT("/Script/")) || Name.StartsWith(TEXT("/Engine/")))
			{
				continue;
			}
			if (Visited.Contains(Dependency))
			{
				continue;
			}
			Visited.Add(Dependency);
			Frontier.Add(Dependency);
		}
	}

	Entries.Sort([](const FSizeEntry& A, const FSizeEntry& B) { return A.Size > B.Size; });

	TArray<TSharedPtr<FJsonValue>> TopJson;
	const int32 TopCount = FMath::Min(Entries.Num(), 15);
	for (int32 Index = 0; Index < TopCount; ++Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Entries[Index].Package);
		Obj->SetStringField(TEXT("class"), Entries[Index].Class);
		Obj->SetNumberField(TEXT("size"), Entries[Index].Size);
		TopJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("root"), RootPackage.ToString());
	Data->SetNumberField(TEXT("total_size"), TotalSize);
	Data->SetNumberField(TEXT("dependency_count"), FMath::Max(0, Entries.Num() - 1));
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	Data->SetArrayField(TEXT("top_contributors"), TopJson);
	Data->SetStringField(TEXT("note"),
		TEXT("Sizes are compressed editor packages on disk, engine and /Script dependencies excluded."));

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
