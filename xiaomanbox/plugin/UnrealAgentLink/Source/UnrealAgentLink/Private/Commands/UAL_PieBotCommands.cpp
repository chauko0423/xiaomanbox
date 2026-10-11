// Copyright uebox.ai

#include "UAL_PieBotCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_EditorCommands.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Brush.h"
#include "Engine/Light.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Info.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/LevelScriptActor.h"
#include "Components/PrimitiveComponent.h"
#include "NavigationData.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UnrealType.h"
#include "GameFramework/PlayerState.h"
#include "Components/ContentWidget.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/UObjectIterator.h"
#include "Containers/Ticker.h"

namespace
{
	/** 一次最多回多少个按钮。一个背包界面能有几百格，全回会把盒子那侧的决策点撑爆 */
	constexpr int32 UAL_BotMaxButtons = 48;

	/** 一次最多回多少行增量日志 */
	constexpr int32 UAL_BotDefaultMaxLogs = 200;

	int32 ReadPlayerIndex(const TSharedPtr<FJsonObject>& Payload)
	{
		double Raw = 0.0;
		if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("player_index"), Raw))
		{
			return FMath::Max(0, static_cast<int32>(Raw));
		}
		return 0;
	}

	/**
	 * 拿到正在跑的游戏世界和它的玩家控制器。游戏没跑就当场回 409。
	 *
	 * 不退回编辑器世界：理由见头文件。
	 */
	bool ResolvePlayScene(const TSharedPtr<FJsonObject>& Payload, const FString& RequestId, UWorld*& OutWorld, APlayerController*& OutPC)
	{
		OutWorld = nullptr;
		OutPC = nullptr;
		if (!UAL_CommandUtils::IsPlayInProgress())
		{
			UAL_CommandUtils::SendError(RequestId, 409,
				UAL_CommandUtils::LStr(
					TEXT("游戏没在运行。这条命令只作用于正在跑的游戏世界。"),
					TEXT("Play is not running. This command only works on the running game world.")));
			return false;
		}
		OutWorld = UAL_CommandUtils::GetLiveWorld(ReadPlayerIndex(Payload));
		OutPC = OutWorld ? OutWorld->GetFirstPlayerController() : nullptr;
		if (!OutWorld)
		{
			UAL_CommandUtils::SendError(RequestId, 409, TEXT("Play is running but the game world could not be resolved."));
			return false;
		}
		return true;
	}

	const TCHAR* MovementModeName(const UCharacterMovementComponent* Movement)
	{
		if (!Movement)
		{
			return TEXT("unknown");
		}
		switch (Movement->MovementMode)
		{
		case MOVE_None: return TEXT("none");
		case MOVE_Walking: return TEXT("walking");
		case MOVE_NavWalking: return TEXT("nav_walking");
		case MOVE_Falling: return TEXT("falling");
		case MOVE_Swimming: return TEXT("swimming");
		case MOVE_Flying: return TEXT("flying");
		case MOVE_Custom: return TEXT("custom");
		default: return TEXT("unknown");
		}
	}

	TSharedPtr<FJsonObject> BuildPawnJson(APawn* Pawn)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Pawn->GetName());
		Json->SetStringField(TEXT("class"), Pawn->GetClass()->GetName());
		Json->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Pawn->GetActorLocation()));
		Json->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(Pawn->GetActorRotation()));
		const FVector Velocity = Pawn->GetVelocity();
		Json->SetObjectField(TEXT("velocity"), UAL_CommandUtils::MakeVectorJson(Velocity));
		Json->SetNumberField(TEXT("speed"), Velocity.Size());

		const ACharacter* Character = Cast<ACharacter>(Pawn);
		const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
		if (Movement)
		{
			Json->SetStringField(TEXT("movement_mode"), MovementModeName(Movement));
			Json->SetBoolField(TEXT("is_falling"), Movement->IsFalling());
			Json->SetBoolField(TEXT("can_jump"), Character->CanJump());
		}
		else
		{
			// 不是 Character（DefaultPawn、载具、自定义 Pawn）：没有「落地」这个概念，
			// 回 unknown 而不是 walking —— 猜一个会让盒子那侧的卡住判定用错阈值
			Json->SetStringField(TEXT("movement_mode"), Pawn->GetMovementComponent() ? TEXT("custom") : TEXT("none"));
		}
		return Json;
	}

	/**
	 * 这个控件此刻真的显示在屏幕上吗。
	 *
	 * 自己 Visible 不够：父面板收起了、外面那个 UserWidget 被藏了，它照样报 Visible。
	 * 所以一路往上走，跨过 UserWidget 的边界（WidgetTree 的 Outer 就是它的 UserWidget），
	 * 直到顶层 —— 顶层还必须真的挂在视口上。
	 */
	bool IsEffectivelyVisible(UWidget* Widget)
	{
		UWidget* Current = Widget;
		int32 Guard = 0;
		while (Current && Guard++ < 64)
		{
			if (!Current->IsVisible())
			{
				return false;
			}
			UWidget* Next = Current->GetParent();
			if (!Next)
			{
				UWidgetTree* Tree = Cast<UWidgetTree>(Current->GetOuter());
				UUserWidget* Owner = Tree ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
				if (!Owner)
				{
					// 走到顶了：顶层必须是挂在视口上的那个
					UUserWidget* AsUserWidget = Cast<UUserWidget>(Current);
					return AsUserWidget && AsUserWidget->IsInViewport();
				}
				Next = Owner;
			}
			Current = Next;
		}
		return false;
	}

	/** 按钮上写的字。按钮里一般套一个 TextBlock，但可能隔着几层面板或一个子 UserWidget */
	FString FindWidgetText(UWidget* Widget, int32 Depth)
	{
		if (!Widget || Depth > 8)
		{
			return FString();
		}
		if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			return Text->GetText().ToString();
		}
		if (const UContentWidget* Content = Cast<UContentWidget>(Widget))
		{
			return FindWidgetText(Content->GetContent(), Depth + 1);
		}
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				const FString Found = FindWidgetText(Panel->GetChildAt(Index), Depth + 1);
				if (!Found.IsEmpty())
				{
					return Found;
				}
			}
			return FString();
		}
		if (const UUserWidget* User = Cast<UUserWidget>(Widget))
		{
			return User->WidgetTree ? FindWidgetText(User->WidgetTree->RootWidget, Depth + 1) : FString();
		}
		return FString();
	}

	struct FVisibleButton
	{
		FString Id;
		FString Owner;
		FString Text;
		bool bEnabled = true;
		UButton* Button = nullptr;
	};

	/** 屏幕上此刻看得见的全部按钮。`bOutTruncated` = 超过上限被截了 */
	void CollectVisibleButtons(UWorld* World, TArray<FVisibleButton>& Out, bool& bOutTruncated)
	{
		bOutTruncated = false;
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Owner = *It;
			if (!IsValid(Owner) || Owner->GetWorld() != World || !Owner->WidgetTree)
			{
				continue;
			}
			// ForEachWidget 不进子 UserWidget 的树 —— 子 UserWidget 会作为 Owner
			// 在这个外层循环里单独被遍历到，所以每个按钮只数一次
			Owner->WidgetTree->ForEachWidget([&](UWidget* Child)
			{
				UButton* Button = Cast<UButton>(Child);
				if (!Button || !IsEffectivelyVisible(Button))
				{
					return;
				}
				if (Out.Num() >= UAL_BotMaxButtons)
				{
					bOutTruncated = true;
					return;
				}
				FVisibleButton& Entry = Out.AddDefaulted_GetRef();
				Entry.Owner = Owner->GetName();
				Entry.Id = Owner->GetName() + TEXT("/") + Button->GetName();
				Entry.Text = FindWidgetText(Button, 0).Left(80);
				Entry.bEnabled = Button->GetIsEnabled();
				Entry.Button = Button;
			});
		}
	}

	/**
	 * 按名字找 PIE 世界里的 Actor：标签完全相同 > 名字完全相同 > 标签或名字包含。
	 *
	 * 标签优先是因为用户和模型嘴里说的都是大纲视图里那个名字；PIE 复制出来的
	 * Actor 在编辑器构建里保留了标签。
	 */
	AActor* FindActorInPlayWorld(UWorld* World, const FString& Query, int32& OutMatchCount)
	{
		OutMatchCount = 0;
		AActor* ExactLabel = nullptr;
		AActor* ExactName = nullptr;
		AActor* Partial = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			const FString Label = Actor->GetActorLabel();
			const FString Name = Actor->GetName();
			const bool bLabel = Label.Equals(Query, ESearchCase::IgnoreCase);
			const bool bName = Name.Equals(Query, ESearchCase::IgnoreCase);
			const bool bPartial = Label.Contains(Query) || Name.Contains(Query);
			if (bLabel || bName || bPartial)
			{
				++OutMatchCount;
			}
			if (bLabel && !ExactLabel) ExactLabel = Actor;
			if (bName && !ExactName) ExactName = Actor;
			if (bPartial && !Partial) Partial = Actor;
		}
		return ExactLabel ? ExactLabel : ExactName ? ExactName : Partial;
	}

	UNavigationSystemV1* GetNavSystem(UWorld* World)
	{
		return World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	}

	// ------------------------------------------------------------------
	// 以角色为中心的感知（sensors）
	//
	// 判定模型不看图（设计稿 §2.2 那条之外，Jev 本身也只吃文本），所以「周围有什么」
	// 必须由这里量成结构化的东西交出去：八个方向撞到什么、多高、脚下有没有断崖、
	// 附近有哪些有名字的 Actor。**只给原始量**（距离、角度、高度差），
	// 转成「左前方、很近、能跳过去」这类类别是盒子那侧的事 —— 那样阈值改起来
	// 不用重编九个版本的插件。
	// ------------------------------------------------------------------

	/** 八个方向相对朝向的偏角，顺序固定：前、右前、右、右后、后、左后、左、左前 */
	constexpr float UAL_SensorAngles[8] = { 0.f, 45.f, 90.f, 135.f, 180.f, -135.f, -90.f, -45.f };
	constexpr float UAL_SensorRange = 800.f;
	constexpr float UAL_NearbyRange = 2500.f;
	constexpr int32 UAL_MaxNearby = 20;

	FString HitName(const FHitResult& Hit)
	{
		const AActor* Actor = Hit.GetActor();
		if (!Actor)
		{
			return FString();
		}
		const FString Label = Actor->GetActorLabel();
		return Label.IsEmpty() ? Actor->GetName() : Label;
	}

	/**
	 * 在某个高度朝某个方向扫一个小球，回「多远撞到什么」。
	 *
	 * 用球不用线：线太细，会从栏杆缝、桌腿之间穿过去，报一条「畅通」而角色其实过不去。
	 * 球半径取胶囊半径的一半 —— 取满了会在窄门口误报堵死。
	 */
	bool SweepAt(UWorld* World, const APawn* Pawn, const FVector& From, const FVector& Dir, float Radius, FHitResult& OutHit)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(UALBotSensor), false, Pawn);
		return World->SweepSingleByChannel(
			OutHit, From, From + Dir * UAL_SensorRange, FQuat::Identity, ECC_Pawn,
			FCollisionShape::MakeSphere(Radius), Params);
	}

	/**
	 * 不该出现在「附近有什么」里的：几何、灯、体积、控制器、系统 Actor，
	 * 以及**游戏里根本看不见**的信息类 Actor。
	 *
	 * 最后那一条是真机上补的：第三人称模板里 ParticleEventManager、WorldPartitionReplay、
	 * 关卡脚本 Actor、空的 Actor 全挤在「附近」里，还都报在世界原点 ——
	 * 判定模型每一步都得读这堆噪音，而且会被「远处 back_left 有个叫 Actor 的东西」带偏。
	 * 玩法对象总有看得见的东西（网格、文字、粒子），没有的就不是玩家能走过去互动的对象。
	 */
	bool IsBackgroundActor(AActor* Actor)
	{
		if (Actor->IsA<AStaticMeshActor>()
			|| Actor->IsA<ABrush>()
			|| Actor->IsA<AInfo>()
			|| Actor->IsA<ALight>()
			|| Actor->IsA<AController>()
			|| Actor->IsA<ANavigationData>()
			|| Actor->IsA<APlayerStart>()
			|| Actor->IsA<ALevelScriptActor>()
			|| Actor->IsHidden()
			|| UAL_CommandUtils::IsSystemActor(Actor))
		{
			return true;
		}
		if (Actor->IsA<APawn>())
		{
			return false;
		}
		bool bVisibleInGame = false;
		Actor->ForEachComponent<UPrimitiveComponent>(false, [&bVisibleInGame](UPrimitiveComponent* Primitive)
		{
			if (Primitive && Primitive->IsVisible() && !Primitive->bHiddenInGame)
			{
				bVisibleInGame = true;
			}
		});
		return !bVisibleInGame;
	}

	TSharedPtr<FJsonObject> BuildSensors(UWorld* World, APawn* Pawn, float HeadingYaw)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("heading_yaw"), HeadingYaw);

		const ACharacter* Character = Cast<ACharacter>(Pawn);
		const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.f;
		const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 40.f;
		const FVector Location = Pawn->GetActorLocation();
		const float Bottom = Location.Z - HalfHeight;

		// 能跳多高：v² / 2g。算不出来（不是 Character）就当不能跳
		float JumpHeight = 0.f;
		float StepHeight = 45.f;
		if (const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr)
		{
			const float Gravity = FMath::Abs(Movement->GetGravityZ());
			if (Gravity > KINDA_SMALL_NUMBER)
			{
				JumpHeight = FMath::Square(Movement->JumpZVelocity) / (2.f * Gravity);
			}
			StepHeight = Movement->MaxStepHeight;
		}
		Json->SetNumberField(TEXT("jump_height"), JumpHeight);
		Json->SetNumberField(TEXT("step_height"), StepHeight);
		Json->SetNumberField(TEXT("capsule_radius"), Radius);
		Json->SetNumberField(TEXT("capsule_height"), HalfHeight * 2.f);

		// 组合动作要的能力参数：助跑跳能跳多远 = 跑速 × 滞空时间，二段跳看 JumpMaxCount
		if (const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr)
		{
			Json->SetNumberField(TEXT("max_speed"), Movement->MaxWalkSpeed);
			Json->SetNumberField(TEXT("jump_z"), Movement->JumpZVelocity);
			Json->SetNumberField(TEXT("gravity"), FMath::Abs(Movement->GetGravityZ()));
			Json->SetNumberField(TEXT("air_control"), Movement->AirControl);
			Json->SetNumberField(TEXT("jump_max_count"), Character->JumpMaxCount);
		}

		const float Probe = Radius * 0.5f;
		// 三个高度：脚下能迈过去的台阶之上、跳到最高点时脚底、头顶。
		// 低处撞、跳高处不撞 = 跳得过去；两处都撞 = 墙
		const float LowZ = Bottom + StepHeight + Probe + 2.f;
		const float JumpZ = Bottom + FMath::Max(JumpHeight - 5.f, StepHeight) + Probe;
		const float HeadZ = Bottom + HalfHeight * 2.f - Probe;

		TArray<TSharedPtr<FJsonValue>> Rays;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const float Angle = UAL_SensorAngles[Index];
			const FVector Dir = FRotator(0.f, HeadingYaw + Angle, 0.f).Vector();
			TSharedPtr<FJsonObject> Ray = MakeShared<FJsonObject>();
			Ray->SetNumberField(TEXT("angle"), Angle);

			FHitResult Low;
			if (SweepAt(World, Pawn, FVector(Location.X, Location.Y, LowZ), Dir, Probe, Low))
			{
				Ray->SetNumberField(TEXT("low"), Low.Distance);
				Ray->SetStringField(TEXT("hit"), HitName(Low));
				if (const AActor* HitActor = Low.GetActor())
				{
					Ray->SetStringField(TEXT("hit_class"), HitActor->GetClass()->GetName());
				}
			}
			FHitResult Jump;
			if (JumpHeight > StepHeight && SweepAt(World, Pawn, FVector(Location.X, Location.Y, JumpZ), Dir, Probe, Jump))
			{
				Ray->SetNumberField(TEXT("jump"), Jump.Distance);
			}
			FHitResult Head;
			if (SweepAt(World, Pawn, FVector(Location.X, Location.Y, HeadZ), Dir, Probe, Head))
			{
				Ray->SetNumberField(TEXT("head"), Head.Distance);
			}

			// 往这个方向走一步之后脚下还有没有地。前方被挡住就不量 ——
			// 起点落在墙里时向下的射线会给出一个没有意义的结果
			const float Ahead = Radius * 2.f + 60.f;
			if (!Low.bBlockingHit || Low.Distance > Ahead)
			{
				const FVector Start = FVector(Location.X, Location.Y, Bottom + StepHeight) + Dir * Ahead;
				FHitResult Floor;
				FCollisionQueryParams Params(SCENE_QUERY_STAT(UALBotFloor), false, Pawn);
				if (World->LineTraceSingleByChannel(Floor, Start, Start - FVector(0.f, 0.f, 3000.f), ECC_Pawn, Params))
				{
					Ray->SetNumberField(TEXT("drop"), Bottom - Floor.ImpactPoint.Z);
				}
				else
				{
					// 3000 单位以下都没有地：悬崖或者世界边缘
					Ray->SetNumberField(TEXT("drop"), -1.0);
				}

				// 沟对面有没有落脚点：助跑跳要知道「沟多宽、对面多高」。
				// 从 2 米到 7 米每米向下探一次，第一个「不比脚下低太多、也不比跳跃高度高」的地面就是落点
				const bool bGapAhead = !Floor.bBlockingHit || (Bottom - Floor.ImpactPoint.Z) > StepHeight * 2.f;
				if (bGapAhead)
				{
					for (float D = 200.f; D <= 700.f; D += 100.f)
					{
						const FVector Probe2 = FVector(Location.X, Location.Y, Bottom + FMath::Max(JumpHeight, StepHeight)) + Dir * D;
						FHitResult Land;
						if (World->LineTraceSingleByChannel(Land, Probe2, Probe2 - FVector(0.f, 0.f, 800.f), ECC_Pawn, Params))
						{
							const float LandDz = Land.ImpactPoint.Z - Bottom;
							if (LandDz > -300.f && LandDz < JumpHeight - 20.f)
							{
								Ray->SetNumberField(TEXT("landing"), D);
								Ray->SetNumberField(TEXT("landing_dz"), LandDz);
								break;
							}
						}
					}
				}
			}

			// 前面那个障碍有多高：跳上去（比跳跃高度矮）、二段跳上去、还是根本过不去。
			// 头顶那一层也撞了就是高墙，不量 —— 从墙体内部往下打的射线会穿过它打到地板上
			if (Low.bBlockingHit && Low.Distance < 300.f && !(Head.bBlockingHit && Head.Distance < Low.Distance + 60.f))
			{
				const FVector Over = FVector(Low.ImpactPoint.X, Low.ImpactPoint.Y, Bottom + HalfHeight * 3.f) + Dir * 30.f;
				FHitResult Top;
				FCollisionQueryParams Params(SCENE_QUERY_STAT(UALBotTop), false, Pawn);
				if (World->LineTraceSingleByChannel(Top, Over, Over - FVector(0.f, 0.f, HalfHeight * 3.f + 50.f), ECC_Pawn, Params))
				{
					Ray->SetNumberField(TEXT("obstacle_top"), Top.ImpactPoint.Z - Bottom);
				}
			}
			Rays.Add(MakeShared<FJsonValueObject>(Ray));
		}
		Json->SetArrayField(TEXT("rays"), Rays);

		// 附近有名字的 Actor。只收「玩法对象」：几何、灯、体积、控制器、系统 Actor 都不要 ——
		// 一个关卡里几百块墙板会把真正要紧的门、钥匙、敌人淹掉
		struct FNearby
		{
			AActor* Actor;
			float Distance;
		};
		TArray<FNearby> Nearby;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor == Pawn || IsBackgroundActor(Actor))
			{
				continue;
			}
			const float Distance = FVector::Dist(Location, Actor->GetActorLocation());
			if (Distance <= UAL_NearbyRange)
			{
				Nearby.Add({ Actor, Distance });
			}
		}
		Nearby.Sort([](const FNearby& A, const FNearby& B) { return A.Distance < B.Distance; });

		const FVector Eye = FVector(Location.X, Location.Y, Bottom + HalfHeight * 1.7f);
		TArray<TSharedPtr<FJsonValue>> NearbyJson;
		for (int32 Index = 0; Index < Nearby.Num() && Index < UAL_MaxNearby; ++Index)
		{
			AActor* Actor = Nearby[Index].Actor;
			const FVector Target = Actor->GetActorLocation();
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			const FString Label = Actor->GetActorLabel();
			Entry->SetStringField(TEXT("name"), Label.IsEmpty() ? Actor->GetName() : Label);
			Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Entry->SetNumberField(TEXT("distance"), Nearby[Index].Distance);
			Entry->SetNumberField(TEXT("bearing"),
				FRotator::NormalizeAxis((Target - Location).Rotation().Yaw - HeadingYaw));
			Entry->SetNumberField(TEXT("dz"), Target.Z - Location.Z);
			Entry->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Target));
			if (Actor->IsA<APawn>())
			{
				Entry->SetBoolField(TEXT("is_pawn"), true);
			}
			TArray<TSharedPtr<FJsonValue>> Tags;
			for (const FName& Tag : Actor->Tags)
			{
				Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			if (Tags.Num() > 0)
			{
				Entry->SetArrayField(TEXT("tags"), Tags);
			}
			// 看不看得见：眼睛到它之间有没有别的东西挡着
			FHitResult Sight;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(UALBotSight), false, Pawn);
			const bool bBlocked = World->LineTraceSingleByChannel(Sight, Eye, Target, ECC_Visibility, Params)
				&& Sight.GetActor() != Actor;
			Entry->SetBoolField(TEXT("visible"), !bBlocked);
			NearbyJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Json->SetArrayField(TEXT("nearby"), NearbyJson);
		return Json;
	}
}

