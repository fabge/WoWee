// A map id's friendly name, and the directory its terrain lives in.
//
// Two switch statements over map ids, lifted out of WorldLoader on 2026-08-27.
// They are read by the loading screen, by an item tooltip naming where a
// hearthstone points, and by the terrain loader looking for a WDT - which put a
// pair of tables on the src/ui -> src/core edge in the library graph, and they
// are not the composition root's.
//
// Kept as switches rather than a map: the compiler builds the jump table, there
// is nothing to initialise before main, and a missing id is a compile-time hole
// rather than a lookup that silently returns nothing.

#include "game/map_names.hpp"

namespace wowee {
namespace game {

const char* mapDisplayName(uint32_t mapId) {
    // Friendly display names for the loading screen
    switch (mapId) {
        case 0: return "Eastern Kingdoms";
        case 1: return "Kalimdor";
        case 13: return "Test";
        case 34: return "The Stockade";
        case 169: return "Emerald Dream";
        case 530: return "Outland";
        case 571: return "Northrend";
        default: return nullptr;
    }
}

const char* mapWdtName(uint32_t mapId) {
    // Fallback when Map.dbc is unavailable. Names must match WDT directory names
    // (case-insensitive - AssetManager lowercases all paths).
    switch (mapId) {
        // Continents
        case 0: return "Azeroth";
        case 1: return "Kalimdor";
        case 530: return "Expansion01";
        case 571: return "Northrend";
        // Classic dungeons/raids
        case 30: return "PVPZone01";
        case 33: return "Shadowfang";
        case 34: return "StormwindJail";
        case 36: return "DeadminesInstance";
        case 43: return "WailingCaverns";
        case 47: return "RazserfenKraulInstance";
        case 48: return "Blackfathom";
        case 70: return "Uldaman";
        case 90: return "GnomeragonInstance";
        case 109: return "SunkenTemple";
        case 129: return "RazorfenDowns";
        case 189: return "MonasteryInstances";
        case 209: return "TanarisInstance";
        case 229: return "BlackRockSpire";
        case 230: return "BlackrockDepths";
        case 249: return "OnyxiaLairInstance";
        case 289: return "ScholomanceInstance";
        case 309: return "Zul'Gurub";
        case 329: return "Stratholme";
        case 349: return "Mauradon";
        case 369: return "DeeprunTram";
        case 389: return "OrgrimmarInstance";
        case 409: return "MoltenCore";
        case 429: return "DireMaul";
        case 469: return "BlackwingLair";
        case 489: return "PVPZone03";
        case 509: return "AhnQiraj";
        case 529: return "PVPZone04";
        case 531: return "AhnQirajTemple";
        case 533: return "Stratholme Raid";
        // TBC
        case 532: return "Karazahn";
        case 534: return "HyjalPast";
        case 540: return "HellfireMilitary";
        case 542: return "HellfireDemon";
        case 543: return "HellfireRampart";
        case 544: return "HellfireRaid";
        case 545: return "CoilfangPumping";
        case 546: return "CoilfangMarsh";
        case 547: return "CoilfangDraenei";
        case 548: return "CoilfangRaid";
        case 550: return "TempestKeepRaid";
        case 552: return "TempestKeepArcane";
        case 553: return "TempestKeepAtrium";
        case 554: return "TempestKeepFactory";
        case 555: return "AuchindounShadow";
        case 556: return "AuchindounDraenei";
        case 557: return "AuchindounEthereal";
        case 558: return "AuchindounDemon";
        case 560: return "HillsbradPast";
        case 564: return "BlackTemple";
        case 565: return "GruulsLair";
        case 566: return "PVPZone05";
        case 568: return "ZulAman";
        case 580: return "SunwellPlateau";
        case 585: return "Sunwell5ManFix";
        // WotLK
        case 574: return "Valgarde70";
        case 575: return "UtgardePinnacle";
        case 576: return "Nexus70";
        case 578: return "Nexus80";
        case 595: return "StratholmeCOT";
        case 599: return "Ulduar70";
        case 600: return "Ulduar80";
        case 601: return "DrakTheronKeep";
        case 602: return "GunDrak";
        case 603: return "UlduarRaid";
        case 608: return "DalaranPrison";
        case 615: return "ChamberOfAspectsBlack";
        case 617: return "DeathKnightStart";
        case 619: return "Azjol_Uppercity";
        case 624: return "WintergraspRaid";
        case 631: return "IcecrownCitadel";
        case 632: return "IcecrownCitadel5Man";
        case 649: return "ArgentTournamentRaid";
        case 650: return "ArgentTournamentDungeon";
        case 658: return "QuarryOfTears";
        case 668: return "HallsOfReflection";
        case 724: return "ChamberOfAspectsRed";
        default: return "";
    }
}

} // namespace game
} // namespace wowee
