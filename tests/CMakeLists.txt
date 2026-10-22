find_package(Catch2 CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
include(Catch)

# Detailed state comparisons retain legacy capture/catalog codecs only in tests.
# No production target links these snapshots or their compression dependency.
find_package(lz4 CONFIG REQUIRED)
add_library(game_diagnostics STATIC
    game/enemies/CombatantCheckpoint.cpp
    game/enemies/CrittersCheckpoint.cpp
    game/enemies/PlacedEnemiesCheckpoint.cpp
    game/screens/PlayerAttacksCheckpoint.cpp
    game/screens/OpponentCombatCheckpoint.cpp
    game/screens/LevelFixturesCheckpoint.cpp
    game/world/ItemFigureCheckpoint.cpp
    game/world/CombatantProjectilesCheckpoint.cpp
    game/world/ChestsCheckpoint.cpp
    game/world/LockedGatesCheckpoint.cpp
    game/world/BreakablesCheckpoint.cpp
    game/world/SafeRocksCheckpoint.cpp
    game/world/StaticSceneryCheckpoint.cpp
    game/world/RubbleCheckpoint.cpp
    game/world/TrapsCheckpoint.cpp
    game/world/DestructibleWallsCheckpoint.cpp
    game/world/PlacedItemsCheckpoint.cpp
    game/world/LevelTransportersCheckpoint.cpp
    game/world/ExitPortalsCheckpoint.cpp
    engine/world/WorldStateCheckpoint.cpp
    game/world/PlayerMissilesCheckpoint.cpp
    game/world/EffectTreesCheckpoint.cpp
    game/world/RotatorsCheckpoint.cpp
    game/world/LevelWorldCheckpoint.cpp
    game/world/PlayerArsenalCheckpoint.cpp
    game/world/WorldDestructionCheckpoint.cpp
    game/world/FallingSceneryCheckpoint.cpp
    game/world/TowerCameraCheckpoint.cpp
    game/netplay/PlayerHealthCheckpoint.cpp
    game/netplay/PlayerPoseCheckpoint.cpp
    game/netplay/PlayerPotionCheckpoint.cpp
    game/netplay/TurboEffectCheckpoint.cpp
    game/netplay/PlayerActionCheckpoint.cpp
    game/netplay/AnimationCheckpoint.cpp
    game/netplay/RandomCheckpoint.cpp
    game/netplay/EnemyMissilesCheckpoint.cpp
    game/netplay/EnemyPoolCheckpoint.cpp
    game/netplay/GeneratorCheckpoint.cpp
    game/netplay/PlacedEnemyCheckpoint.cpp
    game/netplay/PlayerTurboCheckpoint.cpp
    game/netplay/PlayerKnockbackCheckpoint.cpp
    game/screens/PlayerRuntimeCheckpoint.cpp
    game/enemies/OpponentState.cpp
    game/screens/OpponentResources.cpp
    game/screens/OpponentCheckpoint.cpp
    game/screens/CombatResources.cpp
    game/screens/CombatRollbackCheckpoint.cpp
    game/screens/StageResources.cpp
    game/netplay/CombatStatePacket.cpp
    game/netplay/StageStatePacket.cpp
    game/netplay/PlayerRuntimePacket.cpp)
target_include_directories(game_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_diagnostics PUBLIC gdl::game PRIVATE lz4::lz4)
gdl_apply_compiler_options(game_diagnostics)

add_executable(tests
    engine/io/NativeStateDigestTests.cpp
    game/screens/PlayerNativeStateTests.cpp
    game/enemies/EnemyMissilesNativeDigestTests.cpp
    game/world/CombatantProjectilesNativeDigestTests.cpp
    game/world/LevelTriggersNativeDigestTests.cpp
    game/world/SecretChallengeNativeDigestTests.cpp
    game/world/NativeStateResourcesTests.cpp
    game/enemies/GeneratorNativeDigestTests.cpp
    game/enemies/EnemiesNativeDigestTests.cpp
    game/enemies/CombatantNativeDigestTests.cpp
    game/enemies/PlacedEnemiesNativeDigestTests.cpp
    game/enemies/OpponentOwnersNativeDigestTests.cpp
    game/enemies/CritterStatuesNativeDigestTests.cpp
    game/world/CameraMeterNativeDigestTests.cpp
    game/world/EffectTreesNativeDigestTests.cpp
    game/screens/PlayerAttackNativeDigestTests.cpp
    game/world/PlayerFigureNativeDigestTests.cpp
    game/world/LevelWorldNativeDigestTests.cpp
    game/screens/LevelFixturesNativeDigestTests.cpp
    game/world/SceneAudioNativeDigestTests.cpp
    game/screens/SceneTravelNativeDigestTests.cpp
    game/screens/SceneHudNativeDigestTests.cpp
    game/screens/PlaySceneNativeStateDigestTests.cpp
    game/netplay/LocalStateHistoryTests.cpp
    game/netplay/RollbackExchangeTests.cpp
    game/screens/NativeSoundIdentityTests.cpp
    game/netplay/RollbackInputsTests.cpp
    game/netplay/InputJournalTests.cpp
    game/netplay/FrameEventLedgerTests.cpp
    game/screens/PlayerLocalRewindTests.cpp
    game/screens/PlayerSimulationEqualityTests.cpp
    game/netplay/NativeProbeTimingTests.cpp
    engine/app/ApplicationTimingTests.cpp
    engine/math/Mt19937Tests.cpp
    game/netplay/HostedRoomTests.cpp
    game/netplay/InputCommandTests.cpp
    game/netplay/InputTimelineTests.cpp
    game/netplay/ShotIdentityTests.cpp
    game/netplay/PlayerActionCheckpointTests.cpp
    game/netplay/PlayerTurboCheckpointTests.cpp
    game/netplay/AnimationCheckpointTests.cpp
    game/netplay/RandomCheckpointTests.cpp
    game/netplay/EnemyMissilesCheckpointTests.cpp
    game/netplay/EnemyPoolCheckpointTests.cpp
    game/netplay/GeneratorCheckpointTests.cpp
    game/netplay/PlacedEnemyCheckpointTests.cpp
    game/screens/OpponentPlacementsTests.cpp
    game/screens/PlayerAttacksCheckpointTests.cpp
    game/screens/PlayerRuntimeCheckpointTests.cpp
    game/screens/CombatRollbackCheckpointTests.cpp
    game/netplay/PlayerRuntimePacketTests.cpp
    game/netplay/CombatStatePacketTests.cpp
    game/netplay/PlayerHealthCheckpointTests.cpp
    game/screens/OpponentCombatCheckpointTests.cpp
    game/screens/OpponentLootCheckpointTests.cpp
    game/netplay/StageStatePacketTests.cpp
    game/netplay/CanonicalDigestSinkTests.cpp
    game/world/PlayerCompanionCheckpointTests.cpp
    game/screens/PartyLightingTests.cpp
    game/world/ExitPortalsCheckpointTests.cpp
    game/world/LevelTransportersCheckpointTests.cpp
    game/screens/CombatResourcesTests.cpp
    game/world/ChestsCheckpointTests.cpp
    game/world/LockedGatesCheckpointTests.cpp
    game/world/BreakablesCheckpointTests.cpp
    game/world/SafeRocksCheckpointTests.cpp
    game/world/StaticSceneryCheckpointTests.cpp
    game/world/RubbleCheckpointTests.cpp
    game/world/TrapsCheckpointTests.cpp
    game/world/FallingSceneryCheckpointTests.cpp
    game/world/DestructibleWallsCheckpointTests.cpp
    game/world/RotatorsCheckpointTests.cpp
    game/world/RotatorsLocalRewindTests.cpp
    game/world/PlacedItemsCheckpointTests.cpp
    game/world/PreparedPickupBirthTests.cpp
    game/screens/LevelFixturesCheckpointTests.cpp
    game/world/LevelWorldCheckpointTests.cpp
    game/world/TowerCameraCheckpointTests.cpp
    engine/world/WorldStateCheckpointTests.cpp
    engine/world/WorldCollapsedCollisionTests.cpp
    game/world/WorldDestructionCheckpointTests.cpp
    game/screens/EnemyFeedbackEmissionTests.cpp
    game/screens/PartyDamageTests.cpp
    game/world/PlayerArsenalCheckpointTests.cpp
    game/world/MoveStrikesCheckpointTests.cpp
    game/netplay/EffectIdentityTests.cpp
    game/netplay/PlayerPoseCheckpointTests.cpp
    game/netplay/PlayerPotionCheckpointTests.cpp
    game/world/WorldMotionTests.cpp
    game/screens/TurboEmissionTests.cpp
    game/netplay/TurboEffectResourcesTests.cpp
    game/netplay/TurboEffectCheckpointTests.cpp
    game/combat/MeleeGeometryTests.cpp
    game/netplay/PlayerKnockbackCheckpointTests.cpp
    game/players/MeleeSelectionTests.cpp
    game/players/PlayerActorCheckpointTests.cpp
    game/enemies/EnemyAnimatorCheckpointTests.cpp
    game/enemies/EnemiesCheckpointTests.cpp
    game/enemies/GeneratorsCheckpointTests.cpp
    game/enemies/EnemyMissilesCheckpointTests.cpp
    game/enemies/PlacedEnemiesTests.cpp
    game/enemies/CritterStatuesCheckpointTests.cpp
    game/screens/OpponentSimulationTests.cpp
    game/screens/OpponentResourcesTests.cpp
    game/screens/CombatProgressCheckpointTests.cpp
    game/netplay/MatchSessionTests.cpp
    game/netplay/ShopSyncTests.cpp
    game/screens/OnlineCompletionTests.cpp
    game/netplay/OnlineSessionTests.cpp
    game/netplay/CharacterProfileTests.cpp
    game/netplay/PartyCheckpointTests.cpp
    game/netplay/PartyBootstrapTests.cpp
    game/netplay/OnlinePartyTests.cpp
    game/netplay/MeasuredPacketTransportTests.cpp
    game/netplay/SessionInputsTests.cpp
    game/app/BuildLabelTests.cpp
    game/app/LevelDebugOverlayTests.cpp
    game/app/GameCursorTests.cpp
    game/screens/PlaySceneRumbleTests.cpp
    game/world/StaticSceneryTests.cpp
    game/config/ControlProfilesTests.cpp
    game/menu/ControlPromptsTests.cpp
    game/players/CursorAimTests.cpp
    game/enemies/DeathRulesTests.cpp
    game/enemies/EnemiesDeathTests.cpp
    game/screens/LevelOpponentsDeathTests.cpp
    game/screens/PlaySceneAmbienceTests.cpp
    game/world/DestructibleWallsTests.cpp
    game/world/MissileTargetTests.cpp
    game/combat/DamageTests.cpp
    game/combat/BodyContactTests.cpp
    game/combat/DamageTypesTests.cpp
    game/screens/PlayerPowerupsTests.cpp
    game/menu/SettingsMenuTests.cpp
    game/menu/SettingsRowsTests.cpp
    game/menu/VideoSettingsTests.cpp
    game/menu/CompassHudTests.cpp
    game/screens/SaveMenuTests.cpp
    game/screens/PauseMenuTests.cpp
    formats/ShopWadTests.cpp
    game/players/ShopTests.cpp
    game/screens/AfterLevelSceneTests.cpp
    game/screens/AutoSaveIconTests.cpp
    game/screens/PostShopReturnTests.cpp
    game/screens/InventoryPanelTests.cpp
    TestSupport.h
    fixtures/ReferenceObj.cpp
    fixtures/ReferenceObjTests.cpp
    fixtures/NativeModelFixture.cpp
    fixtures/ReferenceWav.cpp
    fixtures/ReferenceWavTests.cpp
    fixtures/NativeSoundBank.cpp
    FakeRenderDevice.h
    engine/core/TypesTests.cpp
    engine/core/LogTests.cpp
    engine/core/AssertTests.cpp
    engine/core/ClockTests.cpp
    engine/math/MathTests.cpp
    engine/math/PortableMathTests.cpp
    engine/io/ByteReaderTests.cpp
    engine/io/FileTests.cpp
    engine/io/AssetLocatorTests.cpp
    engine/platform/InputTests.cpp
    engine/platform/GamepadsTests.cpp
    engine/platform/GlfwWindowTests.cpp
    engine/platform/DisplayTimingTests.cpp
    engine/platform/PathsTests.cpp
    engine/render/RenderTypesTests.cpp
    engine/render/VulkanTextureTests.cpp
    engine/render/VulkanCoverageTests.cpp
    engine/render/DepthOfFieldTests.cpp
    engine/render/HeatDistortionTests.cpp
    engine/render/ImageTests.cpp
    engine/render/ImmediateBatchTests.cpp
    engine/render/ThreadedRenderDeviceTests.cpp
    engine/render/DebugTexturesTests.cpp
    engine/codec/LzssTests.cpp
    engine/codec/AviReaderTests.cpp
    engine/codec/VqVideoDecoderTests.cpp
    engine/codec/DspAdpcmTests.cpp
    engine/codec/AdsAudioTests.cpp
    engine/codec/MoviePlaybackTests.cpp
    engine/audio/AudioStreamTests.cpp
    engine/audio/AudioMixerTests.cpp
    engine/audio/AudioDeviceTests.cpp
    engine/app/ApplicationTests.cpp
    engine/render/VulkanFrameAcquireTests.cpp
    engine/core/StringsTests.cpp
    engine/assets/PngImageTests.cpp
    engine/assets/TextureSetTests.cpp
    engine/assets/TextureBindingsTests.cpp
    engine/assets/AssetScopeTests.cpp
    engine/assets/ObjectMaterialTests.cpp
    engine/world/AssetAuditTests.cpp
    engine/assets/NativeDataAuditTests.cpp
    engine/assets/NativeAssetsTests.cpp
    engine/assets/NativeShopTests.cpp
    engine/assets/BitmapFontTests.cpp
    engine/assets/SoundSetTests.cpp
    engine/audio/SoundPlayerTests.cpp
    engine/audio/AdsStreamTests.cpp
    engine/audio/MusicDeclickerTests.cpp
    engine/audio/StreamPlaylistTests.cpp
    engine/assets/ModelSetTests.cpp
    engine/assets/AnimationSetTests.cpp
    engine/ui/ModelSpriteTests.cpp
    engine/world/AmbientDimmerTests.cpp
    engine/world/WorldCameraTests.cpp
    engine/world/WorldSceneTests.cpp
    engine/world/TextureAnimatorTests.cpp
    engine/world/WorldAnimatorTests.cpp
    game/world/WorldDestructionTests.cpp
    engine/world/ParticleSystemTests.cpp
    engine/world/ParticleCheckpointTests.cpp
    engine/world/ParticleFieldTests.cpp
    engine/world/ParticleLocalRewindTests.cpp
    engine/world/TreeParticlesTests.cpp
    engine/world/WorldCollisionTests.cpp
    engine/world/TreeModelTests.cpp
    engine/world/AnimationPlayerTests.cpp
    engine/world/TreePoseTests.cpp
    engine/assets/WorldLayoutTests.cpp
    formats/WorldFileTests.cpp
    formats/WorldDataWadTests.cpp
    engine/assets/WorldDataTests.cpp
    engine/assets/MessageTableTests.cpp
    game/menu/ScrollBoxTests.cpp
    game/world/SumnerFigureTests.cpp
    game/world/SumnerHintsTests.cpp
    game/world/PlayerMissilesTests.cpp
    game/world/PlayerMissilesCheckpointTests.cpp
    game/world/EffectTreesPoolCheckpointTests.cpp
    game/world/EffectTreesLocalRewindTests.cpp
    game/world/ItemFigureLocalRewindTests.cpp
    game/world/TargetAssistTests.cpp
    game/world/BodyCollisionTests.cpp
    game/world/BlobShadowTests.cpp
    game/world/PlayerFigureTests.cpp
    game/world/AssetCensusTests.cpp
    game/world/PlayerFamiliarTests.cpp
    game/world/MikeyFigureTests.cpp
    game/world/WeaponGlowTests.cpp
    game/world/CameraMovementLimitTests.cpp
    game/world/SkorneArenaTests.cpp
    game/world/FallingPieceTests.cpp
    game/world/FallingSceneryTests.cpp
    game/enemies/SkorneTests.cpp
    game/enemies/SpiderQueenTests.cpp
    game/enemies/LichTests.cpp
    game/world/PlayerArsenalTests.cpp
    game/world/PotionEmissionTests.cpp
    game/world/PotionFlightTests.cpp
    game/world/PojoTests.cpp
    game/world/PojoVoiceTests.cpp
    game/world/LevelSoundscapeTests.cpp
    game/world/VoiceQueueTests.cpp
    game/menu/HintMenuTests.cpp
    game/world/PlacedItemsTests.cpp
    game/world/AmbientSoundsTests.cpp
    game/world/MusicAreasTests.cpp
    engine/world/WorldLightingTests.cpp
    engine/ui/CanvasTests.cpp
    engine/ui/TextPainterTests.cpp
    formats/ModelArchiveTests.cpp
    formats/GcTextureTests.cpp
    formats/FontFileTests.cpp
    formats/TextRomTests.cpp
    formats/JsonWriterTests.cpp
    formats/AudioRomTests.cpp
    formats/SoundBankTests.cpp
    formats/WavWriterTests.cpp
    formats/GeometryStreamTests.cpp
    formats/AnimationTreeTests.cpp
    formats/ObjWriterTests.cpp
    game/app/CommandLineTests.cpp
    game/app/ScenarioTests.cpp
    game/app/AttractSequencerTests.cpp
    game/app/LevelCompletionTests.cpp
    game/app/MusicDuckTests.cpp
    game/screens/MovieSceneTests.cpp
    game/menu/MenuInputTests.cpp
    game/menu/OptionMenuTests.cpp
    game/screens/TitleSceneTests.cpp
    game/screens/IdleScreenTests.cpp
    game/screens/AttractSceneTests.cpp
    game/config/GameConfigTests.cpp
    game/players/ClassDataTests.cpp
    game/players/NativeClassDataTests.cpp
    game/enemies/NativeCritterDataTests.cpp
    game/enemies/CombatantBindingsTests.cpp
    game/app/AssetRootsTests.cpp
    game/screens/NativeGameplayTests.cpp
    game/players/MagicPerksTests.cpp
    game/players/ProgressionTests.cpp
    game/players/CharacterSaveTests.cpp
    game/players/AutoSaveTests.cpp
    game/players/NameCheatsTests.cpp
    game/players/PartyTests.cpp
    game/players/TurboMeterTests.cpp
    game/players/TurboMoveTests.cpp
    game/players/InventoryTests.cpp
    game/players/MikeyDecoyTests.cpp
    game/players/RelicsTests.cpp
    game/players/ItemPickupTests.cpp
    game/players/PowerupEffectsTests.cpp
    game/players/EnemyShrinkTests.cpp
    game/players/PowerupEndingsTests.cpp
    game/screens/PowerupSelectorTests.cpp
    game/screens/TransitionScreenTests.cpp
    game/screens/LevelLoadingScreenTests.cpp
    game/screens/PortalDepartureTests.cpp
    game/screens/LevelExitSpeechTests.cpp
    game/world/DynamicLightsTests.cpp
    game/world/EffectTreesTests.cpp
    game/world/EffectCheckpointTests.cpp
    game/world/CombatantProjectilesTests.cpp
    game/world/CombatantProjectilesCheckpointTests.cpp
    game/players/PlayerControlsTests.cpp
    game/players/PlayerActorTests.cpp
    game/players/PlayerCaptureTests.cpp
    game/players/ComboMoveTests.cpp
    game/screens/PartyComboTests.cpp
    game/players/PlayerTransportTests.cpp
    game/players/PlayerAnimatorTests.cpp
    game/players/PlayerAnimatorCadenceTests.cpp
    game/players/PlayerAnimatorInterruptTests.cpp
    game/players/ItemAttackTests.cpp
    game/players/KnockbackTests.cpp
    game/players/PlayerImpactTests.cpp
    game/world/LevelWorldTests.cpp
    game/world/LevelCatalogTests.cpp
    game/world/LevelItemArchivesTests.cpp
    game/world/TowerAccessTests.cpp
    game/players/LevelRecordTests.cpp
    game/world/ExitPortalsTests.cpp
    game/world/LevelTransportersTests.cpp
    game/screens/PlaySceneTransportTests.cpp
    game/screens/PlaySceneSwitchTests.cpp
    game/screens/SwitchCutsceneTests.cpp
    game/world/LevelFixturesTests.cpp
    game/world/RubbleTests.cpp
    game/world/SafeRocksTests.cpp
    game/players/LevelWatchTests.cpp
    game/enemies/EnemyKindsTests.cpp
    game/enemies/EnemyAnimatorTests.cpp
    game/enemies/EnemyFeedbackTests.cpp
    game/enemies/EnemyMindTests.cpp
    game/enemies/EnemyMissilesTests.cpp
    game/enemies/EnemiesTests.cpp
    game/enemies/GeneratorsTests.cpp
    game/enemies/CrittersTests.cpp
    game/enemies/CombatantTests.cpp
    game/enemies/CombatantCheckpointTests.cpp
    game/enemies/CombatantLocalRewindTests.cpp
    game/enemies/CrittersLocalRewindTests.cpp
    game/enemies/BossLocalRewindTests.cpp
    game/enemies/CrittersCheckpointTests.cpp
    game/enemies/ChimeraTests.cpp
    game/enemies/CombatantGazeTests.cpp
    game/enemies/CombatantGrabsTests.cpp
    game/enemies/CombatantNodesTests.cpp
    game/world/CameraShakeTests.cpp
    game/enemies/CritterAttackSelectionTests.cpp
    game/enemies/CritterAreaTests.cpp
    game/enemies/CritterMovementTests.cpp
    game/enemies/CritterPatrolTests.cpp
    game/enemies/CritterStatuesTests.cpp
    game/enemies/GargoyleTests.cpp
    game/enemies/CombatantBreathTests.cpp
    game/enemies/CombatantProjectileTests.cpp
    formats/CritterWadTests.cpp
    game/enemies/BossCoinsTests.cpp
    game/enemies/BossesTests.cpp
    game/enemies/LegendItemsTests.cpp
    game/world/MoveStrikesTests.cpp
    game/screens/HelpMessagesTests.cpp
    game/world/LevelTriggersTests.cpp
    game/world/LevelTriggersCheckpointTests.cpp
    game/world/HazardSurfacesTests.cpp
    game/world/RotatorsTests.cpp
    game/world/TowerCameraTests.cpp
    game/world/BossCameraTests.cpp
    game/world/StartCameraTests.cpp
    game/screens/StatusBoxTests.cpp
    game/screens/PickupHudTests.cpp
    game/screens/BossMeterTests.cpp
    game/screens/ChallengeHudTests.cpp
    game/screens/RuneMeterTests.cpp
    game/screens/PlaySceneChallengeTests.cpp
    game/world/SecretChallengeTests.cpp
    game/screens/BossVictoryTests.cpp
    game/screens/BossVictoryPresentationTests.cpp
    game/screens/BossSequenceTests.cpp
    game/screens/LevelArrivalPresentationTests.cpp
    game/screens/LevelMessagesTests.cpp
    game/screens/GameOverTests.cpp
    game/screens/SumnerVisitTests.cpp
    game/screens/TowerPromotionTests.cpp
    game/screens/TowerRelicsTests.cpp
    game/screens/TowerCompletionTests.cpp
    game/screens/LegendPresentationTests.cpp
    game/screens/PlaySceneTests.cpp
    game/screens/PlaySceneG4Tests.cpp
    game/screens/MultiplayerJourneyTests.cpp
    game/screens/PartyMotionTests.cpp
    game/screens/PartyCollisionTests.cpp
    game/players/HeadGemTests.cpp
    game/players/BodyGlowTests.cpp
    game/players/MeleeStreakTests.cpp
    game/players/ExitWaitTests.cpp
    game/world/PowerupCompanionTests.cpp
    game/world/WeaponTrailTests.cpp
    game/screens/FloorRidingTests.cpp
    game/world/NativeTraversalTests.cpp
    game/screens/PartyFiguresTests.cpp
    game/screens/PartyNamesTests.cpp
    game/screens/PartyHudTests.cpp
    game/screens/PartyRecordsTests.cpp
    game/screens/TriggerCuesTests.cpp
    game/screens/TowerWelcomeTests.cpp
    game/screens/PartyPickupsTests.cpp
    game/screens/LevelFixturesTests.cpp
    game/screens/LevelOpponentsTests.cpp
    game/screens/PlayerHealthTests.cpp
    game/screens/PlayerAttacksTests.cpp
    game/menu/NameEntryTests.cpp
    game/screens/SelectLaneTests.cpp
    game/screens/PlayerSelectSceneTests.cpp
    formats/PlayerDataWadTests.cpp
    formats/TplFileTests.cpp
    formats/IcoWriterTests.cpp
    game/menu/BurnDialogueScrollTests.cpp
    engine/assets/StringTableTests.cpp)

target_link_libraries(tests PRIVATE game_diagnostics gdl::engine gdl::formats gdl::game Catch2::Catch2WithMain SDL3::SDL3)
# Focused native gameplay/presentation checks without unrelated GPU or full-suite builds.
add_executable(gameplayregressions
    game/enemies/EnemiesTests.cpp game/enemies/CombatantTests.cpp engine/world/TreeModelTests.cpp
    game/screens/PostShopReturnTests.cpp
    game/screens/LevelFixturesTests.cpp
    game/world/PowerupCompanionTests.cpp game/config/GameConfigTests.cpp
    game/players/ShopTests.cpp game/players/AutoSaveTests.cpp
    game/world/SecretChallengeTests.cpp game/screens/PlaySceneChallengeTests.cpp
    game/world/LevelSoundscapeTests.cpp game/screens/PlaySceneAmbienceTests.cpp
    game/screens/PlaySceneSwitchTests.cpp game/screens/TriggerCuesTests.cpp
    engine/world/WorldAnimatorTests.cpp
    fixtures/NativeModelFixture.cpp fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
target_link_libraries(gameplayregressions PRIVATE gdl::game Catch2::Catch2WithMain)
target_include_directories(gameplayregressions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gameplayregressions PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
gdl_apply_compiler_options(gameplayregressions)
set_target_properties(gameplayregressions PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
# Focused character-form, inventory and progression regressions with native gameplay checks.
add_executable(playerformtests game/world/PojoTests.cpp game/world/PojoVoiceTests.cpp game/players/NameCheatsTests.cpp
    game/players/InventoryTests.cpp game/players/CharacterSaveTests.cpp
    game/world/PlayerFigureTests.cpp game/world/PlayerArsenalTests.cpp
    game/world/PlayerMissilesTests.cpp
    game/world/EffectTreesTests.cpp
    game/screens/PlayerAttacksTests.cpp
    game/screens/PartyPickupsTests.cpp game/screens/PlaySceneTests.cpp
    game/netplay/CharacterProfileTests.cpp
    game/world/PowerupCompanionTests.cpp fixtures/NativeModelFixture.cpp
    fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
target_link_libraries(playerformtests PRIVATE gdl::game Catch2::Catch2WithMain)
target_include_directories(playerformtests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(playerformtests PRIVATE
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
gdl_apply_compiler_options(playerformtests)
set_target_properties(playerformtests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
if(GDL_ENABLE_NETPLAY)
    target_link_libraries(tests PRIVATE gdl::engine_net)
    target_sources(tests PRIVATE engine/net/SessionTransportTests.cpp game/netplay/OnlineRunTests.cpp
        game/netplay/NativeCampaignTransportTests.cpp)
    # Fast, asset-free native adapter and room checks, separate from GPU fixtures.
    add_executable(netplaytests engine/net/SessionTransportTests.cpp
        engine/io/Sha256Tests.cpp game/netplay/AssetDigestTests.cpp game/screens/NetplayMenuTests.cpp
        game/netplay/HostedRoomTests.cpp game/netplay/OnlineSessionTests.cpp
        game/app/CommandLineTests.cpp)
    target_link_libraries(netplaytests PRIVATE gdl::game gdl::engine_net Catch2::Catch2WithMain)
    target_include_directories(netplaytests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gdl_apply_compiler_options(netplaytests)
    set_target_properties(netplaytests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    # Focused native gameplay regressions without rebuilding unrelated test suites.
    add_executable(netplayplaytests game/netplay/OnlinePartyTests.cpp
        engine/assets/AssetScopeTests.cpp
        engine/world/WorldSceneTests.cpp
        engine/world/TreeModelTests.cpp
        game/netplay/ShotIdentityTests.cpp
        game/world/PlayerArsenalTests.cpp
        game/world/PotionEmissionTests.cpp
        game/world/PotionFlightTests.cpp
        game/netplay/PlayerActionCheckpointTests.cpp
        game/netplay/PlayerTurboCheckpointTests.cpp
        game/netplay/AnimationCheckpointTests.cpp
        game/netplay/RandomCheckpointTests.cpp
        game/netplay/EnemyMissilesCheckpointTests.cpp
        game/netplay/EnemyPoolCheckpointTests.cpp
        game/netplay/GeneratorCheckpointTests.cpp
        game/netplay/PlacedEnemyCheckpointTests.cpp
        game/screens/OpponentPlacementsTests.cpp
        game/screens/PlayerAttacksCheckpointTests.cpp
        game/screens/PlayerRuntimeCheckpointTests.cpp
        game/screens/CombatRollbackCheckpointTests.cpp
        game/netplay/PlayerRuntimePacketTests.cpp
        game/netplay/CombatStatePacketTests.cpp
        game/netplay/PlayerHealthCheckpointTests.cpp
        game/screens/OpponentCombatCheckpointTests.cpp
        game/screens/OpponentLootCheckpointTests.cpp
        game/netplay/StageStatePacketTests.cpp
        game/netplay/CanonicalDigestSinkTests.cpp
        game/world/PlayerCompanionCheckpointTests.cpp
        game/screens/PartyLightingTests.cpp
        game/enemies/CombatantCheckpointTests.cpp
        game/enemies/CrittersCheckpointTests.cpp
        game/world/CombatantProjectilesCheckpointTests.cpp
        game/world/ExitPortalsCheckpointTests.cpp
        game/world/LevelTransportersCheckpointTests.cpp
        game/screens/CombatResourcesTests.cpp
        game/world/ChestsCheckpointTests.cpp
        game/world/LockedGatesCheckpointTests.cpp
        game/world/BreakablesCheckpointTests.cpp
        game/world/SafeRocksCheckpointTests.cpp
        game/world/StaticSceneryCheckpointTests.cpp
        game/world/RubbleCheckpointTests.cpp
        game/world/TrapsCheckpointTests.cpp
        game/world/FallingSceneryCheckpointTests.cpp
        game/world/DestructibleWallsCheckpointTests.cpp
        game/world/RotatorsCheckpointTests.cpp
        game/world/RotatorsLocalRewindTests.cpp
        game/world/PlacedItemsCheckpointTests.cpp
        game/world/PreparedPickupBirthTests.cpp
        game/screens/LevelFixturesCheckpointTests.cpp
        game/world/LevelWorldCheckpointTests.cpp
        game/world/TowerCameraCheckpointTests.cpp
        engine/world/WorldStateCheckpointTests.cpp
        engine/world/WorldCollapsedCollisionTests.cpp
        engine/world/WorldCollisionTests.cpp
        engine/world/ParticleFieldTests.cpp
        game/world/WorldDestructionCheckpointTests.cpp
        game/screens/EnemyFeedbackEmissionTests.cpp
        game/world/LevelTriggersCheckpointTests.cpp
        game/screens/PartyDamageTests.cpp
        game/screens/HelpMessagesTests.cpp
        game/world/PlayerArsenalCheckpointTests.cpp
        game/world/MoveStrikesCheckpointTests.cpp
        game/netplay/EffectIdentityTests.cpp
        game/netplay/PlayerPoseCheckpointTests.cpp
        game/netplay/PlayerPotionCheckpointTests.cpp
        engine/world/ParticleCheckpointTests.cpp
        game/world/EffectCheckpointTests.cpp
        game/world/WorldMotionTests.cpp
        game/screens/TurboEmissionTests.cpp
        game/netplay/TurboEffectResourcesTests.cpp
        game/netplay/TurboEffectCheckpointTests.cpp
        game/world/PlayerMissilesTests.cpp
        game/world/PlayerMissilesCheckpointTests.cpp
        game/world/EffectTreesPoolCheckpointTests.cpp
        game/combat/MeleeGeometryTests.cpp
        game/netplay/PlayerKnockbackCheckpointTests.cpp
        game/players/KnockbackTests.cpp
        game/players/PlayerActorCheckpointTests.cpp
        game/enemies/EnemyAnimatorCheckpointTests.cpp
        game/enemies/EnemiesCheckpointTests.cpp
        engine/math/Mt19937Tests.cpp
        engine/world/AmbientDimmerTests.cpp
        game/enemies/GeneratorsCheckpointTests.cpp
        game/enemies/EnemyMissilesCheckpointTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/enemies/CritterStatuesCheckpointTests.cpp
        game/screens/OpponentSimulationTests.cpp
        game/screens/OpponentResourcesTests.cpp
        game/screens/CombatProgressCheckpointTests.cpp
        game/screens/LevelOpponentsTests.cpp
        game/screens/LevelOpponentsDeathTests.cpp
        game/enemies/EnemyMissilesTests.cpp
        game/enemies/GeneratorsTests.cpp
        game/players/TurboMeterTests.cpp game/players/TurboMoveTests.cpp
        game/players/MeleeSelectionTests.cpp
        engine/world/AnimationPlayerTests.cpp engine/world/WorldAnimatorTests.cpp
        game/players/PlayerAnimatorTests.cpp
        game/netplay/MeasuredPacketTransportTests.cpp
        game/screens/OnlineCompletionTests.cpp
        engine/audio/SoundPlayerTests.cpp
        game/netplay/OnlineRunTests.cpp game/netplay/NativeCampaignTransportTests.cpp
        game/screens/TitleSceneTests.cpp
        game/netplay/InputCommandTests.cpp
        game/netplay/InputTimelineTests.cpp game/netplay/SessionInputsTests.cpp
        game/netplay/MatchSessionTests.cpp game/netplay/ShopSyncTests.cpp game/players/CursorAimTests.cpp
        game/screens/PartyMotionTests.cpp game/screens/PartyFiguresTests.cpp
        game/screens/FloorRidingTests.cpp
        game/screens/PlayerAttacksTests.cpp game/enemies/EnemiesTests.cpp
        fixtures/NativeModelFixture.cpp fixtures/ReferenceObj.cpp fixtures/NativeSoundBank.cpp)
    target_link_libraries(netplayplaytests PRIVATE game_diagnostics gdl::game Catch2::Catch2WithMain)
    target_include_directories(netplayplaytests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(netplayplaytests PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
        GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(netplayplaytests)
    set_target_properties(netplayplaytests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    # Actual gameplay, independent-process determinism observations; no replicas.
    add_executable(nativeprobe game/netplay/NativeSimulationProbe.cpp game/netplay/NativeProbeLink.cpp
        game/screens/SimulationObserver.cpp game/screens/SimulationObserverBindings.cpp)
    target_link_libraries(nativeprobe PRIVATE game_diagnostics gdl::game gdl::engine_net)
    target_include_directories(nativeprobe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nativeprobe PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(nativeprobe)
    set_target_properties(nativeprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
    target_sources(netplayplaytests PRIVATE game/screens/SimulationSeedTests.cpp
        game/screens/SimulationObserver.cpp game/screens/SimulationObserverBindings.cpp
        game/screens/NativePresentationReadinessTests.cpp
        game/netplay/HintInputCommandTests.cpp game/screens/NativeHintInputTests.cpp
        game/screens/NativeAgreementSceneTests.cpp
        game/netplay/NativeAgreementTests.cpp
        game/netplay/OnlineNativeBootstrapTests.cpp game/netplay/NativeOnlinePlayTests.cpp
        game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp
        game/screens/SimulationObservationTests.cpp game/screens/PlaySceneSimulationCameraTests.cpp
        game/netplay/LockstepInputsTests.cpp game/netplay/LockstepInputs.cpp game/netplay/MatchLockstepTests.cpp
        game/screens/NativeRollbackLifecycleTests.cpp game/screens/NativePlaySetupTests.cpp
        game/world/SpeechTimingTests.cpp)
    add_executable(simulationtests game/screens/SimulationSeedTests.cpp
        engine/io/NativeStateDigestTests.cpp
        engine/io/Sha256Tests.cpp game/netplay/CanonicalDigestSinkTests.cpp
        engine/world/WorldCollisionTests.cpp engine/world/WorldAnimatorTests.cpp
        engine/world/WorldSceneTests.cpp
        game/screens/PlayerNativeStateTests.cpp
        game/enemies/EnemyMissilesNativeDigestTests.cpp
        game/world/CombatantProjectilesNativeDigestTests.cpp
        game/world/LevelTriggersNativeDigestTests.cpp
        game/world/SecretChallengeNativeDigestTests.cpp
        game/world/NativeStateResourcesTests.cpp
        engine/assets/MessageTableTests.cpp
        engine/assets/BitmapFontTests.cpp
        engine/ui/TextPainterTests.cpp
        game/enemies/GeneratorNativeDigestTests.cpp
        game/enemies/EnemiesNativeDigestTests.cpp
        game/enemies/CombatantNativeDigestTests.cpp
        game/enemies/PlacedEnemiesNativeDigestTests.cpp
        game/enemies/OpponentOwnersNativeDigestTests.cpp
        game/enemies/CritterStatuesNativeDigestTests.cpp
        game/world/CameraMeterNativeDigestTests.cpp
        game/world/EffectTreesNativeDigestTests.cpp
        game/screens/PlayerAttackNativeDigestTests.cpp
        game/world/PlayerFigureNativeDigestTests.cpp
        game/world/LevelWorldNativeDigestTests.cpp
        game/screens/LevelFixturesNativeDigestTests.cpp
        game/world/SceneAudioNativeDigestTests.cpp
        game/screens/SceneTravelNativeDigestTests.cpp
        game/screens/SceneHudNativeDigestTests.cpp
        game/screens/PlaySceneNativeStateDigestTests.cpp
        game/netplay/EnemyPoolCheckpointTests.cpp
        game/netplay/EnemyMissilesCheckpointTests.cpp
        game/netplay/PlacedEnemyCheckpointTests.cpp
        game/enemies/EnemiesDeathTests.cpp game/enemies/EnemyAnimatorTests.cpp
        game/screens/LevelOpponentsDeathTests.cpp
        engine/world/AnimationPlayerTests.cpp
        game/screens/PlaySceneLocalRewindTests.cpp
        game/screens/PlaySceneOutputTests.cpp
        game/screens/SimulationObserverBossTests.cpp
        game/netplay/NativeRollbackSceneTests.cpp
        game/screens/NativeRollbackDriverTests.cpp
        game/screens/PlaySceneControlEqualityTests.cpp
        game/world/SceneSoundEventsTests.cpp
        game/screens/TriggerCuesTests.cpp
        game/screens/PlayerAttacksCheckpointTests.cpp
        game/world/MoveStrikesCheckpointTests.cpp
        game/screens/OpponentCombatCheckpointTests.cpp
        game/enemies/EnemyMissilesCheckpointTests.cpp
        game/enemies/PlacedEnemiesTests.cpp
        game/enemies/CritterStatuesCheckpointTests.cpp
        game/screens/BossMeterTests.cpp
        game/world/ExitPortalsCheckpointTests.cpp
        game/world/LevelTransportersCheckpointTests.cpp
        game/screens/PortalDepartureTests.cpp
        game/screens/LevelArrivalPresentationTests.cpp
        game/world/SumnerFigureTests.cpp
        game/screens/PartyHudTests.cpp
        game/screens/ChallengeHudTests.cpp
        game/screens/LevelMessagesTests.cpp
        game/screens/SumnerVisitTests.cpp
        game/menu/ScrollBoxTests.cpp
        game/menu/HintMenuTests.cpp
        game/menu/BurnDialogueScrollTests.cpp
        game/world/SumnerHintsTests.cpp
        game/screens/TowerPromotionTests.cpp
        game/screens/BossVictoryPresentationTests.cpp
        game/screens/LegendPresentationTests.cpp
        engine/world/WorldStateCheckpointTests.cpp
        game/world/PlacedItemsCheckpointTests.cpp game/world/PlacedItemsTests.cpp
        game/world/SafeRocksCheckpointTests.cpp game/world/RubbleCheckpointTests.cpp
        game/world/LevelTriggersCheckpointTests.cpp
        game/world/LevelWorldCheckpointTests.cpp game/screens/LevelFixturesCheckpointTests.cpp
        game/world/FallingSceneryCheckpointTests.cpp game/world/DestructibleWallsCheckpointTests.cpp
        game/world/WorldDestructionCheckpointTests.cpp game/world/SkorneArenaTests.cpp
        game/enemies/GeneratorsTests.cpp game/enemies/GeneratorsCheckpointTests.cpp
        game/enemies/CombatantLocalRewindTests.cpp game/enemies/CombatantCheckpointTests.cpp
        game/enemies/CrittersLocalRewindTests.cpp game/enemies/CrittersCheckpointTests.cpp
        game/enemies/BossLocalRewindTests.cpp
        engine/world/TreePoseTests.cpp
        engine/world/ParticleLocalRewindTests.cpp engine/world/ParticleSystemTests.cpp
        game/world/EffectTreesLocalRewindTests.cpp game/world/EffectTreesTests.cpp
        game/world/ItemFigureLocalRewindTests.cpp
        game/world/ChestsCheckpointTests.cpp game/world/LockedGatesCheckpointTests.cpp
        game/world/BreakablesCheckpointTests.cpp game/world/TrapsCheckpointTests.cpp
        game/world/RotatorsCheckpointTests.cpp game/world/StaticSceneryCheckpointTests.cpp
        game/world/RotatorsLocalRewindTests.cpp
        game/world/EffectTreesPoolCheckpointTests.cpp game/world/EffectCheckpointTests.cpp
        game/world/PlayerMissilesTests.cpp game/world/PlayerMissilesCheckpointTests.cpp
        game/world/CombatantProjectilesTests.cpp game/world/CombatantProjectilesCheckpointTests.cpp
        game/screens/TowerRelicsTests.cpp
        game/world/TowerCameraTests.cpp game/world/TowerCameraCheckpointTests.cpp
        game/world/PowerupCompanionTests.cpp game/world/PlayerFamiliarTests.cpp
        game/netplay/LocalStateHistoryTests.cpp
        game/netplay/RollbackExchangeTests.cpp
        game/screens/NativeSoundIdentityTests.cpp
        game/netplay/RollbackInputsTests.cpp
        game/netplay/FrameEventLedgerTests.cpp
        game/screens/PlayerLocalRewindTests.cpp
        game/screens/PlayerSimulationEqualityTests.cpp
        game/screens/PlayerRuntimeCheckpointTests.cpp
        game/world/PlayerCompanionCheckpointTests.cpp game/world/PlayerFigureTests.cpp
        game/screens/PartyMotionTests.cpp game/screens/PartyFiguresTests.cpp
        game/players/PlayerActorCheckpointTests.cpp
        game/netplay/NativeProbeTimingTests.cpp
        game/enemies/EnemiesCheckpointTests.cpp engine/world/TextureAnimatorTests.cpp
        game/screens/SimulationObserver.cpp game/screens/SimulationObserverBindings.cpp
        game/screens/NativePresentationReadinessTests.cpp
        engine/app/ApplicationTimingTests.cpp
        game/netplay/InputCommandTests.cpp
        game/netplay/HintInputCommandTests.cpp game/screens/NativeHintInputTests.cpp
        game/screens/NativeAgreementSceneTests.cpp
        game/netplay/NativeAgreementTests.cpp
        game/netplay/OnlineSessionTests.cpp game/netplay/OnlineNativeBootstrapTests.cpp
        game/netplay/NativeOnlinePlayTests.cpp game/netplay/OnlineRunTests.cpp
        game/netplay/NativeCampaignTransportTests.cpp
        game/netplay/OnlinePartyTests.cpp
        game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp engine/assets/StringTableTests.cpp
        game/screens/SimulationObservationTests.cpp game/screens/PlaySceneSimulationCameraTests.cpp
        game/netplay/LockstepInputsTests.cpp game/netplay/LockstepInputs.cpp game/netplay/MatchLockstepTests.cpp
        game/netplay/MatchSessionTests.cpp game/netplay/ShopSyncTests.cpp game/players/ShopTests.cpp
        game/screens/AfterLevelSceneTests.cpp
        game/screens/NativeRollbackLifecycleTests.cpp game/screens/NativePlaySetupTests.cpp
        game/world/SpeechTimingTests.cpp
        game/world/VoiceQueueTests.cpp game/world/LevelSoundscapeTests.cpp
        game/screens/BossSequenceTests.cpp fixtures/NativeSoundBank.cpp fixtures/NativeModelFixture.cpp
        fixtures/ReferenceObj.cpp game/screens/PlayerAttacksTests.cpp game/screens/PartyPickupsTests.cpp
        game/screens/PlaySceneTests.cpp engine/math/Mt19937Tests.cpp)
    target_link_libraries(simulationtests PRIVATE game_diagnostics gdl::game Catch2::Catch2WithMain)
    target_include_directories(simulationtests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(simulationtests PRIVATE
        GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}" GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")
    gdl_apply_compiler_options(simulationtests)
    set_target_properties(simulationtests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
endif()
target_sources(tests PRIVATE engine/io/Sha256Tests.cpp game/netplay/AssetDigestTests.cpp
    game/screens/PlaySceneLocalRewindTests.cpp
    game/screens/PlaySceneOutputTests.cpp
    game/screens/SimulationObserverBossTests.cpp
    game/netplay/NativeRollbackSceneTests.cpp
    game/screens/NativeRollbackDriverTests.cpp
    game/screens/PlaySceneControlEqualityTests.cpp
    game/world/SceneSoundEventsTests.cpp
    game/screens/SimulationObserver.cpp game/screens/SimulationObserverBindings.cpp
    game/screens/NativePresentationReadinessTests.cpp
    game/netplay/HintInputCommandTests.cpp game/screens/NativeHintInputTests.cpp
    game/screens/NetplayMenuTests.cpp
    game/screens/SimulationSeedTests.cpp game/screens/SimulationObservationTests.cpp
    game/screens/PlaySceneSimulationCameraTests.cpp game/screens/NativePlaySetupTests.cpp
    game/screens/NativeRollbackLifecycleTests.cpp game/netplay/LockstepInputsTests.cpp game/netplay/LockstepInputs.cpp
    game/screens/NativeAgreementSceneTests.cpp
    game/netplay/MatchLockstepTests.cpp game/netplay/NativeAgreementTests.cpp
    game/netplay/OnlineNativeBootstrapTests.cpp game/netplay/NativeOnlinePlayTests.cpp
    game/app/NativeOnlineIdentityTests.cpp game/app/NumericWitnessTests.cpp game/world/SpeechTimingTests.cpp)
target_link_libraries(tests PRIVATE Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tests PRIVATE
    VK_NO_PROTOTYPES
    GDL_TEST_ASSET_DIR="${GDL_ASSET_DIR}"
    GDL_TEST_UNPACKED_DIR="${GDL_UNPACKED_DIR}"
    GDL_TEST_DATA_DIR="${GDL_DATA_DIR}")

gdl_apply_compiler_options(tests)

# Shares bin/ with the game so the compiled shaders and dependency DLLs are found.
set_target_properties(tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    FOLDER "tests")
add_dependencies(tests gauntlet_shaders)

# Unit tests are discovered individually; the GPU integration tests run as one
# CTest entry labelled "gpu" so machines without a display can skip them
# with `ctest -LE gpu`. Tests tagged [assets] skip themselves when the game
# data is not installed.
set(GDL_TEST_DISCOVERY_FILTER "~[gpu]" CACHE STRING
    "Catch2 discovery filter (public release runners exclude private asset tests)")
catch_discover_tests(tests TEST_SPEC "${GDL_TEST_DISCOVERY_FILTER}" TEST_PREFIX "unit.")
add_test(NAME gpu.integration COMMAND tests "[gpu]")
set_tests_properties(gpu.integration PROPERTIES LABELS gpu)