void FUAL_PieBotCommands::RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap)
{
	CommandMap.Add(TEXT("pie.observe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Observe(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.set_view"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetView(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.click_widget"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ClickWidget(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.nav_path"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_NavPath(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.scene"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Scene(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.move_to"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_MoveTo(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.move_stop"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_MoveStop(Payload, RequestId);
	});
	CommandMap.Add(TEXT("pie.plan_path"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_PlanPath(Payload, RequestId);
	});
}

void FUAL_PieBotCommands::Handle_Observe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("playing"), true);
	Result->SetNumberField(TEXT("game_time"), World->GetTimeSeconds());
	Result->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
	Result->SetBoolField(TEXT("paused"), World->IsPaused());

	double SessionElapsed = 0.0;
	bool bPlayStarted = false;
	const bool bSession = FUAL_EditorCommands::IsPieSessionActive(SessionElapsed, bPlayStarted);
	Result->SetBoolField(TEXT("session_active"), bSession);
	if (bSession)
	{
		Result->SetNumberField(TEXT("session_elapsed"), SessionElapsed);
	}

	if (const AWorldSettings* Settings = World->GetWorldSettings())
	{
		if (Settings->bEnableWorldBoundsChecks)
		{
			Result->SetNumberField(TEXT("kill_z"), Settings->KillZ);
		}
	}

	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (Pawn)
	{
		Result->SetObjectField(TEXT("pawn"), BuildPawnJson(Pawn));
	}
	else
	{
		// 没有 pawn 是一个要如实报的状态（还没 Possess、死了在等重生、工程根本没有可控角色），
		// 不是错误
		Result->SetField(TEXT("pawn"), MakeShared<FJsonValueNull>());
	}
	Result->SetBoolField(TEXT("has_controller"), PC != nullptr);

	if (PC)
	{
		Result->SetObjectField(TEXT("control_rotation"), UAL_CommandUtils::MakeRotatorJson(PC->GetControlRotation()));

		// 输入模式。**注入成功 ≠ 玩家按得动**（设计 §12.8）：视口忽略输入时，动作注入照样生效
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		UGameViewportClient* Viewport = World->GetGameViewport();
		Input->SetBoolField(TEXT("viewport_ignores_input"), Viewport && Viewport->IgnoreInput());
		Input->SetBoolField(TEXT("move_input_ignored"), PC->IsMoveInputIgnored());
		Input->SetBoolField(TEXT("look_input_ignored"), PC->IsLookInputIgnored());
		Input->SetBoolField(TEXT("show_mouse_cursor"), PC->bShowMouseCursor);
		Input->SetBoolField(TEXT("cinematic_mode"), PC->bCinematicMode);
		Result->SetObjectField(TEXT("input"), Input);
	}

	// 连续移动的状态：盒子轮询 observe 时顺便拿到，不用另开一条命令
	Result->SetObjectField(TEXT("move"), BuildMoveStatus());

	bool bSensors = false;
	if (Payload.IsValid() && Payload->TryGetBoolField(TEXT("sensors"), bSensors) && bSensors && Pawn)
	{
		// 朝向以调用方为准：机器人按「自己要走的方向」理解前后左右，
		// 那不一定是此刻的控制器朝向（它每一步都会重新转）
		double Heading = PC ? PC->GetControlRotation().Yaw : Pawn->GetActorRotation().Yaw;
		Payload->TryGetNumberField(TEXT("heading_yaw"), Heading);
		Result->SetObjectField(TEXT("sensors"), BuildSensors(World, Pawn, static_cast<float>(Heading)));
	}

	bool bIncludeWidgets = true;
	if (Payload.IsValid())
	{
		Payload->TryGetBoolField(TEXT("include_widgets"), bIncludeWidgets);
	}
	if (bIncludeWidgets)
	{
		TArray<FVisibleButton> Buttons;
		bool bTruncated = false;
		CollectVisibleButtons(World, Buttons, bTruncated);
		TArray<TSharedPtr<FJsonValue>> ButtonJson;
		for (const FVisibleButton& Button : Buttons)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("id"), Button.Id);
			Entry->SetStringField(TEXT("owner"), Button.Owner);
			Entry->SetStringField(TEXT("text"), Button.Text);
			Entry->SetBoolField(TEXT("enabled"), Button.bEnabled);
			ButtonJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("buttons"), ButtonJson);
		if (bTruncated)
		{
			Result->SetBoolField(TEXT("buttons_truncated"), true);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TargetArray = nullptr;
	if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("targets"), TargetArray) && TargetArray)
	{
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (const TSharedPtr<FJsonValue>& Value : *TargetArray)
		{
			FString Query;
			if (!Value.IsValid() || !Value->TryGetString(Query) || Query.IsEmpty())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("query"), Query);
			int32 Matches = 0;
			AActor* Actor = FindActorInPlayWorld(World, Query, Matches);
			Entry->SetBoolField(TEXT("found"), Actor != nullptr);
			if (Actor)
			{
				Entry->SetStringField(TEXT("name"), Actor->GetName());
				Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				Entry->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Actor->GetActorLocation()));
				Entry->SetNumberField(TEXT("match_count"), Matches);
				if (Pawn)
				{
					Entry->SetNumberField(TEXT("distance"), FVector::Dist(Pawn->GetActorLocation(), Actor->GetActorLocation()));
				}
			}
			Targets.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("targets"), Targets);
	}

	// 增量日志：只有 pie.run 起的会话才有捕获器
	double LogSinceRaw = -1.0;
	if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("log_since"), LogSinceRaw) && LogSinceRaw >= 0.0)
	{
		double MaxLogsRaw = UAL_BotDefaultMaxLogs;
		Payload->TryGetNumberField(TEXT("max_logs"), MaxLogsRaw);
		TArray<FUAL_EditorCommands::FPieLogLine> Lines;
		int32 NextSeq = 0;
		bool bDropped = false;
		if (FUAL_EditorCommands::ReadPieSessionLogs(
				static_cast<int32>(LogSinceRaw), FMath::Clamp(static_cast<int32>(MaxLogsRaw), 1, 1000), Lines, NextSeq, bDropped))
		{
			TArray<TSharedPtr<FJsonValue>> LogEntries;
			for (const FUAL_EditorCommands::FPieLogLine& Line : Lines)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("seq"), Line.Seq);
				Entry->SetNumberField(TEXT("at"), FMath::RoundToDouble(Line.At * 100.0) / 100.0);
				Entry->SetStringField(TEXT("kind"), Line.Kind);
				Entry->SetStringField(TEXT("text"), Line.Text);
				LogEntries.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("logs"), LogEntries);
			Result->SetNumberField(TEXT("log_cursor"), NextSeq);
			if (bDropped)
			{
				Result->SetBoolField(TEXT("logs_dropped"), true);
			}
		}
	}

	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_PieBotCommands::Handle_SetView(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}
	if (!PC)
	{
		UAL_CommandUtils::SendError(RequestId, 409, TEXT("No PlayerController in the game world yet."));
		return;
	}

	FRotator Rotation = PC->GetControlRotation();
	double Yaw = 0.0;
	if (!Payload.IsValid() || !Payload->TryGetNumberField(TEXT("yaw"), Yaw))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("yaw is required (degrees)."));
		return;
	}
	Rotation.Yaw = static_cast<float>(Yaw);
	double Pitch = 0.0;
	if (Payload->TryGetNumberField(TEXT("pitch"), Pitch))
	{
		Rotation.Pitch = static_cast<float>(FMath::Clamp(Pitch, -89.0, 89.0));
	}
	Rotation.Roll = 0.0f;
	PC->SetControlRotation(Rotation);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetObjectField(TEXT("control_rotation"), UAL_CommandUtils::MakeRotatorJson(PC->GetControlRotation()));
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_PieBotCommands::Handle_ClickWidget(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}

	FString Id;
	if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("id is required (from pie.observe buttons[].id)."));
		return;
	}

	TArray<FVisibleButton> Buttons;
	bool bTruncated = false;
	CollectVisibleButtons(World, Buttons, bTruncated);
	const FVisibleButton* Found = Buttons.FindByPredicate([&Id](const FVisibleButton& B) { return B.Id == Id; });
	if (!Found || !Found->Button)
	{
		// 按钮在观察和点击之间消失了（界面切走了），这是游戏状态变了，不是参数错
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Button \"%s\" is not visible on screen any more."), *Id));
		return;
	}
	if (!Found->bEnabled)
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("Button \"%s\" is disabled; a player could not click it either."), *Id));
		return;
	}

	Found->Button->OnClicked.Broadcast();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("id"), Found->Id);
	Result->SetStringField(TEXT("text"), Found->Text);
	Result->SetStringField(TEXT("layer_note"), UAL_CommandUtils::LStr(
		TEXT("直接触发了按钮的 OnClicked，没有走屏幕命中测试 —— 证明的是点了之后的逻辑，不是玩家点得到。"),
		TEXT("Broadcast OnClicked directly without Slate hit-testing: this proves what the click does, not that a player can reach it.")));
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_PieBotCommands::Handle_NavPath(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}
	if (!Payload.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Pass either to:{x,y,z} or random_radius."));
		return;
	}
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);

	UNavigationSystemV1* NavSys = GetNavSystem(World);
	const bool bHasNav = NavSys && NavSys->GetDefaultNavDataInstance() != nullptr;
	Result->SetBoolField(TEXT("has_navmesh"), bHasNav);
	if (!bHasNav)
	{
		// 没有导航网格不是错误：很多小关卡就是没铺。调用方据此退回直线走
		UAL_CommandUtils::AddWorldInfo(Result);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	FVector From = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	TSharedPtr<FJsonObject> FromObj;
	if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("from"), FromObj))
	{
		From = UAL_CommandUtils::ReadVectorDirect(FromObj, From);
	}

	double RandomRadius = 0.0;
	if (Payload->TryGetNumberField(TEXT("random_radius"), RandomRadius) && RandomRadius > 0.0)
	{
		FVector Point = FVector::ZeroVector;
		const bool bFound = UNavigationSystemV1::K2_GetRandomReachablePointInRadius(
			World, From, Point, static_cast<float>(RandomRadius));
		Result->SetBoolField(TEXT("found"), bFound);
		if (bFound)
		{
			Result->SetObjectField(TEXT("point"), UAL_CommandUtils::MakeVectorJson(Point));
		}
		UAL_CommandUtils::AddWorldInfo(Result);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	TSharedPtr<FJsonObject> ToObj;
	if (!UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("to"), ToObj))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Pass either to:{x,y,z} or random_radius."));
		return;
	}
	const FVector To = UAL_CommandUtils::ReadVectorDirect(ToObj, FVector::ZeroVector);

	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, From, To, Pawn);
	const bool bValid = Path && Path->IsValid();
	Result->SetBoolField(TEXT("found"), bValid);
	if (bValid)
	{
		TArray<TSharedPtr<FJsonValue>> Points;
		for (const FVector& Point : Path->PathPoints)
		{
			Points.Add(MakeShared<FJsonValueObject>(UAL_CommandUtils::MakeVectorJson(Point)));
		}
		Result->SetArrayField(TEXT("points"), Points);
		Result->SetBoolField(TEXT("is_partial"), Path->IsPartial());
		Result->SetNumberField(TEXT("length"), Path->GetPathLength());
	}
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// pie.scene —— 整个游戏世界的「玩法对象」清单
// ============================================================================

