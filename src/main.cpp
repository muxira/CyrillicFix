// CyrillicFix - proper Cyrillic text rendering for Beat Saber (Quest, 1.40.8_7379)
//
// How it works:
//   1. At runtime we create a real TextMeshPro font asset (dynamic SDF atlas) from a TTF file:
//      either a user-provided one (font.ttf in the mod data folder) or an Android system font
//      (Roboto / Noto Sans), which always contains Cyrillic.
//   2. We hook TMP_Text::GetTextElement, the place where TMP looks up a glyph for every character.
//      For Cyrillic code points we ALWAYS return the glyph from our font, even if one of the game's
//      fonts (or its fallbacks) "found" something for them - that is exactly what produces '?' and '.'
//      instead of letters. Every other character keeps the game's behaviour; our font is only used
//      for them when nothing at all was found.

#include "main.hpp"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "TMPro/AtlasPopulationMode.hpp"
#include "TMPro/FontStyles.hpp"
#include "TMPro/FontWeight.hpp"
#include "TMPro/ShaderUtilities.hpp"
#include "TMPro/TMP_Asset.hpp"
#include "TMPro/TMP_Character.hpp"
#include "TMPro/TMP_FontAsset.hpp"
#include "TMPro/TMP_FontAssetUtilities.hpp"
#include "TMPro/TMP_Settings.hpp"
#include "TMPro/TMP_Text.hpp"
#include "TMPro/TMP_TextElement.hpp"
#include "UnityEngine/Font.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Shader.hpp"
#include "UnityEngine/TextCore/FaceInfo.hpp"
#include "UnityEngine/TextCore/LowLevel/GlyphRenderMode.hpp"
#include "UnityEngine/Texture2D.hpp"

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

// Folder where the user can drop a custom font with Cyrillic support (font.ttf / font.otf).
static constexpr char const* MOD_DATA_DIR = "/sdcard/ModData/com.beatgames.beatsaber/Mods/CyrillicFix/";

// Dynamic atlas parameters
static constexpr int SAMPLING_POINT_SIZE = 72;
static constexpr int ATLAS_PADDING = 7;
static constexpr int ATLAS_SIZE = 2048;

static TMPro::TMP_FontAsset* cyrFont = nullptr;
static bool initAttempted = false;

using namespace TMPro;
using namespace UnityEngine;

// ---------------------------------------------------------------------------
// Code points that are always rendered with our font
// ---------------------------------------------------------------------------
static inline bool IsForcedCodepoint(uint32_t c) {
    return (c >= 0x0400 && c <= 0x052F)     // Cyrillic + Cyrillic Supplement (ru, uk, be, sr, kk, ...)
        || (c >= 0x1C80 && c <= 0x1C8F)     // Cyrillic Extended-C
        || (c >= 0x2DE0 && c <= 0x2DFF)     // Cyrillic Extended-A
        || (c >= 0xA640 && c <= 0xA69F)     // Cyrillic Extended-B
        || c == 0x2116;                     // Numero sign
}

static std::string ToStd(StringW s) {
    return s ? static_cast<std::string>(s) : std::string("<null>");
}

// ---------------------------------------------------------------------------
// Font loading
// ---------------------------------------------------------------------------
static std::vector<std::string> CandidateFontPaths() {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;
    auto push = [&](std::string const& p) {
        if (seen.insert(p).second)
            paths.push_back(p);
    };

    // 1) user font has the highest priority
    push(std::string(MOD_DATA_DIR) + "font.ttf");
    push(std::string(MOD_DATA_DIR) + "font.otf");

    // 2) well-known Android / Horizon OS system fonts with Cyrillic
    push("/system/fonts/Roboto-Regular.ttf");
    push("/system/fonts/RobotoStatic-Regular.ttf");
    push("/system/fonts/NotoSans-Regular.ttf");
    push("/system/fonts/DroidSans.ttf");

    // 3) anything Unity reports as an OS font that looks like Roboto / NotoSans / DroidSans
    try {
        auto osFonts = Font::GetPathsToOSFonts();
        if (osFonts) {
            for (auto p : osFonts) {
                auto s = ToStd(p);
                if (s.find("Roboto") != std::string::npos && s.find("Mono") == std::string::npos &&
                    s.find("Condensed") == std::string::npos && s.find("Serif") == std::string::npos)
                    push(s);
            }
            for (auto p : osFonts) {
                auto s = ToStd(p);
                if (s.find("NotoSans-") != std::string::npos || s.find("DroidSans") != std::string::npos)
                    push(s);
            }
        }
    } catch (std::exception const& e) {
        logger.warn("GetPathsToOSFonts failed: {}", e.what());
    }
    return paths;
}

