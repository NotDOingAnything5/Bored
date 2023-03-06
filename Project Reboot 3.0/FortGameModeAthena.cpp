#include "FortGameModeAthena.h"

#include "reboot.h"
#include "NetSerialization.h"
#include "FortPlayerControllerAthena.h"
#include "GameplayStatics.h"
#include "KismetStringLibrary.h"
#include "SoftObjectPtr.h"
#include "FortPickup.h"
#include "FortLootPackage.h"
#include "BuildingContainer.h"
#include "events.h"

static bool bFirstPlayerJoined = false;

enum class EDynamicFoundationEnabledState : uint8_t
{
	Unknown = 0,
	Enabled = 1,
	Disabled = 2,
	EDynamicFoundationEnabledState_MAX = 3
};


// Enum FortniteGame.EDynamicFoundationType
enum class EDynamicFoundationType : uint8_t
{
	Static = 0,
	StartEnabled_Stationary = 1,
	StartEnabled_Dynamic = 2,
	StartDisabled = 3,
	EDynamicFoundationType_MAX = 4
};

void ShowFoundation(UObject* BuildingFoundation)
{
	if (!BuildingFoundation)
		return;

	SetBitfield(BuildingFoundation->GetPtr<PlaceholderBitfield>("bServerStreamedInLevel"), 2, true);

	static auto DynamicFoundationTypeOffset = BuildingFoundation->GetOffset("DynamicFoundationType");
	BuildingFoundation->Get<uint8_t>(DynamicFoundationTypeOffset) = true ? 0 : 3;

	static auto OnRep_ServerStreamedInLevelFn = FindObject<UFunction>("/Script/FortniteGame.BuildingFoundation.OnRep_ServerStreamedInLevel");
	BuildingFoundation->ProcessEvent(OnRep_ServerStreamedInLevelFn);

	static auto DynamicFoundationRepDataOffset = BuildingFoundation->GetOffset("DynamicFoundationRepData", false);

	if (DynamicFoundationRepDataOffset != 0)
	{
		auto DynamicFoundationRepData = BuildingFoundation->GetPtr(DynamicFoundationRepDataOffset);

		static auto EnabledStateOffset = FindOffsetStruct("/Script/FortniteGame.DynamicBuildingFoundationRepData", "EnabledState");

		*(EDynamicFoundationEnabledState*)(__int64(DynamicFoundationRepData) + EnabledStateOffset) = EDynamicFoundationEnabledState::Enabled;

		static auto OnRep_DynamicFoundationRepDataFn = FindObject<UFunction>("/Script/FortniteGame.BuildingFoundation.OnRep_DynamicFoundationRepData");
		BuildingFoundation->ProcessEvent(OnRep_DynamicFoundationRepDataFn);
	}

	static auto FoundationEnabledStateOffset = BuildingFoundation->GetOffset("FoundationEnabledState", false);

	if (FoundationEnabledStateOffset != 0)
		BuildingFoundation->Get<EDynamicFoundationEnabledState>(FoundationEnabledStateOffset) = EDynamicFoundationEnabledState::Enabled;
}