namespace
{
	constexpr int32 UAL_SceneMaxActors = 60;
	constexpr int32 UAL_SceneMaxNavQueries = 40;
	constexpr int32 UAL_SceneMaxVars = 12;
	constexpr int32 UAL_SceneMaxEvents = 12;
	constexpr int32 UAL_SceneMaxComponents = 10;

	bool IsBlueprintClass(const UClass* Class)
	{
		return Class && Cast<const UBlueprintGeneratedClass>(Class) != nullptr;
	}

	/** 蓝图变量是什么引擎内部槽位（事件图的帧、默认根组件），不是玩法状态 */
	bool IsPlumbingVar(const FString& Name)
	{
		return Name.StartsWith(TEXT("UberGraphFrame")) || Name == TEXT("DefaultSceneRoot");
	}

	/**
	 * 读蓝图里定义的变量（只读蓝图层，不读 C++ 基类那几百个属性）。
	 *
	 * 这是「门锁没锁」「身上有没有钥匙」最直接的来源 —— 很多游戏根本不打 PrintString，
	 * 状态只在变量里。组件引用跳过（那是结构不是状态），对象引用只报名字，数组只报长度。
	 */
	TSharedPtr<FJsonObject> ReadBlueprintVars(UObject* Object)
	{
		TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
		if (!Object || !IsBlueprintClass(Object->GetClass()))
		{
			return Vars;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It && Count < UAL_SceneMaxVars; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop || !IsBlueprintClass(Prop->GetOwnerClass()) || IsPlumbingVar(Prop->GetName()))
			{
				continue;
			}
			const void* Value = Prop->ContainerPtrToValuePtr<void>(Object);
			if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
			{
				const UObject* Target = ObjectProp->GetObjectPropertyValue(Value);
				if (Target && Target->IsA<UActorComponent>())
				{
					continue;
				}
				const AActor* TargetActor = Cast<AActor>(Target);
				Vars->SetStringField(Prop->GetName(),
					TargetActor ? TargetActor->GetActorLabel() : Target ? Target->GetName() : TEXT("None"));
			}
			else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				FScriptArrayHelper Helper(ArrayProp, Value);
				Vars->SetStringField(Prop->GetName(), FString::Printf(TEXT("array(%d)"), Helper.Num()));
			}
			else if (CastField<FStructProperty>(Prop) || CastField<FMapProperty>(Prop) || CastField<FSetProperty>(Prop))
			{
				continue;
			}
			else
			{
				TSharedPtr<FJsonValue> Json = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, Value);
				if (!Json.IsValid())
				{
					continue;
				}
				Vars->SetField(Prop->GetName(), Json);
			}
			++Count;
		}
		return Vars;
	}

	/** 这个 Actor 会不会在玩家走进去时有反应：有碰撞为「重叠」的组件 */
	bool IsTrigger(const AActor* Actor)
	{
		bool bTrigger = false;
		Actor->ForEachComponent<UPrimitiveComponent>(false, [&bTrigger](const UPrimitiveComponent* Primitive)
		{
			if (Primitive && Primitive->GetGenerateOverlapEvents() && Primitive->IsQueryCollisionEnabled()
				&& Primitive->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Overlap)
			{
				bTrigger = true;
			}
		});
		return bTrigger;
	}

	/**
	 * 整关范围内值得告诉决策者的对象。
	 *
	 * 比「附近」那份宽：看不见的触发体积要（走进去就有事发生）、会动的静态网格要
	 * （关卡蓝图常把门做成一块会动的网格）；纯几何、灯、信息类 Actor 照样不要。
	 */
	bool IsSceneRelevant(AActor* Actor, const APawn* Self)
	{
		if (!IsValid(Actor) || Actor == Self)
		{
			return false;
		}
		if (Actor->IsA<AInfo>() || Actor->IsA<ALight>() || Actor->IsA<AController>()
			|| Actor->IsA<ANavigationData>() || Actor->IsA<APlayerStart>() || Actor->IsA<ALevelScriptActor>()
			|| UAL_CommandUtils::IsSystemActor(Actor))
		{
			return false;
		}
		if (Actor->IsA<APawn>())
		{
			return true;
		}
		if (IsTrigger(Actor))
		{
			return true;
		}
		if (Actor->IsA<ABrush>() || Actor->IsHidden())
		{
			return false;
		}
		if (Actor->IsA<AStaticMeshActor>())
		{
			const USceneComponent* Root = Actor->GetRootComponent();
			return Root && Root->Mobility == EComponentMobility::Movable;
		}
		return !IsBackgroundActor(Actor) || IsBlueprintClass(Actor->GetClass());
	}

	TSharedPtr<FJsonObject> DescribeSceneActor(AActor* Actor, const FVector& From, float HeadingYaw)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		const FString Label = Actor->GetActorLabel();
		Json->SetStringField(TEXT("name"), Label.IsEmpty() ? Actor->GetName() : Label);
		Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

		// 类链：BP_LockedDoor_C → BP_Door_C → Actor。父类名常常比子类名更说明它是什么
		TArray<TSharedPtr<FJsonValue>> Chain;
		for (UClass* Class = Actor->GetClass()->GetSuperClass(); Class && Chain.Num() < 4; Class = Class->GetSuperClass())
		{
			Chain.Add(MakeShared<FJsonValueString>(Class->GetName()));
			if (!IsBlueprintClass(Class))
			{
				break;
			}
		}
		Json->SetArrayField(TEXT("parents"), Chain);

		FVector Origin;
		FVector Extent;
		Actor->GetActorBounds(false, Origin, Extent);
		const FVector Location = Actor->GetActorLocation();
		Json->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Location));
		Json->SetObjectField(TEXT("extent"), UAL_CommandUtils::MakeVectorJson(Extent));
		Json->SetNumberField(TEXT("distance"), FVector::Dist(From, Location));
		Json->SetNumberField(TEXT("bearing"), FRotator::NormalizeAxis((Location - From).Rotation().Yaw - HeadingYaw));
		Json->SetNumberField(TEXT("dz"), Location.Z - From.Z);
		if (Actor->IsA<APawn>())
		{
			Json->SetBoolField(TEXT("is_pawn"), true);
		}
		if (IsTrigger(Actor))
		{
			Json->SetBoolField(TEXT("trigger"), true);
		}

		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& Tag : Actor->Tags)
		{
			Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		if (Tags.Num() > 0)
		{
			Json->SetArrayField(TEXT("tags"), Tags);
		}

		// 组件：「类型:名字」—— 蓝图里起的组件名（InteractionBox、KeyMesh）是很强的语义
		TArray<TSharedPtr<FJsonValue>> Components;
		TArray<UActorComponent*> Owned;
		Actor->GetComponents(Owned);
		for (UActorComponent* Component : Owned)
		{
			if (!Component || Components.Num() >= UAL_SceneMaxComponents)
			{
				continue;
			}
			const FString Type = Component->GetClass()->GetName();
			if (Type == TEXT("SceneComponent") || Type == TEXT("BillboardComponent") || Type == TEXT("ArrowComponent"))
			{
				continue;
			}
			Components.Add(MakeShared<FJsonValueString>(Type + TEXT(":") + Component->GetName()));
		}
		if (Components.Num() > 0)
		{
			Json->SetArrayField(TEXT("components"), Components);
		}

		// 接口和蓝图里写了的事件 / 函数：BPI_Interactable、Interact、OpenDoor 这些名字
		// 直接说明「它能被怎么用」
		TArray<TSharedPtr<FJsonValue>> Interfaces;
		TArray<TSharedPtr<FJsonValue>> Events;
		bool bOnOverlap = false;
		bool bOnHit = false;
		for (UClass* Class = Actor->GetClass(); Class && IsBlueprintClass(Class); Class = Class->GetSuperClass())
		{
			for (const FImplementedInterface& Interface : Class->Interfaces)
			{
				if (Interface.Class)
				{
					Interfaces.AddUnique(MakeShared<FJsonValueString>(Interface.Class->GetName()));
				}
			}
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				const FString Name = It->GetName();
				if (Name == TEXT("ReceiveActorBeginOverlap"))
				{
					bOnOverlap = true;
				}
				if (Name == TEXT("ReceiveHit"))
				{
					bOnHit = true;
				}
				if (Name.StartsWith(TEXT("ExecuteUbergraph")) || Name == TEXT("UserConstructionScript")
					|| Name.StartsWith(TEXT("Receive")) || Name.StartsWith(TEXT("BndEvt__"))
					|| Name.StartsWith(TEXT("InpActEvt_")) || Name.StartsWith(TEXT("InpAxisEvt_")))
				{
					continue;
				}
				if (Events.Num() < UAL_SceneMaxEvents)
				{
					Events.Add(MakeShared<FJsonValueString>(Name));
				}
			}
		}
		if (Interfaces.Num() > 0)
		{
			Json->SetArrayField(TEXT("interfaces"), Interfaces);
		}
		if (Events.Num() > 0)
		{
			Json->SetArrayField(TEXT("events"), Events);
		}
		if (bOnOverlap)
		{
			Json->SetBoolField(TEXT("reacts_to_overlap"), true);
		}
		if (bOnHit)
		{
			Json->SetBoolField(TEXT("reacts_to_hit"), true);
		}

		TSharedPtr<FJsonObject> Vars = ReadBlueprintVars(Actor);
		if (Vars->Values.Num() > 0)
		{
			Json->SetObjectField(TEXT("vars"), Vars);
		}
		return Json;
	}

	/**
	 * 从玩家走过去走不走得到。目标位置常常在网格里面（门、箱子），先投到导航网格上再寻路。
	 */
	void AddReachability(UWorld* World, APawn* Pawn, AActor* Actor, const TSharedPtr<FJsonObject>& Json)
	{
		UNavigationSystemV1* NavSys = GetNavSystem(World);
		if (!NavSys || !NavSys->GetDefaultNavDataInstance())
		{
			return;
		}
		FNavLocation Projected;
		const bool bOnNav = NavSys->ProjectPointToNavigation(
			Actor->GetActorLocation(), Projected, FVector(250.f, 250.f, 400.f), static_cast<const ANavigationData*>(nullptr));
		if (!bOnNav)
		{
			Json->SetStringField(TEXT("nav"), TEXT("off_navmesh"));
			return;
		}
		UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
			World, Pawn->GetActorLocation(), Projected.Location, Pawn);
		if (!Path || !Path->IsValid())
		{
			Json->SetStringField(TEXT("nav"), TEXT("unreachable"));
			return;
		}
		Json->SetStringField(TEXT("nav"), Path->IsPartial() ? TEXT("partial") : TEXT("reachable"));
		Json->SetNumberField(TEXT("path_length"), Path->GetPathLength());
	}
}