// Equivalent of `new Font(path)`: the constructor itself was stripped from the game build,
// but the Internal_CreateFontFromPath icall is still there.
static Font* CreateUnityFontFromPath(std::string const& path) {
    auto font = reinterpret_cast<Font*>(il2cpp_functions::object_new(classof(Font*)));
    if (!font)
        return nullptr;
    Font::Internal_CreateFontFromPath(font, path);
    return font;
}

// When creating an asset TMP looks for the "TextMeshPro/Mobile/Distance Field" shader.
// If it is not included in the build, use the shader of the game's own fonts instead.
static void EnsureSdfShader() {
    try {
        if (ShaderUtilities::get_ShaderRef_MobileSDF())
            return;
    } catch (...) {}

    for (auto fa : Resources::FindObjectsOfTypeAll<TMP_FontAsset*>()) {
        if (!fa || !fa->material)
            continue;
        auto shader = fa->material->shader;
        if (shader) {
            logger.info("TMP Mobile SDF shader not found, using shader '{}' from font '{}'", ToStd(shader->name), ToStd(fa->name));
            ShaderUtilities::setStaticF_k_ShaderRef_MobileSDF(shader);
            return;
        }
    }
    logger.warn("No SDF shader found at all!");
}

static void KeepAlive(Object* obj) {
    if (obj)
        obj->set_hideFlags(HideFlags::DontUnloadUnusedAsset);
}

static TMP_FontAsset* TryMakeFontAsset(Font* font, std::string const& label) {
    if (!font)
        return nullptr;
    try {
        auto asset = TMP_FontAsset::CreateFontAsset(
            font,
            SAMPLING_POINT_SIZE,
            ATLAS_PADDING,
            TextCore::LowLevel::GlyphRenderMode::SDFAA,
            ATLAS_SIZE,
            ATLAS_SIZE,
            AtlasPopulationMode::Dynamic,
            true
        );
        if (!asset) {
            logger.warn("CreateFontAsset returned null for {}", label);
            return nullptr;
        }
        TMP_FontAsset* fa = asset.ptr();

        // make sure the font really contains Cyrillic and that glyphs get rendered into the atlas
        if (!fa->HasCharacter_Internal(0x042F /* CYRILLIC CAPITAL LETTER YA */, false, true) || !fa->HasCharacter_Internal(0x0436 /* CYRILLIC SMALL LETTER ZHE */, false, true)) {
            logger.warn("Font {} loaded but has no Cyrillic glyphs, skipping", label);
            return nullptr;
        }

        fa->set_name("CyrillicFix SDF");
        KeepAlive(font);
        KeepAlive(fa);
        KeepAlive(fa->material);
        if (fa->m_AtlasTextures) {
            for (auto tex : fa->m_AtlasTextures)
                KeepAlive(tex);
        }
        return fa;
    } catch (std::exception const& e) {
        logger.warn("CreateFontAsset threw for {}: {}", label, e.what());
    } catch (...) {
        logger.warn("CreateFontAsset threw unknown exception for {}", label);
    }
    return nullptr;
}

static void Prewarm(TMP_FontAsset* fa) {
    std::u16string chars;
    for (char16_t c = 0x0400; c <= 0x045F; c++)  // base set: ru/uk/be/sr/mk
        chars.push_back(c);
    for (char16_t c = 0x0490; c <= 0x04FF; c++)  // Ukrainian Ghe with upturn, Kazakh, Tatar, etc.
        chars.push_back(c);
    chars.push_back(0x2116);

    try {
        fa->TryAddCharacters(StringW(chars), false);
    } catch (std::exception const& e) {
        logger.warn("Prewarm failed: {}", e.what());
    }
    if (fa->m_AtlasTextures) {
        for (auto tex : fa->m_AtlasTextures)
            KeepAlive(tex);
    }
}

// Log the game's fonts and whether they contain Cyrillic - helps diagnosing rendering issues.
static void LogDiagnostics() {
    try {
        auto settings = TMP_Settings::get_instance();
        if (settings)
            logger.info(
                "TMP_Settings: missingGlyphCharacter={} matchMaterialPreset={}",
                settings->m_missingGlyphCharacter,
                settings->m_matchMaterialPreset
            );

        for (auto fa : Resources::FindObjectsOfTypeAll<TMP_FontAsset*>()) {
            if (!fa || fa == cyrFont)
                continue;
            bool hasYa = fa->HasCharacter_Internal(0x042F, false, false);
            bool hasI = fa->HasCharacter_Internal(0x0456, false, false);
            std::string fallbacks;
            if (fa->m_FallbackFontAssetTable) {
                for (auto fb : ListW<UnityW<TMP_FontAsset>>(fa->m_FallbackFontAssetTable)) {
                    fallbacks += fb ? ToStd(fb->name) : "<null>";
                    fallbacks += ", ";
                }
            }
            logger.info(
                "Font '{}': mode={} has U+042F={} has U+0456={} fallbacks=[{}]",
                ToStd(fa->name),
                (int) fa->atlasPopulationMode.value__,
                hasYa,
                hasI,
                fallbacks
            );
        }
    } catch (std::exception const& e) {
        logger.warn("Diagnostics failed: {}", e.what());
    }
}