static void StreamLevel(std::string LevelName, FVector Location = {})
{
	static auto BuildingFoundation3x3Class = FindObject<UClass>("/Script/FortniteGame.BuildingFoundation3x3");
	FTransform Transform{};
	Transform.Scale3D = { 1, 1, 1 };
	Transform.Translation = Location;
	auto BuildingFoundation = GetWorld()->SpawnActor<ABuildingSMActor>(BuildingFoundation3x3Class, Transform);

	BuildingFoundation->InitializeBuildingActor(BuildingFoundation, nullptr, false);

	static auto FoundationNameOffset = FindOffsetStruct("/Script/FortniteGame.BuildingFoundationStreamingData", "FoundationName");
	static auto FoundationLocationOffset = FindOffsetStruct("/Script/FortniteGame.BuildingFoundationStreamingData", "FoundationLocation");
	static auto StreamingDataOffset = BuildingFoundation->GetOffset("StreamingData");
	static auto LevelToStreamOffset = BuildingFoundation->GetOffset("LevelToStream");

	auto StreamingData = BuildingFoundation->GetPtr<__int64>(StreamingDataOffset);

	*(FName*)(__int64(StreamingData) + FoundationNameOffset) = UKismetStringLibrary::Conv_StringToName(std::wstring(LevelName.begin(), LevelName.end()).c_str());
	*(FVector*)(__int64(StreamingData) + FoundationLocationOffset) = Location;

	*(FName*)(__int64(BuildingFoundation) + LevelToStreamOffset) = UKismetStringLibrary::Conv_StringToName(std::wstring(LevelName.begin(), LevelName.end()).c_str());
	
	static auto OnRep_LevelToStreamFn = FindObject<UFunction>("/Script/FortniteGame.BuildingFoundation.OnRep_LevelToStream");
	BuildingFoundation->ProcessEvent(OnRep_LevelToStreamFn);

	ShowFoundation(BuildingFoundation);
}