void FUAL_PieBotCommands::Handle_Scene(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const FVector From = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	double Heading = PC ? PC->GetControlRotation().Yaw : 0.0;
	double MaxActorsRaw = UAL_SceneMaxActors;
	if (Payload.IsValid())
	{
		Payload->TryGetNumberField(TEXT("heading_yaw"), Heading);
		Payload->TryGetNumberField(TEXT("max_actors"), MaxActorsRaw);
	}
	const int32 MaxActors = FMath::Clamp(static_cast<int32>(MaxActorsRaw), 1, 200);

	TArray<AActor*> Relevant;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IsSceneRelevant(*It, Pawn))
		{
			Relevant.Add(*It);
		}
	}
	Relevant.Sort([&From](const AActor& A, const AActor& B)
	{
		return FVector::DistSquared(From, A.GetActorLocation()) < FVector::DistSquared(From, B.GetActorLocation());
	});

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("level"), World->GetMapName());
	UNavigationSystemV1* NavSys = GetNavSystem(World);
	Result->SetBoolField(TEXT("has_navmesh"), NavSys && NavSys->GetDefaultNavDataInstance());
	Result->SetNumberField(TEXT("heading_yaw"), Heading);
	Result->SetNumberField(TEXT("total_relevant"), Relevant.Num());

	// 玩家自己身上的状态：背包、血量、「拿到钥匙了没」多半在角色、控制器或 PlayerState 的变量里
	TSharedPtr<FJsonObject> Player = MakeShared<FJsonObject>();
	if (Pawn)
	{
		Player->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(From));
		Player->SetObjectField(TEXT("pawn_vars"), ReadBlueprintVars(Pawn));
	}
	if (PC)
	{
		Player->SetObjectField(TEXT("controller_vars"), ReadBlueprintVars(PC));
		if (PC->PlayerState)
		{
			Player->SetObjectField(TEXT("state_vars"), ReadBlueprintVars(PC->PlayerState));
		}
	}
	Result->SetObjectField(TEXT("player"), Player);

	TArray<TSharedPtr<FJsonValue>> Actors;
	for (int32 Index = 0; Index < Relevant.Num() && Index < MaxActors; ++Index)
	{
		TSharedPtr<FJsonObject> Json = DescribeSceneActor(Relevant[Index], From, static_cast<float>(Heading));
		if (Pawn && Index < UAL_SceneMaxNavQueries)
		{
			AddReachability(World, Pawn, Relevant[Index], Json);
		}
		Actors.Add(MakeShared<FJsonValueObject>(Json));
	}
	Result->SetArrayField(TEXT("actors"), Actors);
	if (Relevant.Num() > MaxActors)
	{
		Result->SetBoolField(TEXT("truncated"), true);
	}
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// pie.move_to / pie.move_stop —— 连续移动（逐帧在游戏线程上执行）
// ============================================================================
//
// 为什么放在插件里：盒子那边每走一米就要一次网络往返（看落差 → 决定 → 再按一段），
// 两段之间角色会刹车、视角一跳一跳地转，看着像在蠕动。这里一次收下一串路径点，
// 每一帧平滑转向、持续给移动输入、边走边看前方：
//   - 前方是矮障碍：跑动中起跳（能二段跳就补第二跳）
//   - 前方是沟、对面有落脚点、允许跨沟：冲到边缘起跳
//   - 前方是悬崖且不允许跳：当场停下，报 edge
//   - 两秒没更靠近终点：报 blocked
// 盒子只在这些「事件」上做决策（交给判定模型），过程是连续的。
//
// 代价要说清：这里用的是 Pawn::AddMovementInput / Character::Jump，
// 比动作层注入还底层 —— 连游戏自己处理移动输入的那段蓝图也绕过了。
// 它证明的是「这条路走得通」，不是「玩家按键走得通」。