static void InitFont() {
    if (initAttempted)
        return;
    initAttempted = true;

    std::error_code ec;
    std::filesystem::create_directories(MOD_DATA_DIR, ec);

    EnsureSdfShader();

    for (auto const& path : CandidateFontPaths()) {
        if (!std::filesystem::exists(path, ec))
            continue;
        logger.info("Trying font file {}", path);
        Font* font = nullptr;
        try {
            font = CreateUnityFontFromPath(path);
        } catch (std::exception const& e) {
            logger.warn("Font load failed for {}: {}", path, e.what());
            continue;
        }
        if (auto fa = TryMakeFontAsset(font, path)) {
            cyrFont = fa;
            logger.info("Using font {}", path);
            break;
        }
    }

    // last resort: dynamic OS font by name
    if (!cyrFont) {
        for (auto name : {"Roboto", "Noto Sans", "Droid Sans", "sans-serif"}) {
            try {
                auto font = Font::CreateDynamicFontFromOSFont(name, SAMPLING_POINT_SIZE);
                if (auto fa = TryMakeFontAsset(font.ptr(), name)) {
                    cyrFont = fa;
                    logger.info("Using OS font by name '{}'", name);
                    break;
                }
            } catch (std::exception const& e) {
                logger.warn("CreateDynamicFontFromOSFont('{}') failed: {}", name, e.what());
            }
        }
    }

    LogDiagnostics();

    if (!cyrFont) {
        logger.error("Could not create any Cyrillic font asset, mod is inactive");
        return;
    }

    Prewarm(cyrFont);
    logger.info("Cyrillic font ready (atlas textures: {})", cyrFont->m_AtlasTextures ? cyrFont->m_AtlasTextures.size() : 0);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
MAKE_HOOK_MATCH(
    MainFlowCoordinator_DidActivate,
    &GlobalNamespace::MainFlowCoordinator::DidActivate,
    void,
    GlobalNamespace::MainFlowCoordinator* self,
    bool firstActivation,
    bool addedToHierarchy,
    bool screenSystemEnabling
) {
    if (!initAttempted)
        InitFont();
    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
}

static std::unordered_set<uint32_t> loggedMissing;

MAKE_HOOK_MATCH(
    TMP_Text_GetTextElement,
    &TMP_Text::GetTextElement,
    TMP_TextElement*,
    TMP_Text* self,
    uint32_t unicode,
    TMP_FontAsset* fontAsset,
    FontStyles fontStyle,
    FontWeight fontWeight,
    ByRef<bool> isUsingAlternativeTypeface
) {
    if (cyrFont && IsForcedCodepoint(unicode)) {
        auto character = TMP_FontAssetUtilities::GetCharacterFromFontAsset(unicode, cyrFont, false, fontStyle, fontWeight, isUsingAlternativeTypeface);
        if (character)
            return character;
        if (loggedMissing.insert(unicode).second)
            logger.warn("U+{:04X} not present in Cyrillic font, falling back to game lookup", unicode);
    }

    auto result = TMP_Text_GetTextElement(self, unicode, fontAsset, fontStyle, fontWeight, isUsingAlternativeTypeface);

    // nothing found at all - try our font (Greek, extended Latin, etc.)
    if (!result && cyrFont) {
        auto character = TMP_FontAssetUtilities::GetCharacterFromFontAsset(unicode, cyrFont, false, fontStyle, fontWeight, isUsingAlternativeTypeface);
        if (character)
            return character;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Scotland2 entry points
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default"))) void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;
    modInfo.assign(*info);

    Paper::Logger::RegisterFileContextId(MOD_ID);
    logger.info("Completed setup!");
}

extern "C" __attribute__((visibility("default"))) void late_load() {
    il2cpp_functions::Init();

    logger.info("Installing hooks...");
    INSTALL_HOOK(logger, MainFlowCoordinator_DidActivate);
    INSTALL_HOOK(logger, TMP_Text_GetTextElement);
    logger.info("Installed all hooks!");
}
