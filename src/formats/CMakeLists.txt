# Native asset readers shared by the runtime and optional inspection/export tools.
add_library(formats STATIC
    LegendsMap.h
    LegendsMap.cpp
    LegendsDreamcastMap.cpp
    LegendsDreamcastScene.cpp
    LegendsN64Scene.cpp
    LegendsN64Camera.cpp
    LegendsN64Tower.cpp
    LegendsN64Progress.cpp
    LegendsN64Gates.cpp
    LegendsN64Traps.cpp
    LegendsN64Portal.cpp
    LegendsN64Text.cpp
    LegendsN64Switches.cpp
    LegendsN64Morph.cpp
    LegendsN64Morph.h
    LegendsSceneMotion.cpp
    LegendsDreamcastTexture.h
    LegendsDreamcastTexture.cpp
    LegendsN64Texture.h
    LegendsN64Texture.cpp
    ModelArchive.h
    ModelArchive.cpp
    GcTexture.h
    GcTexture.cpp
    FontFile.h
    FontFile.cpp
    TextRom.h
    TextRom.cpp
    JsonWriter.h
    JsonWriter.cpp
    AudioRom.h
    AudioRom.cpp
    SoundBank.h
    SoundBank.cpp
    WavWriter.h
    WavWriter.cpp
    GeometryStream.h
    GeometryStream.cpp
    KeyframeTrack.h
    KeyframeTrack.cpp
    AnimationTree.h
    AnimationTree.cpp
    ObjWriter.h
    ObjWriter.cpp
    PlayerDataWad.h
    PlayerDataWad.cpp
    CritterWad.h
    CritterWad.cpp
    WadDirectory.h
    WadDirectory.cpp
    ShopWad.h
    ShopWad.cpp
    WorldDataWad.h
    WorldDataWad.cpp
    WorldFile.h
    WorldFile.cpp
    ParticleTemplate.h
    ParticleTemplate.cpp
    TplFile.h
    TplFile.cpp
    IcoWriter.h
    IcoWriter.cpp)

add_library(gdl::formats ALIAS formats)

target_link_libraries(formats PUBLIC gdl::engine_base)
gdl_apply_compiler_options(formats)
set_target_properties(formats PROPERTIES FOLDER "formats")