namespace
{
	struct FUAL_MoveJob
	{
		bool bActive = false;
		TArray<FVector> Points;
		int32 Index = 0;
		float AcceptRadius = 90.f;
		float FinalRadius = 150.f;
		bool bAutoJump = true;
		bool bJumpGaps = false;
		bool bStopAtEdge = true;
		float EdgeDrop = 400.f;
		float MaxSeconds = 20.f;
		float TurnRate = 540.f;

		FString Status = TEXT("idle");
		FString Detail;
		double StartedAt = 0.0;
		double LastProgressAt = 0.0;
		double LastJumpAt = -10.0;
		double PendingSecondJumpAt = -1.0;
		float BestDistance = TNumericLimits<float>::Max();
		float Travelled = 0.f;
		int32 Jumps = 0;
		FVector LastLocation = FVector::ZeroVector;
		FTSTicker::FDelegateHandle Ticker;
		/** 最近发生的事（起跳、量到的障碍高度、被挡的位置），给盒子那边诊断用 */
		TArray<FString> Events;
		double LastTallNoteAt = -10.0;

		void Note(const FString& Event)
		{
			if (Events.Num() >= 16)
			{
				Events.RemoveAt(0);
			}
			Events.Add(Event);
		}
	};

	FUAL_MoveJob GMoveJob;