UObject* GetPlaylistToUse()
{
	auto Playlist = FindObject("/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

	if (Globals::bCreative)
		Playlist = FindObject("/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2");

	if (Globals::bGoingToPlayEvent)
	{
		if (Fortnite_Version != 12.61)
			Playlist = GetEventPlaylist();
	}

	return Playlist;
}

bool AFortGameModeAthena::Athena_ReadyToStartMatchHook(AFortGameModeAthena* GameMode)
{
	auto GameState = GameMode->GetGameStateAthena();

	auto SetPlaylist = [&GameState](UObject* Playlist) -> void {
		if (Fortnite_Version >= 6.10)
		{
			auto CurrentPlaylistInfo = GameState->GetPtr<FFastArraySerializer>("CurrentPlaylistInfo");

			static auto PlaylistReplicationKeyOffset = FindOffsetStruct("/Script/FortniteGame.PlaylistPropertyArray", "PlaylistReplicationKey");
			static auto BasePlaylistOffset = FindOffsetStruct("/Script/FortniteGame.PlaylistPropertyArray", "BasePlaylist");
			static auto OverridePlaylistOffset = FindOffsetStruct("/Script/FortniteGame.PlaylistPropertyArray", "OverridePlaylist");

			*(UObject**)(__int64(CurrentPlaylistInfo) + BasePlaylistOffset) = Playlist;
			*(UObject**)(__int64(CurrentPlaylistInfo) + OverridePlaylistOffset) = Playlist;

			(*(int*)(__int64(CurrentPlaylistInfo) + PlaylistReplicationKeyOffset))++;
			CurrentPlaylistInfo->MarkArrayDirty();

			GameState->OnRep_CurrentPlaylistInfo();
		}
		else
		{
			GameState->Get("CurrentPlaylistData") = Playlist;
			GameState->OnRep_CurrentPlaylistInfo(); // calls OnRep_CurrentPlaylistData
		}
	};

	auto& LocalPlayers = GetLocalPlayers();

	if (LocalPlayers.Num() && LocalPlayers.Data)
	{
		LocalPlayers.Remove(0);
	}

	static int LastNum2 = 1;

	if (AmountOfRestarts != LastNum2)
	{
		LastNum2 = AmountOfRestarts;

		GameMode->Get<int>("WarmupRequiredPlayerCount") = 1;	
		
		SetPlaylist(GetPlaylistToUse());
		
		auto Fortnite_Season = std::floor(Fortnite_Version);

		if (Fortnite_Season == 6)
		{
			if (Fortnite_Version != 6.10)
			{
				auto Lake = FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake1"));
				auto Lake2 = FindObject("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake2");

				Fortnite_Version <= 6.21 ? ShowFoundation(Lake) : ShowFoundation(Lake2);
				// ^ This shows the lake after or before the event i dont know if this is needed.
			}
			else
			{
				auto Lake = FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest12"));
				ShowFoundation(Lake);
			}

			auto FloatingIsland = Fortnite_Version == 6.10 ? FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest13")) :
				FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_FloatingIsland"));

			ShowFoundation(FloatingIsland);
		}

		if (Fortnite_Season >= 7 && Fortnite_Season <= 10)
		{
			if (Fortnite_Season == 7)
			{
				if (Fortnite_Version == 7.30)
				{
					auto PleasantParkIdk = FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkFestivus"));
					ShowFoundation(PleasantParkIdk);

					auto PleasantParkGround = FindObject("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkDefault");
					ShowFoundation(PleasantParkGround);
				}

				auto PolarPeak = FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_25x36"));
				ShowFoundation(PolarPeak);

				auto tiltedtower = FindObject("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.ShopsNew");
				ShowFoundation(tiltedtower); // 7.40 specific?
			}

			else if (Fortnite_Season == 8)
			{
				auto Volcano = FindObject(("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_50x53_Volcano"));
				ShowFoundation(Volcano);
			}

			else if (Fortnite_Season == 10)
			{
				if (Fortnite_Version >= 10.20)
				{
					auto Island = FindObject("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest16");
					ShowFoundation(Island);
				}
			}

			auto TheBlock = FindObject("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.SLAB_2"); // SLAB_3 is blank
			ShowFoundation(TheBlock);
		}

		if (Fortnite_Version == 17.50f) {
			auto FarmAfter = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.farmbase_2"));
			ShowFoundation(FarmAfter);

			auto FarmPhase = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.Farm_Phase_03")); // Farm Phases (Farm_Phase_01, Farm_Phase_02 and Farm_Phase_03)
			ShowFoundation(FarmPhase);
		}

		if (Fortnite_Version == 17.40f) {
			auto AbductedCoral = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.CoralPhase_02")); // Coral Castle Phases (CoralPhase_01, CoralPhase_02 and CoralPhase_03)
			ShowFoundation(AbductedCoral);

			auto CoralFoundation_01 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation_0"));
			ShowFoundation(CoralFoundation_01);

			auto CoralFoundation_05 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation6"));
			ShowFoundation(CoralFoundation_05);

			auto CoralFoundation_07 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation3"));
			ShowFoundation(CoralFoundation_07);

			auto CoralFoundation_10 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation2_1"));
			ShowFoundation(CoralFoundation_10);

			auto CoralFoundation_13 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation4"));
			ShowFoundation(CoralFoundation_13);

			auto CoralFoundation_17 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.LF_Athena_16x16_Foundation5"));
			ShowFoundation(CoralFoundation_17);
		}

		if (Fortnite_Version == 17.30f) {
			auto AbductedSlurpy = FindObject(("LF_Athena_POI_50x50_C /Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.Slurpy_Phase03")); // Slurpy Swamp Phases (Slurpy_Phase01, Slurpy_Phase02 and Slurpy_Phase03)
			ShowFoundation(AbductedSlurpy);
		}

		if (Fortnite_Season == 13)
		{
			auto SpawnIsland = FindObject("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.Lobby_Foundation");
			ShowFoundation(SpawnIsland);

			// SpawnIsland->RepData->Soemthing = FoundationSetup->LobbyLocation;
		}

		if (Fortnite_Version == 12.41)
		{
			auto JS03 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.LF_Athena_POI_19x19_2"));
			ShowFoundation(JS03);

			auto JH00 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head6_18"));
			ShowFoundation(JH00);

			auto JH01 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head5_14"));
			ShowFoundation(JH01);

			auto JH02 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head3_8"));
			ShowFoundation(JH02);

			auto JH03 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head_2"));
			ShowFoundation(JH03);

			auto JH04 = FindObject(("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head4_11"));
			ShowFoundation(JH04);
		}

		auto PlaylistToUse = GetPlaylistToUse();

		if (PlaylistToUse)
		{
			static auto AdditionalLevelsOffset = PlaylistToUse->GetOffset("AdditionalLevels");
			auto& AdditionalLevels = PlaylistToUse->Get<TArray<TSoftObjectPtr<UClass>>>(AdditionalLevelsOffset);

			for (int i = 0; i < AdditionalLevels.Num(); i++)
			{
				// auto World = Cast<UWorld>(Playlist->AdditionalLevels[i].Get());
				// StreamLevel(UKismetSystemLibrary::GetPathName(World->PersistentLevel).ToString());
				StreamLevel(AdditionalLevels.at(i).SoftObjectPtr.ObjectID.AssetPathName.ToString());
			}
		}
	}

	static int LastNum6 = 1;

	if (AmountOfRestarts != LastNum6)
	{
		LastNum6 = AmountOfRestarts;

		if (Globals::bGoingToPlayEvent && DoesEventRequireLoading())
		{
			bool bb;
			LoadEvent(&bb);

			if (!bb)
				LastNum6 = -1;
		}
	}

	static int LastNum5 = 1;

	if (AmountOfRestarts != LastNum5 && LastNum6 == AmountOfRestarts)
	{
		LastNum5 = AmountOfRestarts;

		bool bb;
		CallOnReadys(&bb);

		if (!bb)
			LastNum5 = -1;
	}

	/* static auto FortPlayerStartWarmupClass = FindObject<UClass>("/Script/FortniteGame.FortPlayerStartWarmup");
	TArray<AActor*> Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FortPlayerStartWarmupClass);

	int ActorsNum = Actors.Num();

	Actors.Free();

	if (ActorsNum == 0)
		return false; */

	static auto MapInfoOffset = GameState->GetOffset("MapInfo");
	auto MapInfo = GameState->Get(MapInfoOffset);
	
	if (!MapInfo)
		return false;

	static auto FlightInfosOffset = MapInfo->GetOffset("FlightInfos");

	static int LastNum3 = 1;

	if (AmountOfRestarts != LastNum3)
	{
		LastNum3 = AmountOfRestarts;

		GetWorld()->Listen();
		SetBitfield(GameMode->GetPtr<PlaceholderBitfield>("bWorldIsReady"), 1, true);

		// GameState->OnRep_CurrentPlaylistInfo();

		// return false;
	}

	// if (GameState->GetPlayersLeft() < GameMode->Get<int>("WarmupRequiredPlayerCount"))
	// if (!bFirstPlayerJoined)
		// return false;

	static int LastNum = 1;

	if (AmountOfRestarts != LastNum)
	{
		LastNum = AmountOfRestarts;

		float Duration = 40.f;
		float EarlyDuration = Duration;

		float TimeSeconds = 35.f; // UGameplayStatics::GetTimeSeconds(GetWorld());

		if (Engine_Version >= 424)
		{
			GameState->GetGamePhase() = EAthenaGamePhase::Warmup;
			GameState->OnRep_GamePhase();
		}

		GameState->Get<float>("WarmupCountdownEndTime") = TimeSeconds + Duration;
		GameMode->Get<float>("WarmupCountdownDuration") = Duration;

		GameState->Get<float>("WarmupCountdownStartTime") = TimeSeconds;
		GameMode->Get<float>("WarmupEarlyCountdownDuration") = EarlyDuration;

		GameState->OnRep_CurrentPlaylistInfo();

		LOG_INFO(LogDev, "Initialized!");
	}

	if (Engine_Version >= 424) // returning true is stripped on c2+
	{
		if (GameState->GetPlayersLeft() >= GameMode->Get<int>("WarmupRequiredPlayerCount"))
		{
			if (MapInfo->Get<TArray<__int64>>(FlightInfosOffset).ArrayNum <= 0)
				return true;
		}
	}

	return Athena_ReadyToStartMatchOriginal(GameMode);
}

int AFortGameModeAthena::Athena_PickTeamHook(AFortGameModeAthena* GameMode, uint8 preferredTeam, AActor* Controller)
{
	static auto NextTeamIndex = 3;
	return ++NextTeamIndex;
}

enum class EFortCustomPartType : uint8_t // todo move
{
	Head = 0,
	Body = 1,
	Hat = 2,
	Backpack = 3,
	Charm = 4,
	Face = 5,
	NumTypes = 6,
	EFortCustomPartType_MAX = 7
};

void AFortGameModeAthena::Athena_HandleStartingNewPlayerHook(AFortGameModeAthena* GameMode, AActor* NewPlayerActor)
{
	if (!NewPlayerActor)
		return;

	auto SpawnIsland_FloorLoot = FindObject<UClass>("/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_Warmup.Tiered_Athena_FloorLoot_Warmup_C");
	auto BRIsland_FloorLoot = FindObject<UClass>("/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_01.Tiered_Athena_FloorLoot_01_C");

	TArray<AActor*> SpawnIsland_FloorLoot_Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), SpawnIsland_FloorLoot);

	TArray<AActor*> BRIsland_FloorLoot_Actors =	UGameplayStatics::GetAllActorsOfClass(GetWorld(), BRIsland_FloorLoot);

	auto SpawnIslandTierGroup = UKismetStringLibrary::Conv_StringToName(L"Loot_AthenaFloorLoot_Warmup");
	auto BRIslandTierGroup = UKismetStringLibrary::Conv_StringToName(L"Loot_AthenaFloorLoot");

	float UpZ = 50;

	EFortPickupSourceTypeFlag SpawnFlag = EFortPickupSourceTypeFlag::Container;

	bool bPrintWarmup = false;

	for (int i = 0; i < SpawnIsland_FloorLoot_Actors.Num(); i++)
	{
		ABuildingContainer* CurrentActor = (ABuildingContainer*)SpawnIsland_FloorLoot_Actors.at(i);

		// CurrentActor->K2_DestroyActor();
		// continue;

		auto Location = CurrentActor->GetActorLocation();
		Location.Z += UpZ;

		std::vector<std::pair<UFortItemDefinition*, int>> LootDrops = PickLootDrops(SpawnIslandTierGroup, bPrintWarmup);

		if (bPrintWarmup)
		{
			std::cout << "\n\n";
		}

		if (LootDrops.size())
		{
			for (auto& LootDrop : LootDrops)
				AFortPickup::SpawnPickup(LootDrop.first, Location, LootDrop.second, SpawnFlag);
		}
	}

	bool bPrint = false;

	int spawned = 0;

	for (int i = 0; i < BRIsland_FloorLoot_Actors.Num(); i++)
	{
		ABuildingContainer* CurrentActor = (ABuildingContainer*)BRIsland_FloorLoot_Actors.at(i);

		// CurrentActor->K2_DestroyActor();
		spawned++;
		// continue;

		auto Location = CurrentActor->GetActorLocation();
		Location.Z += UpZ;

		std::vector<std::pair<UFortItemDefinition*, int>> LootDrops = PickLootDrops(BRIslandTierGroup, bPrint);

		if (bPrint)
			std::cout << "\n";

		if (LootDrops.size())
		{
			for (auto& LootDrop : LootDrops)
				AFortPickup::SpawnPickup(LootDrop.first, Location, LootDrop.second, SpawnFlag);
		}
	}

	auto GameState = GameMode->GetGameStateAthena();

	auto NewPlayer = (AFortPlayerControllerAthena*)NewPlayerActor;

	auto WorldInventory = NewPlayer->GetWorldInventory();

	static UFortItemDefinition* EditToolItemDefinition = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/EditTool.EditTool");
	static UFortItemDefinition* PickaxeDefinition = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");
	static UFortItemDefinition* BuildingItemData_Wall = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall");
	static UFortItemDefinition* BuildingItemData_Floor = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor");
	static UFortItemDefinition* BuildingItemData_Stair_W = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W");
	static UFortItemDefinition* BuildingItemData_RoofS = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS");
	static UFortItemDefinition* WoodItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData");

	WorldInventory->AddItem(EditToolItemDefinition, nullptr);
	WorldInventory->AddItem(BuildingItemData_Wall, nullptr);
	WorldInventory->AddItem(BuildingItemData_Floor, nullptr);
	WorldInventory->AddItem(BuildingItemData_Stair_W, nullptr);
	WorldInventory->AddItem(BuildingItemData_RoofS, nullptr);
	WorldInventory->AddItem(PickaxeDefinition, nullptr);
	WorldInventory->AddItem(WoodItemData, nullptr, 100);

	WorldInventory->Update(true);

	auto PlayerStateAthena = NewPlayer->GetPlayerStateAthena();

	static auto CharacterPartsOffset = PlayerStateAthena->GetOffset("CharacterParts", false);

	if (CharacterPartsOffset != 0)
	{
		auto CharacterParts = PlayerStateAthena->GetPtr<__int64>("CharacterParts");
		
		static auto PartsOffset = FindOffsetStruct("/Script/FortniteGame.CustomCharacterParts", "Parts");
		auto Parts = (UObject**)(__int64(CharacterParts) + PartsOffset); // UCustomCharacterPart* Parts[0x6]

		static auto headPart = LoadObject("/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
		static auto bodyPart = LoadObject("/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");

		Parts[(int)EFortCustomPartType::Head] = headPart;
		Parts[(int)EFortCustomPartType::Body] = bodyPart;

		static auto OnRep_CharacterPartsFn = FindObject<UFunction>("/Script/FortniteGame.FortPlayerState.OnRep_CharacterParts");
		PlayerStateAthena->ProcessEvent(OnRep_CharacterPartsFn);
	}

	PlayerStateAthena->GetSquadId() = PlayerStateAthena->GetTeamIndex() - 2;

	// if (false)
	{
		// idk if this is needed

		static auto bHasServerFinishedLoadingOffset = NewPlayer->GetOffset("bHasServerFinishedLoading");
		NewPlayer->Get<bool>(bHasServerFinishedLoadingOffset) = true;

		static auto OnRep_bHasServerFinishedLoadingFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.OnRep_bHasServerFinishedLoading");
		NewPlayer->ProcessEvent(OnRep_bHasServerFinishedLoadingFn);

		static auto bHasStartedPlayingOffset = PlayerStateAthena->GetOffset("bHasStartedPlaying");
		PlayerStateAthena->Get<bool>(bHasStartedPlayingOffset) = true; // this is a bitfield!!!

		static auto OnRep_bHasStartedPlayingFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerState.OnRep_bHasStartedPlaying");
		PlayerStateAthena->ProcessEvent(OnRep_bHasStartedPlayingFn);
	}

	// if (false)
	{
		static auto GameplayAbilitySet = LoadObject<UObject>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer") ? 
			LoadObject<UObject>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer") :
			LoadObject<UObject>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_DefaultPlayer.GAS_DefaultPlayer");

		// LOG_INFO(LogDev, "GameplayAbilitySet {}", __int64(GameplayAbilitySet));

		if (GameplayAbilitySet)
		{
			static auto GameplayAbilitiesOffset = GameplayAbilitySet->GetOffset("GameplayAbilities");
			auto GameplayAbilities = GameplayAbilitySet->GetPtr<TArray<UClass*>>(GameplayAbilitiesOffset);

			for (int i = 0; i < GameplayAbilities->Num(); i++)
			{
				UClass* AbilityClass = GameplayAbilities->At(i);

				// LOG_INFO(LogDev, "AbilityClass {}", __int64(AbilityClass));

				if (!AbilityClass)
					continue;

				// LOG_INFO(LogDev, "AbilityClass Name {}", AbilityClass->GetFullName());

				// LOG_INFO(LogDev, "DefaultAbility {}", __int64(DefaultAbility));
				// LOG_INFO(LogDev, "DefaultAbility Name {}", DefaultAbility->GetFullName());

				PlayerStateAthena->GetAbilitySystemComponent()->GiveAbilityEasy(AbilityClass);
			}
		}
	}
	
	static auto GameMemberInfoArrayOffset = GameState->GetOffset("GameMemberInfoArray", false);

	// if (false)
	// if (GameMemberInfoArrayOffset != 0)
	if (Engine_Version >= 423)
	{
		struct FUniqueNetIdRepl
		{
			unsigned char ahh[0x0028];
		};

		struct FGameMemberInfo : public FFastArraySerializerItem
		{
			unsigned char                                      SquadId;                                                  // 0x000C(0x0001) (ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
			unsigned char                                      TeamIndex;                                                // 0x000D(0x0001) (ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
			unsigned char                                      UnknownData00[0x2];                                       // 0x000E(0x0002) MISSED OFFSET
			FUniqueNetIdRepl                            MemberUniqueId;                                           // 0x0010(0x0028) (HasGetValueTypeHash, NativeAccessSpecifierPublic)
		};

		static auto GameMemberInfoStructSize = 0x38;
		// LOG_INFO(LogDev, "Compare: 0x{:x} 0x{:x}", GameMemberInfoStructSize, sizeof(FGameMemberInfo));

		auto GameMemberInfo = Alloc<FGameMemberInfo>(GameMemberInfoStructSize);

		((FFastArraySerializerItem*)GameMemberInfo)->MostRecentArrayReplicationKey = -1;
		((FFastArraySerializerItem*)GameMemberInfo)->ReplicationID = -1;
		((FFastArraySerializerItem*)GameMemberInfo)->ReplicationKey = -1;

		if (false)
		{
			static auto GameMemberInfo_SquadIdOffset = 0x000C;
			static auto GameMemberInfo_TeamIndexOffset = 0x000D;
			static auto GameMemberInfo_MemberUniqueIdOffset = 0x0010;
			static auto UniqueIdSize = 0x0028;

			*(uint8*)(__int64(GameMemberInfo) + GameMemberInfo_SquadIdOffset) = PlayerStateAthena->GetSquadId();
			*(uint8*)(__int64(GameMemberInfo) + GameMemberInfo_TeamIndexOffset) = PlayerStateAthena->GetTeamIndex();
			CopyStruct((void*)(__int64(GameMemberInfo) + GameMemberInfo_MemberUniqueIdOffset), PlayerStateAthena->Get<void*>("UniqueId"), UniqueIdSize);

		}
		else
		{
			GameMemberInfo->SquadId = PlayerStateAthena->GetSquadId();
			GameMemberInfo->TeamIndex = PlayerStateAthena->GetTeamIndex();
			GameMemberInfo->MemberUniqueId = PlayerStateAthena->Get<FUniqueNetIdRepl>("UniqueId");
		}

		static auto GameMemberInfoArray_MembersOffset = FindOffsetStruct("/Script/FortniteGame.GameMemberInfoArray", "Members");

		auto GameMemberInfoArray = GameState->GetPtr<FFastArraySerializer>(GameMemberInfoArrayOffset);

		((TArray<FGameMemberInfo>*)(__int64(GameMemberInfoArray) + GameMemberInfoArray_MembersOffset))->Add(*GameMemberInfo, GameMemberInfoStructSize);
		GameMemberInfoArray->MarkArrayDirty();
	}

	return Athena_HandleStartingNewPlayerOriginal(GameMode, NewPlayerActor);
}