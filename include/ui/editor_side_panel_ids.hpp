#pragma once

#include "ui/editor_side_panel.hpp"

#include <string>
#include <windows.h>

namespace omsi::ui::editor_side_panel_ids {

constexpr int IDC_TAB_STRIP = 3000;
constexpr int IDC_STATUS = 3001;
constexpr int IDC_BTN_LOAD_MAP = 3002;
constexpr int IDC_BTN_SHORTCUTS = 3003;
constexpr int IDC_BTN_TOOLBAR_MENU = 3004;
constexpr int IDC_BTN_OPEN_TILE = 3005;
constexpr int IDC_BTN_ENTRYPOINTS = 3006;
constexpr int IDC_BTN_OPTIONS = 3007;
constexpr int IDC_O_CATALOG = 3100;
constexpr int IDC_O_INST = 3101;
constexpr int IDC_O_LOAD = 3102;
constexpr int IDC_O_LOADDIR = 3103;
constexpr int IDC_O_APPLY = 3104;
constexpr int IDC_O_STRINGS = 3105;
constexpr int IDC_O_EDITFILE = 3106;
constexpr int IDC_O_FOLDER = 3107;
constexpr int IDC_O_RELOAD = 3108;
constexpr int IDC_O_TILE_SEARCH = 3116;
constexpr int IDC_O_INST_SEARCH = 3117;
constexpr int IDC_O_OPENMAP = 3118;
constexpr int IDC_O_CAT_SEARCH = 3121;
constexpr int IDC_O_SLOT0 = 3150; // 3150..3165 (16 slots)
constexpr int IDC_O_X = 3110;
constexpr int IDC_O_Y = 3111;
constexpr int IDC_O_Z = 3112;
constexpr int IDC_O_ROT = 3113;
constexpr int IDC_O_PITCH = 3114;
constexpr int IDC_O_BANK = 3115;
constexpr int IDC_S_CATALOG = 3200;
constexpr int IDC_S_INST = 3201;
constexpr int IDC_S_LOAD = 3202;
constexpr int IDC_S_CREATE = 3273;
constexpr int IDC_S_APPLY = 3203;
constexpr int IDC_S_EDITFILE = 3204;
constexpr int IDC_S_FOLDER = 3205;
constexpr int IDC_S_RELOAD = 3206;
constexpr int IDC_S_TILE_SEARCH = 3207;
constexpr int IDC_S_INST_SEARCH = 3208;
constexpr int IDC_S_OPENMAP = 3209;
constexpr int IDC_S_CAT_SEARCH = 3226;
constexpr int IDC_S_Z = 3210;
constexpr int IDC_S_ROT = 3211;
constexpr int IDC_S_SKEW0 = 3212;
constexpr int IDC_S_SKEW1 = 3213;
constexpr int IDC_S_LEN = 3214;
constexpr int IDC_S_RAD = 3215;
constexpr int IDC_S_G0 = 3216;
constexpr int IDC_S_G1 = 3217;
constexpr int IDC_S_G0_EDIT = 3240;
constexpr int IDC_S_G1_EDIT = 3241;
constexpr int IDC_S_G0_UP = 3242;
constexpr int IDC_S_G0_DN = 3243;
constexpr int IDC_S_G1_UP = 3244;
constexpr int IDC_S_G1_DN = 3245;
constexpr int IDC_S_DH = 3218;
constexpr int IDC_S_DH_CHK = 3219;
constexpr int IDC_S_CANT0 = 3220;
constexpr int IDC_S_CANT1 = 3221;
constexpr int IDC_S_MIRROR = 3222;
constexpr int IDC_S_ENDS = 3230;
constexpr int IDC_S_PATH_ENDS = 3231;
constexpr int IDC_S_PATH_RAILS = 3232;
constexpr int IDC_S_AUTOFILL = 3233;
constexpr int IDC_T_UVG = 3300;
constexpr int IDC_T_SPEED = 3301;
constexpr int IDC_T_APPLY = 3302;
constexpr int IDC_T_RADIO0 = 3310; // + rule index

constexpr int IDC_T_CONVERT = 3400;
constexpr int IDC_T_APIKEY = 3401;
constexpr int IDC_T_REFLAT = 3402;
constexpr int IDC_T_REFLON = 3403;
constexpr int IDC_T_DTM = 3405;
constexpr int IDC_T_SPAWN_TERR = 3423;
constexpr int IDC_T_SPAWN_MESH = 3424;
constexpr int IDC_T_SPAWN_ALIGN = 3426;
constexpr int IDC_T_CREATE_TILE = 3413;
constexpr int IDC_T_DELETE_TILE = 3414;
constexpr int IDC_T_AERIAL_VIS = 3406;
constexpr int IDC_TE_BRUSH = 3500;
constexpr int IDC_TE_SPEED = 3501;
constexpr int IDC_TE_SETH = 3502;
constexpr int IDC_TE_HIGHER = 3503;
constexpr int IDC_TE_LOWER = 3504;
constexpr int IDC_TE_SET = 3505;
constexpr int IDC_TE_PAINT = 3506;
constexpr int IDC_TE_RUBBER = 3507;
constexpr int IDC_TE_LAYERS = 3508;
constexpr int IDC_TE_LAYER_NEW = 3509;
constexpr int IDC_TE_LAYER_EDIT = 3510;
constexpr int IDC_TE_LAYER_DEL = 3511;
constexpr int IDC_T_AERIAL_RES = 3407;
constexpr int IDC_T_AERIAL_DNL = 3408;
constexpr int IDC_T_LINK_AERIAL = 3409;
constexpr int IDC_T_SATPROV = 3410;
constexpr int IDC_T_DTMPROV = 3411;
constexpr int IDC_T_CUSTPRE = 3412;
constexpr int IDC_T_AERIAL_MISS = 3415;
constexpr int IDC_T_AERIAL_ALL = 3416;
constexpr int IDC_T_LIGHTMAP = 3417;
constexpr int IDC_T_WHOLE_MAP = 3418;
constexpr int IDC_T_CHRONO_LIST = 3419;
constexpr int IDC_T_CHRONO_PLUS = 3420;
constexpr int IDC_T_CHRONO_MINUS = 3421;
constexpr int IDC_T_CHRONO_REM = 3422;
constexpr int IDC_T_BAKE_PARTS = 3430;
constexpr int IDC_T_BAKE_O3D = 3431;
constexpr int IDC_T_BAKE_REPLACE = 3432;
constexpr int IDC_T_BAKE_FBX = 3433;
constexpr int IDC_T_BAKE_STATUS = 3434;
constexpr int IDC_T_AERIAL_DEL = 3425;
constexpr int IDC_O_ATTACH = 3120;
constexpr int IDC_O_ATTACH_SLI = 3126;
constexpr int IDC_O_TANGENTIAL = 3127;
constexpr int IDC_O_ALIGN_TERR = 3128;
constexpr int IDC_O_ATT_PNT = 3121;
constexpr int IDC_O_PARENT = 3122;
constexpr int IDC_O_PARENT_ID = 3123;
constexpr int IDC_O_SPAWN_TERR = 3124;
constexpr int IDC_O_SPAWN_MESH = 3125;
constexpr int IDC_O_SPAWN_ALIGN = 3129;
constexpr int IDC_S_SPLIT = 3240;
constexpr int IDC_S_COMPLETE = 3241;
constexpr int IDC_S_ALIGN_START = 3242;
constexpr int IDC_S_ALIGN_END = 3243;
constexpr int IDC_S_MAX_RAD = 3244;
constexpr int IDC_S_SPAWN_TERR = 3245;
constexpr int IDC_S_SPAWN_MESH = 3246;
constexpr int IDC_S_SPAWN_ALIGN = 3247;
constexpr int IDC_S_ROT_TRK = 3250;
constexpr int IDC_S_LEN_TRK = 3251;
constexpr int IDC_S_RAD_TRK = 3252;
constexpr int IDC_S_SKEW0_TRK = 3253;
constexpr int IDC_S_SKEW1_TRK = 3254;
constexpr int IDC_S_CANT0_TRK = 3255;
constexpr int IDC_S_CANT1_TRK = 3256;
constexpr int IDC_S_Z_UP = 3257;
constexpr int IDC_S_Z_DN = 3258;
constexpr int IDC_S_ROT_UP = 3259;
constexpr int IDC_S_ROT_DN = 3260;
constexpr int IDC_S_SKEW0_UP = 3261;
constexpr int IDC_S_SKEW0_DN = 3262;
constexpr int IDC_S_SKEW1_UP = 3263;
constexpr int IDC_S_SKEW1_DN = 3264;
constexpr int IDC_S_LEN_UP = 3265;
constexpr int IDC_S_LEN_DN = 3266;
constexpr int IDC_S_RAD_UP = 3267;
constexpr int IDC_S_RAD_DN = 3268;
constexpr int IDC_S_CANT0_UP = 3269;
constexpr int IDC_S_CANT0_DN = 3270;
constexpr int IDC_S_CANT1_UP = 3271;
constexpr int IDC_S_CANT1_DN = 3272;
constexpr int IDC_TT_TRACKS = 3600;
constexpr int IDC_TT_TRIPS = 3601;
constexpr int IDC_TT_STATION = 3602;
constexpr int IDC_TT_SIGNAL = 3603;
constexpr int IDC_TT_REFRESH = 3604;
constexpr int IDC_TT_OPEN = 3605;
constexpr int IDC_TT_MODE0 = 3610;
constexpr int IDC_TT_MODE1 = 3611;
constexpr int IDC_TT_MODE2 = 3612;
constexpr int IDC_TT_TRACK_CREATE = 3613;
constexpr int IDC_TT_TRACK_DEL = 3614;
constexpr int IDC_TT_TRACK_COMBO = 3615;
constexpr int IDC_TT_TRIP_CREATE1 = 3616;
constexpr int IDC_TT_TRIP_CREATE2 = 3617;
constexpr int IDC_TT_TRIP_DEL_STN = 3618;
constexpr int IDC_TT_TRIP_PROFILES = 3619;
constexpr int IDC_TT_TRIP_ASSIST = 3620;
constexpr int IDC_TT_TRIP_ADD = 3621;
constexpr int IDC_TT_TRIP_TYPE2 = 3622;
constexpr int IDC_TT_TERMINUS = 3623;
constexpr int IDC_TT_LINE = 3624;
constexpr int IDC_TT_TRAIN_REV = 3625;
constexpr int IDC_TT_COPY_TRACK = 3626;
constexpr int IDC_TT_COPY_TRIP = 3627;
constexpr int IDC_TT_COPY_BOTH = 3628;
constexpr int IDC_TT_EDIT_TT = 3629;
constexpr int IDC_TT_FIND_ERR = 3630;
constexpr int IDC_TT_TRIP_COMBO = 3631;
constexpr int IDC_TT_LENGTH = 3632;
// Station Links tab
constexpr int IDC_SL_FILTER = 3640;
constexpr int IDC_SL_NEW = 3641;
constexpr int IDC_SL_SEARCH_ERR = 3642;
constexpr int IDC_SL_DELETE = 3643;
constexpr int IDC_SL_MODE_START = 3644;
constexpr int IDC_SL_MODE_END = 3645;
constexpr int IDC_SL_MODE_PATH = 3646;
constexpr int IDC_SL_LINKS = 3647;
constexpr int IDC_SL_SEGMENTS = 3648;
constexpr int IDC_SL_LOG = 3649;

constexpr int kTabCount = 9;
constexpr int kContentX = EditorSidePanel::kTabStripW + 4;

extern const wchar_t* kTabNames[kTabCount];

// Group labels painted left of tabs (OMSI Prior./Chrono/Envir./Debug)
struct TabGroup {
    int first = 0;
    int last = 0;
    const wchar_t* name = L"";
};
// OMSI: Prior.=Tile/Objects/Splines, Chrono=Terrain/Traffic/Tracks, Envir.=Station/Spline-Exp., Debug=Signal
constexpr TabGroup kGroups[] = {{0, 2, L"Prior."}, {3, 5, L"Chrono"}, {6, 7, L"Envir."}, {8, 8, L"Debug"}};

// OMSI "Set Rules" radios (order matches screenshot)
enum class RuleRadio : int {
    SpeedLimit = 0,
    OvertakeProhib,
    NoUnsched,
    DenExtremeLow,
    DenVLow,
    DenLow,
    DenNormal,
    DenHigh,
    HigherPrio,
    LowerPrio,
    CarsProhib,
    TrucksAllowed,
    BusesCabs,
    Count
};

extern const wchar_t* kRuleRadioLabels[static_cast<int>(RuleRadio::Count)];

std::wstring to_wide(const std::string& s);
HWND mk_static(HWND p, const wchar_t* t, int x, int y, int w, int h);
HWND mk_edit(HWND p, int id, int x, int y, int w, int h);
HWND mk_btn(HWND p, int id, const wchar_t* t, int x, int y, int w, int h);
HWND mk_check(HWND p, int id, const wchar_t* t, int x, int y, int w, int h);
HWND mk_radio(HWND p, int id, const wchar_t* t, int x, int y, int w, int h, bool first);
HWND mk_list(HWND p, int id, int x, int y, int w, int h);
HWND mk_combo(HWND p, int id, int x, int y, int w, int h);

} // namespace omsi::ui::editor_side_panel_ids