	void FinishMove(const TCHAR* Status, const FString& Detail)
	{
		GMoveJob.bActive = false;
		GMoveJob.Status = Status;
		GMoveJob.Detail = Detail;
		if (GMoveJob.Ticker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GMoveJob.Ticker);
			GMoveJob.Ticker.Reset();
		}
	}

	/** 从脚底高度往下看 Ahead 厘米外有没有地；回落差（厘米），-1 = 没有地 */
	float ProbeDrop(UWorld* World, const APawn* Pawn, const FVector& Foot, const FVector& Dir, float Ahead, float StepHeight)
	{
		const FVector Start = Foot + FVector(0.f, 0.f, StepHeight) + Dir * Ahead;
		FHitResult Floor;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(UALMoveFloor), false, Pawn);
		if (World->LineTraceSingleByChannel(Floor, Start, Start - FVector(0.f, 0.f, 3000.f), ECC_Pawn, Params))
		{
			return Foot.Z - Floor.ImpactPoint.Z;
		}
		return -1.f;
	}

	bool TickMove(float Delta)
	{
		if (!GMoveJob.bActive)
		{
			return false;
		}
		if (!UAL_CommandUtils::IsPlayInProgress())
		{
			FinishMove(TEXT("cancelled"), TEXT("play ended"));
			return false;
		}
		UWorld* World = UAL_CommandUtils::GetLiveWorld(0);
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			FinishMove(TEXT("no_pawn"), TEXT("the player pawn is gone"));
			return false;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - GMoveJob.StartedAt > GMoveJob.MaxSeconds)
		{
			FinishMove(TEXT("timeout"), TEXT("ran out of time"));
			return false;
		}

		ACharacter* Character = Cast<ACharacter>(Pawn);
		const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
		const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.f;
		const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 40.f;
		const float StepHeight = Movement ? Movement->MaxStepHeight : 45.f;
		float JumpHeight = 0.f;
		if (Movement && FMath::Abs(Movement->GetGravityZ()) > KINDA_SMALL_NUMBER)
		{
			JumpHeight = FMath::Square(Movement->JumpZVelocity) / (2.f * FMath::Abs(Movement->GetGravityZ()));
		}
		const bool bFalling = Movement && Movement->IsFalling();

		const FVector Location = Pawn->GetActorLocation();
		GMoveJob.Travelled += FVector::Dist2D(Location, GMoveJob.LastLocation);
		GMoveJob.LastLocation = Location;
		const FVector Foot(Location.X, Location.Y, Location.Z - HalfHeight);

		// 到了当前路径点就换下一个
		while (GMoveJob.Index < GMoveJob.Points.Num())
		{
			const bool bLast = GMoveJob.Index == GMoveJob.Points.Num() - 1;
			const float Radius2 = bLast ? GMoveJob.FinalRadius : GMoveJob.AcceptRadius;
			if (FVector::Dist2D(Location, GMoveJob.Points[GMoveJob.Index]) > Radius2)
			{
				break;
			}
			++GMoveJob.Index;
		}
		if (GMoveJob.Index >= GMoveJob.Points.Num())
		{
			FinishMove(TEXT("reached"), FString());
			return false;
		}

		// 进展：离终点的最近距离两秒没刷新就是被挡住了
		const float ToFinal = FVector::Dist2D(Location, GMoveJob.Points.Last());
		if (ToFinal < GMoveJob.BestDistance - 30.f)
		{
			GMoveJob.BestDistance = ToFinal;
			GMoveJob.LastProgressAt = Now;
		}
		else if (Now - GMoveJob.LastProgressAt > 2.0 && !bFalling)
		{
			GMoveJob.Note(FString::Printf(TEXT("blocked at (%.0f, %.0f, %.0f)"), Location.X, Location.Y, Location.Z));
			FinishMove(TEXT("blocked"), TEXT("no progress toward the destination for 2 seconds"));
			return false;
		}

		FVector Dir = GMoveJob.Points[GMoveJob.Index] - Location;
		Dir.Z = 0.f;
		if (!Dir.Normalize())
		{
			return true;
		}

		// 视角平滑转过去（每秒最多 TurnRate 度），不是一下拧到位
		const FRotator Control = PC->GetControlRotation();
		const float NewYaw = FMath::FixedTurn(Control.Yaw, Dir.Rotation().Yaw, GMoveJob.TurnRate * Delta);
		PC->SetControlRotation(FRotator(Control.Pitch, NewYaw, 0.f));

		// 二段跳的第二下
		if (GMoveJob.PendingSecondJumpAt > 0.0 && Now >= GMoveJob.PendingSecondJumpAt && Character)
		{
			Character->Jump();
			++GMoveJob.Jumps;
			GMoveJob.PendingSecondJumpAt = -1.0;
		}

		if (!bFalling)
		{
			// 落地了就松开跳跃键。不松的话 bPressedJump 一直挂着，引擎不会把跳跃次数清零，
			// 下一级台阶就跳不起来了 —— 真机上「规划出要跳三次的路，走不到一米就卡住」就是这个
			if (Character && Now - GMoveJob.LastJumpAt > 0.2)
			{
				Character->StopJumping();
			}

			// 前方落差
			const float Drop = ProbeDrop(World, Pawn, Foot, Dir, Radius + 70.f, StepHeight);
			const bool bEdge = Drop < 0.f || Drop > GMoveJob.EdgeDrop;
			if (bEdge)
			{
				bool bCanClear = false;
				if (GMoveJob.bJumpGaps && Character && Character->CanJump() && Movement)
				{
					// 沟对面 2~6 米内有落脚点，而且跑速 × 滞空够得着
					const float Airtime = 2.f * Movement->JumpZVelocity / FMath::Max(1.f, FMath::Abs(Movement->GetGravityZ()));
					const float Reach = Pawn->GetVelocity().Size2D() * Airtime * 0.85f;
					for (float D = 200.f; D <= FMath::Min(600.f, Reach); D += 50.f)
					{
						const float Landing = ProbeDrop(World, Pawn, Foot, Dir, D, JumpHeight);
						if (Landing >= 0.f && Landing < 300.f)
						{
							bCanClear = true;
							break;
						}
					}
				}
				if (bCanClear)
				{
					Character->Jump();
					++GMoveJob.Jumps;
					GMoveJob.LastJumpAt = Now;
				}
				else if (GMoveJob.bStopAtEdge)
				{
					FinishMove(TEXT("edge"), Drop < 0.f ? TEXT("no ground ahead") : FString::Printf(TEXT("a %.0f cm drop ahead"), Drop));
					return false;
				}
			}

			// 前方矮障碍：跑动中起跳
			if (GMoveJob.bAutoJump && Character && Now - GMoveJob.LastJumpAt > 0.6)
			{
				FHitResult Low;
				FCollisionQueryParams Params(SCENE_QUERY_STAT(UALMoveLow), false, Pawn);
				const float Probe = Radius * 0.5f;
				const FVector From(Location.X, Location.Y, Foot.Z + StepHeight + Probe + 2.f);
				if (World->SweepSingleByChannel(Low, From, From + Dir * (Radius + 60.f), FQuat::Identity, ECC_Pawn,
						FCollisionShape::MakeSphere(Probe), Params))
				{
					const FVector Over = FVector(Low.ImpactPoint.X, Low.ImpactPoint.Y, Foot.Z + HalfHeight * 3.f) + Dir * 30.f;
					FHitResult Top;
					float TopHeight = TNumericLimits<float>::Max();
					if (World->LineTraceSingleByChannel(Top, Over, Over - FVector(0.f, 0.f, HalfHeight * 3.f + 50.f), ECC_Pawn, Params))
					{
						TopHeight = Top.ImpactPoint.Z - Foot.Z;
					}
					if (TopHeight < JumpHeight - 10.f && Character->CanJump())
					{
						Character->Jump();
						++GMoveJob.Jumps;
						GMoveJob.LastJumpAt = Now;
						GMoveJob.Note(FString::Printf(TEXT("jump at z=%.0f over a %.0f cm step"), Foot.Z, TopHeight));
					}
					else if (TopHeight < JumpHeight - 10.f)
					{
						if (Now - GMoveJob.LastTallNoteAt > 0.5)
						{
							GMoveJob.LastTallNoteAt = Now;
							GMoveJob.Note(FString::Printf(TEXT("wanted to jump a %.0f cm step but CanJump() is false"), TopHeight));
						}
					}
					else if (Character->JumpMaxCount > 1 && TopHeight < JumpHeight * 1.8f && Character->CanJump())
					{
						Character->Jump();
						++GMoveJob.Jumps;
						GMoveJob.LastJumpAt = Now;
						GMoveJob.PendingSecondJumpAt = Now + 0.2;
						GMoveJob.Note(FString::Printf(TEXT("double jump at z=%.0f toward a %.0f cm ledge"), Foot.Z, TopHeight));
					}
					else if (Now - GMoveJob.LastTallNoteAt > 0.5)
					{
						GMoveJob.LastTallNoteAt = Now;
						GMoveJob.Note(FString::Printf(TEXT("obstacle ahead is %.0f cm high, too high to jump (max %.0f)"), TopHeight, JumpHeight));
					}
				}
			}
		}
		else if (Character && Now - GMoveJob.LastJumpAt > 0.35)
		{
			// 落地之前松开跳跃键，下次才能再跳
			Character->StopJumping();
		}

		Pawn->AddMovementInput(Dir, 1.f);
		return true;
	}

	TSharedPtr<FJsonObject> MoveStatusJson()
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("active"), GMoveJob.bActive);
		Json->SetStringField(TEXT("status"), GMoveJob.Status);
		if (!GMoveJob.Detail.IsEmpty())
		{
			Json->SetStringField(TEXT("detail"), GMoveJob.Detail);
		}
		Json->SetNumberField(TEXT("index"), GMoveJob.Index);
		Json->SetNumberField(TEXT("points"), GMoveJob.Points.Num());
		Json->SetNumberField(TEXT("travelled"), GMoveJob.Travelled);
		Json->SetNumberField(TEXT("jumps"), GMoveJob.Jumps);
		Json->SetNumberField(TEXT("elapsed"), GMoveJob.StartedAt > 0.0 ? FPlatformTime::Seconds() - GMoveJob.StartedAt : 0.0);
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const FString& Event : GMoveJob.Events)
		{
			Events.Add(MakeShared<FJsonValueString>(Event));
		}
		Json->SetArrayField(TEXT("events"), Events);
		return Json;
	}
}

void FUAL_PieBotCommands::Handle_MoveTo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		UAL_CommandUtils::SendError(RequestId, 409, TEXT("No player pawn to move."));
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* PointArray = nullptr;
	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("points"), PointArray) || !PointArray || PointArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("points is required: [{x,y,z}, ...]"));
		return;
	}

	// 路径点先全部验完再动手。原来不是对象的点悄悄丢掉：全丢光时 Points 为空，
	// 下一帧 TickMove 直接判「reached」，调用方拿到 ok:true 和「到了」，角色一步没动。
	// 只丢一部分也不行 —— 少一个拐点就是另一条路线，可能正好撞墙或掉下平台。
	// 缺坐标分量同理：ReadVectorDirect 会拿 0 补，等于往世界原点走。
	// 所以有一个坏点就整单 400，而且放在 FinishMove 之前，不打断正在跑的那次移动
	TArray<FVector> Points;
	TArray<FString> BadPoints;
	for (int32 Index = 0; Index < PointArray->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*PointArray)[Index];
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			BadPoints.Add(FString::Printf(TEXT("#%d is not an object"), Index));
		}
		else if (!(*Obj)->TryGetNumberField(TEXT("x"), X) || !(*Obj)->TryGetNumberField(TEXT("y"), Y) ||
			!(*Obj)->TryGetNumberField(TEXT("z"), Z))
		{
			BadPoints.Add(FString::Printf(TEXT("#%d is missing a numeric x/y/z"), Index));
		}
		else
		{
			Points.Add(FVector(X, Y, Z));
		}
	}
	if (BadPoints.Num() > 0)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("skipped_points"), BadPoints.Num());
		Details->SetNumberField(TEXT("valid_points"), Points.Num());
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("%d of %d points are malformed (%s). Nothing was moved; every point needs {x,y,z}."),
				BadPoints.Num(), PointArray->Num(), *FString::Join(BadPoints, TEXT("; "))),
			Details);
		return;
	}

	FinishMove(TEXT("replaced"), TEXT("a new move replaced this one"));
	GMoveJob = FUAL_MoveJob();
	GMoveJob.Points = MoveTemp(Points);
	double Number = 0.0;
	if (Payload->TryGetNumberField(TEXT("accept_radius"), Number)) GMoveJob.AcceptRadius = static_cast<float>(Number);
	if (Payload->TryGetNumberField(TEXT("final_radius"), Number)) GMoveJob.FinalRadius = static_cast<float>(Number);
	if (Payload->TryGetNumberField(TEXT("max_seconds"), Number)) GMoveJob.MaxSeconds = FMath::Clamp(static_cast<float>(Number), 1.f, 60.f);
	if (Payload->TryGetNumberField(TEXT("edge_drop"), Number)) GMoveJob.EdgeDrop = static_cast<float>(Number);
	Payload->TryGetBoolField(TEXT("auto_jump"), GMoveJob.bAutoJump);
	Payload->TryGetBoolField(TEXT("jump_gaps"), GMoveJob.bJumpGaps);
	Payload->TryGetBoolField(TEXT("stop_at_edge"), GMoveJob.bStopAtEdge);

	GMoveJob.bActive = true;
	GMoveJob.Status = TEXT("running");
	GMoveJob.StartedAt = FPlatformTime::Seconds();
	GMoveJob.LastProgressAt = GMoveJob.StartedAt;
	GMoveJob.LastLocation = Pawn->GetActorLocation();
	GMoveJob.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickMove), 0.0f);

	TSharedPtr<FJsonObject> Result = MoveStatusJson();
	Result->SetBoolField(TEXT("ok"), true);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_PieBotCommands::Handle_MoveStop(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (GMoveJob.bActive)
	{
		FinishMove(TEXT("cancelled"), TEXT("stopped by the caller"));
	}
	TSharedPtr<FJsonObject> Result = MoveStatusJson();
	Result->SetBoolField(TEXT("ok"), true);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

TSharedPtr<FJsonObject> FUAL_PieBotCommands::BuildMoveStatus()
{
	return MoveStatusJson();
}

// ============================================================================
// pie.plan_path —— 没有导航网格时的路径规划（网格 A*）
// ============================================================================
//
// 真机上撞过的坑：角色和出口之间一堵墙，没有导航网格，机器人「径直走 → 撞墙 →
// 往旁边挪一步 → 又径直走」来回撞同一堵墙。缺的不是决策，是「路径」这个东西。
//
// 做法：在角色和目标周围铺一张网格，逐格问两件事 —— 脚下有没有地、这里站不站得下
// （胶囊体重叠测试）。相邻两格之间：高差在台阶高度内 = 能走，在跳跃高度内 = 要跳一下，
// 更高或者落差太大 = 不通。A* 找路，再用「两点之间每一格都能走」把路径拉直成几个拐点。
// 找不到完整的路时回离目标最近的那一格，标 partial。

void FUAL_PieBotCommands::Handle_PlanPath(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = nullptr;
	APlayerController* PC = nullptr;
	if (!ResolvePlayScene(Payload, RequestId, World, PC))
	{
		return;
	}
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	TSharedPtr<FJsonObject> ToObj;
	if (!Pawn || !UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("to"), ToObj))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Needs a player pawn and to:{x,y,z}."));
		return;
	}
	const double StartedAt = FPlatformTime::Seconds();
	const FVector Target = UAL_CommandUtils::ReadVectorDirect(ToObj);

	ACharacter* Character = Cast<ACharacter>(Pawn);
	const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
	const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.f;
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 40.f;
	const float StepHeight = Movement ? Movement->MaxStepHeight : 45.f;
	float JumpHeight = 0.f;
	if (Movement && FMath::Abs(Movement->GetGravityZ()) > KINDA_SMALL_NUMBER)
	{
		JumpHeight = FMath::Square(Movement->JumpZVelocity) / (2.f * FMath::Abs(Movement->GetGravityZ()));
	}
	double MaxDropRaw = 400.0;
	Payload->TryGetNumberField(TEXT("max_drop"), MaxDropRaw);
	const float MaxDrop = static_cast<float>(MaxDropRaw);

	const FVector Start = Pawn->GetActorLocation();
	const float StartFoot = Start.Z - HalfHeight;
	const float Margin = 1500.f;
	const FVector2D Min(FMath::Min(Start.X, Target.X) - Margin, FMath::Min(Start.Y, Target.Y) - Margin);
	const FVector2D Max(FMath::Max(Start.X, Target.X) + Margin, FMath::Max(Start.Y, Target.Y) + Margin);
	// 半米一格：台阶面常常只有一米多深，一米一格会把楼梯切断。区域太大就放大格子，总数封顶 200×200
	const float Cell = FMath::Max(50.f, FMath::Max(Max.X - Min.X, Max.Y - Min.Y) / 200.f);
	const int32 W = FMath::Clamp(FMath::CeilToInt((Max.X - Min.X) / Cell), 2, 220);
	const int32 H = FMath::Clamp(FMath::CeilToInt((Max.Y - Min.Y) / Cell), 2, 220);

	// ---- 多层：每一格从上往下把所有能站的地面都探出来 ----
	// 只取最上面一层的话，站在墙根的角色会被当成站在墙顶；桥下、楼下的路也整层看不见
	struct FNode
	{
		int32 Col;
		float FloorZ;
	};
	TArray<FNode> Nodes;
	TArray<int32> ColumnFirst;
	TArray<int32> ColumnCount;
	ColumnFirst.Init(0, W * H);
	ColumnCount.Init(0, W * H);
	const float TopZ = FMath::Max(Start.Z, Target.Z) + HalfHeight * 2.f + JumpHeight + 600.f;
	const float BottomZ = FMath::Min(StartFoot, Target.Z) - MaxDrop - 400.f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(UALPlan), false, Pawn);
	// 站立检测用瘦一点的胶囊：角色贴着台阶竖面站是正常的，按全宽测会把窄台阶面全判成站不下
	const FCollisionShape Standing = FCollisionShape::MakeCapsule(Radius * 0.5f, HalfHeight - 5.f);
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 Col = Y * W + X;
			ColumnFirst[Col] = Nodes.Num();
			const FVector2D P(Min.X + (X + 0.5f) * Cell, Min.Y + (Y + 0.5f) * Cell);
			float From = TopZ;
			for (int32 Layer = 0; Layer < 4 && From > BottomZ; ++Layer)
			{
				FHitResult Floor;
				if (!World->LineTraceSingleByChannel(Floor, FVector(P, From), FVector(P, BottomZ), ECC_Pawn, Params))
				{
					break;
				}
				const float Z = Floor.ImpactPoint.Z;
				// 下一层从这块地下面接着探。射线从实体内部出发不会打中它自己，会穿到下面那层
				From = Z - 5.f;
				const FVector Stand(P, Z + HalfHeight + 5.f);
				if (World->OverlapAnyTestByChannel(Stand, FQuat::Identity, ECC_Pawn, Standing, Params))
				{
					continue;
				}
				Nodes.Add({ Col, Z });
			}
			ColumnCount[Col] = Nodes.Num() - ColumnFirst[Col];
		}
	}

	auto ColumnOf = [&](const FVector& V) -> int32
	{
		const int32 X = FMath::Clamp(FMath::FloorToInt((V.X - Min.X) / Cell), 0, W - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt((V.Y - Min.Y) / Cell), 0, H - 1);
		return Y * W + X;
	};
	// 在一格（或它周围 Radius 格内）里找地面高度最接近 FootZ、且不高过它太多的那一层
	auto NodeNear = [&](const FVector& V, float FootZ, int32 Radius2) -> int32
	{
		const int32 C = ColumnOf(V);
		const int32 CX = C % W;
		const int32 CY = C / W;
		int32 Best = -1;
		float BestScore = TNumericLimits<float>::Max();
		for (int32 Dy = -Radius2; Dy <= Radius2; ++Dy)
		{
			for (int32 Dx = -Radius2; Dx <= Radius2; ++Dx)
			{
				const int32 X = CX + Dx;
				const int32 Y = CY + Dy;
				if (X < 0 || Y < 0 || X >= W || Y >= H)
				{
					continue;
				}
				const int32 Col = Y * W + X;
				for (int32 I = 0; I < ColumnCount[Col]; ++I)
				{
					const FNode& N = Nodes[ColumnFirst[Col] + I];
					const float Dz = FMath::Abs(N.FloorZ - FootZ);
					if (Dz > JumpHeight + 50.f)
					{
						continue;
					}
					const float Score = Dz + (FMath::Abs(Dx) + FMath::Abs(Dy)) * Cell;
					if (Score < BestScore)
					{
						BestScore = Score;
						Best = ColumnFirst[Col] + I;
					}
				}
			}
		}
		return Best;
	};
	// 两个节点之间：0 = 不通，1 = 走，2 = 要跳
	auto Link = [&](int32 A, int32 B) -> int32
	{
		const float Dz = Nodes[B].FloorZ - Nodes[A].FloorZ;
		if (Dz <= StepHeight + 5.f && Dz >= -MaxDrop)
		{
			return 1;
		}
		if (Dz > 0.f && Dz < JumpHeight - 10.f)
		{
			return 2;
		}
		return 0;
	};
	// 头顶空间：从低的一层迈到高的一层时，高层地面不能压在角色头上
	// （桥底下那一层的邻居里会有桥面 —— 它比角色头还高，不是「跳上去」的台阶）
	auto HeadroomOk = [&](int32 A, int32 B) -> bool
	{
		return Nodes[B].FloorZ - Nodes[A].FloorZ < HalfHeight * 2.f;
	};

	const int32 StartNode = NodeNear(Start, StartFoot, 1);
	const int32 GoalNode = NodeNear(Target, Target.Z - 100.f, 4);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("cell"), Cell);
	Result->SetNumberField(TEXT("grid"), W * H);
	Result->SetNumberField(TEXT("walkable"), Nodes.Num());
	if (StartNode < 0 || GoalNode < 0)
	{
		Result->SetBoolField(TEXT("found"), false);
		Result->SetBoolField(TEXT("partial"), false);
		Result->SetArrayField(TEXT("points"), {});
		Result->SetNumberField(TEXT("length"), 0);
		Result->SetNumberField(TEXT("jumps"), 0);
		Result->SetStringField(TEXT("reason"), StartNode < 0 ? TEXT("no walkable ground under the player") : TEXT("no walkable ground near the target"));
		Result->SetNumberField(TEXT("ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// ---- A*（八邻域；斜走要求两侧直角格有能接上的层，不贴墙角切） ----
	const int32 GoalCol = Nodes[GoalNode].Col;
	auto Heuristic = [&](int32 Node)
	{
		const int32 C = Nodes[Node].Col;
		const int32 Dx = FMath::Abs(C % W - GoalCol % W);
		const int32 Dy = FMath::Abs(C / W - GoalCol / W);
		return (Dx + Dy) + (1.4142f - 2.f) * FMath::Min(Dx, Dy);
	};
	auto HasLinkedLayer = [&](int32 From, int32 Col) -> bool
	{
		for (int32 I = 0; I < ColumnCount[Col]; ++I)
		{
			const int32 N = ColumnFirst[Col] + I;
			if (Link(From, N) != 0 && HeadroomOk(From, N))
			{
				return true;
			}
		}
		return false;
	};
	TArray<float> G;
	G.Init(TNumericLimits<float>::Max(), Nodes.Num());
	TArray<int32> Parent;
	Parent.Init(-1, Nodes.Num());
	TArray<bool> Closed;
	Closed.Init(false, Nodes.Num());
	struct FOpen
	{
		float F;
		int32 Node;
		bool operator<(const FOpen& Other) const { return F < Other.F; }
	};
	TArray<FOpen> Open;
	G[StartNode] = 0.f;
	Open.HeapPush({ Heuristic(StartNode), StartNode });
	int32 Closest = StartNode;
	float ClosestH = Heuristic(StartNode);
	int32 Expanded = 0;
	bool bFound = false;
	while (Open.Num() > 0)
	{
		FOpen Current;
		Open.HeapPop(Current);
		if (Closed[Current.Node])
		{
			continue;
		}
		Closed[Current.Node] = true;
		++Expanded;
		const float Hc = Heuristic(Current.Node);
		if (Hc < ClosestH)
		{
			ClosestH = Hc;
			Closest = Current.Node;
		}
		if (Current.Node == GoalNode)
		{
			bFound = true;
			break;
		}
		const int32 C = Nodes[Current.Node].Col;
		const int32 CX = C % W;
		const int32 CY = C / W;
		for (int32 Dy = -1; Dy <= 1; ++Dy)
		{
			for (int32 Dx = -1; Dx <= 1; ++Dx)
			{
				if ((Dx == 0 && Dy == 0) || CX + Dx < 0 || CY + Dy < 0 || CX + Dx >= W || CY + Dy >= H)
				{
					continue;
				}
				if (Dx != 0 && Dy != 0
					&& (!HasLinkedLayer(Current.Node, CY * W + CX + Dx) || !HasLinkedLayer(Current.Node, (CY + Dy) * W + CX)))
				{
					continue;
				}
				const int32 NCol = (CY + Dy) * W + CX + Dx;
				for (int32 I = 0; I < ColumnCount[NCol]; ++I)
				{
					const int32 N = ColumnFirst[NCol] + I;
					const int32 Kind = Link(Current.Node, N);
					if (Kind == 0 || !HeadroomOk(Current.Node, N))
					{
						continue;
					}
					const float Step = (Dx != 0 && Dy != 0 ? 1.4142f : 1.f) * (Kind == 2 ? 3.f : 1.f);
					if (G[Current.Node] + Step < G[N])
					{
						G[N] = G[Current.Node] + Step;
						Parent[N] = Current.Node;
						Open.HeapPush({ G[N] + Heuristic(N), N });
					}
				}
			}
		}
	}

	TArray<int32> Chain;
	for (int32 N = bFound ? GoalNode : Closest; N != -1; N = Parent[N])
	{
		Chain.Insert(N, 0);
	}
	auto NodePoint = [&](int32 N)
	{
		const int32 C = Nodes[N].Col;
		return FVector(Min.X + (C % W + 0.5f) * Cell, Min.Y + (C / W + 0.5f) * Cell, Nodes[N].FloorZ + HalfHeight);
	};

	// 拉直：只在同一高度上、一路都能走（不跳）的两点之间直连；有跳的地方保留每一个起跳点
	auto StraightOk = [&](int32 AI, int32 BI) -> bool
	{
		for (int32 I = AI + 1; I <= BI; ++I)
		{
			if (Link(Chain[I - 1], Chain[I]) != 1 || FMath::Abs(Nodes[Chain[I]].FloorZ - Nodes[Chain[AI]].FloorZ) > StepHeight)
			{
				return false;
			}
		}
		// 链上相邻没问题，直线上的格子也得能走（链可能绕开了一个柱子）
		const FVector A = NodePoint(Chain[AI]);
		const FVector B = NodePoint(Chain[BI]);
		const int32 Samples = FMath::CeilToInt(FVector::Dist2D(A, B) / (Cell * 0.5f));
		for (int32 S = 1; S < Samples; ++S)
		{
			const FVector P = FMath::Lerp(A, B, static_cast<float>(S) / Samples);
			if (NodeNear(P, Nodes[Chain[AI]].FloorZ, 0) < 0)
			{
				return false;
			}
		}
		return true;
	};
	TArray<TSharedPtr<FJsonValue>> Points;
	int32 JumpCount = 0;
	float Length = 0.f;
	{
		int32 Anchor = 0;
		FVector Last = Start;
		while (Anchor < Chain.Num() - 1)
		{
			int32 Next = Anchor + 1;
			for (int32 Probe = Chain.Num() - 1; Probe > Anchor + 1; --Probe)
			{
				if (StraightOk(Anchor, Probe))
				{
					Next = Probe;
					break;
				}
			}
			if (Link(Chain[Next - 1], Chain[Next]) == 2)
			{
				++JumpCount;
			}
			const FVector P = NodePoint(Chain[Next]);
			Length += FVector::Dist2D(Last, P);
			Last = P;
			Points.Add(MakeShared<FJsonValueObject>(UAL_CommandUtils::MakeVectorJson(P)));
			Anchor = Next;
		}
	}

	Result->SetBoolField(TEXT("found"), bFound);
	Result->SetBoolField(TEXT("partial"), !bFound && Points.Num() > 0);
	Result->SetArrayField(TEXT("points"), Points);
	Result->SetNumberField(TEXT("length"), Length);
	Result->SetNumberField(TEXT("jumps"), JumpCount);
	Result->SetNumberField(TEXT("expanded"), Expanded);
	Result->SetNumberField(TEXT("start_floor"), Nodes[StartNode].FloorZ);
	Result->SetNumberField(TEXT("goal_floor"), Nodes[GoalNode].FloorZ);
	Result->SetBoolField(TEXT("goal_walkable"), true);
	TArray<TSharedPtr<FJsonValue>> Raw;
	for (int32 I = 0; I < Chain.Num() && I < 200; ++I)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		const FVector P = NodePoint(Chain[I]);
		Entry->SetNumberField(TEXT("x"), P.X);
		Entry->SetNumberField(TEXT("y"), P.Y);
		Entry->SetNumberField(TEXT("z"), Nodes[Chain[I]].FloorZ);
		if (I > 0)
		{
			Entry->SetNumberField(TEXT("link"), Link(Chain[I - 1], Chain[I]));
		}
		Raw.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Result->SetArrayField(TEXT("raw"), Raw);
	Result->SetNumberField(TEXT("ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}
