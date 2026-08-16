// MIT License
//
// Copyright (c) Thinker Mod authors
// https://github.com/induktio/thinker/
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#ifdef BUILD_REL
    #define MOD_VERSION "Thinker Mod v5.4"
#else
    #define MOD_VERSION "Thinker Mod develop build"
#endif

#ifdef BUILD_DEBUG
    #define MOD_DATE __DATE__ " " __TIME__
    #define DEBUG 1
    #define debug(...) fprintf(debug_log, __VA_ARGS__);
    #define debugw(...) { fprintf(debug_log, __VA_ARGS__); \
        fflush(debug_log); }
    #define debug_ver(...) if (conf.debug_verbose) { fprintf(debug_log, __VA_ARGS__); }
    #define flushlog() fflush(debug_log);
#else
    #define MOD_DATE __DATE__
    #define DEBUG 0
    #ifndef NDEBUG
    #define NDEBUG /* Disable assertions */
    #endif
    #define debug(...) /* Nothing */
    #define debugw(...) /* Nothing */
    #define debug_ver(...) /* Nothing */
    #define flushlog()
    #ifdef __GNUC__
    #pragma GCC diagnostic ignored "-Wunused-variable"
    #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
    #endif
#endif

#ifdef __GNUC__
    #define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
    #pragma GCC diagnostic ignored "-Wchar-subscripts"
    #pragma GCC diagnostic ignored "-Wattributes"
    #pragma GCC diagnostic error "-Wreturn-type"
#else
    #define UNUSED(x) UNUSED_ ## x
#endif

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <psapi.h>
#include <set>
#include <list>
#include <queue>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include "engine.h"

#define DLL_EXPORT extern "C" __declspec(dllexport)
#define GameAppName "Alpha Centauri"
#define GameIniFile ".\\Alpha Centauri.ini"
#define ModAppName "thinker"
#define ModIniFile ".\\thinker.ini"

#ifdef BUILD_DEBUG
#ifdef assert
#undef assert
#define assert(_Expression) \
((!!(_Expression)) \
|| (fprintf(debug_log, "Assertion Failed: %s %s %d\n", #_Expression, __FILE__, __LINE__) \
&& (_assert(#_Expression, __FILE__, __LINE__), 0)))
#endif
#endif

const bool DEF = true;
const bool ATT = false;

enum VideoMode {
    VM_Native = 0,
    VM_Custom = 1,
    VM_Window = 2,
};

/*
Neural Amplifier: process exit codes for an unattended run.

The harness has to distinguish three outcomes without parsing anything, because
the run it is grading may have produced no parseable output at all. EXIT_FAILURE
is left to exit_fail(), which every fatal path in the mod already uses, so a
distinct code is only needed for the two outcomes this fork adds.

NA_EXIT_UNANSWERABLE is deliberately not EXIT_FAILURE: "the run asked a question"
is an operator-fixable configuration problem, not a broken engine, and a harness
that cannot tell them apart will retry the one that can never succeed.
*/
enum NaExitCode {
    NA_EXIT_TURN_LIMIT = 0,     // -na-exit-turn reached; the run did what it was asked
    NA_EXIT_UNANSWERABLE = 3,   // a dialog asked something no unattended run may answer
};

struct LMConfig {
    int crater = 1;
    int volcano = 1;
    int jungle = 1;
    int uranium = 1;
    int sargasso = 1;
    int ruins = 1;
    int dunes = 1;
    int fresh = 1;
    int mesa = 1;
    int canyon = 0;
    int geothermal = 1;
    int ridge = 1;
    int borehole = 1;
    int nexus = 1;
    int unity = 1;
    int fossil = 1;
};

/*
Config parsed from thinker.ini. Alpha Centauri.ini related options
can be set negative values to use the defaults from Alpha Centauri.ini.
*/
struct Config {
    int video_mode = VM_Native;
    int window_width = 1024;
    int window_height = 768;
    int minimised = 0; // internal variable
    int video_player = 2; // internal variable
    int playing_movie = 0;  // internal variable
    int screen_width = 1024; // internal variable
    int screen_height = 768; // internal variable
    int directdraw = 0;
    int disable_opening_movie = -1;
    int smac_only = 0;
    int smooth_scrolling = 0;
    int scroll_area = 40;
    int auto_minimise = 0;
    int render_base_info = 1;
    int render_high_detail = 1; // unlisted option
    int editor_free_units = 1; // unlisted option
    int autosave_interval = 1;
    int warn_on_former_replace = 1;
    int manage_player_bases = 0;
    int manage_player_units = 0;
    int render_probe_labels = 1;
    int foreign_treaty_popup = 0;
    int game_event_popup = 0;
    int new_base_names = 1;
    int new_unit_names = 1;
    int design_units = 1;
    int factions_enabled = 7;
    /*
    Neural Amplifier: bitmask of faction ids whose decisions are routed to the
    LLM orchestrator instead of Thinker's native AI. 0 disables the bridge, so
    an unconfigured build behaves exactly like stock Thinker.
    Bit N = faction N, e.g. llm_factions=2 routes faction 1 only.
    */
    int llm_factions = 0;
    /*
    Neural Amplifier: how long a decision may wait on the orchestrator before the
    engine's own answer is applied instead (invariant 9 — the game never stalls
    waiting on the brain). Bounds the whole exchange, not each stage.

    2500ms is chosen to cover a Haiku call on a warm connection and to be short
    enough that a dead orchestrator costs a noticeable pause rather than a hung
    turn. A base is asked several times per turn but only the first call reaches
    the network, so this is the per-base-turn cost, not the per-call cost.
    */
    int llm_timeout_ms = 2500;
    /*
    Neural Amplifier: deterministic tier for base.governor_config, one of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    Applies only to player-owned bases that have set NO governor priority bit --
    see na_governor_policy() in plan.cpp for why that is the whole point.
    */
    int na_governor_policy = 0;
    /*
    Neural Amplifier: deterministic tier for base.abandon, the second of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    The surface is narrower than its name: it is the size-1 base that has a colony
    pod ready, where completing the pod spends the last population and destroys the
    base. See na_should_abandon_base() in base.cpp for why the answer is almost
    always no, and why saying no silently is the actual defect.
    */
    int na_abandon_policy = 0;
    /*
    Neural Amplifier: deterministic tier for base.hq_escape, the third of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    Unlike the other two this does NOT change the answer — see na_should_escape_hq()
    in base.cpp. It names the answer, records it, and repairs an asymmetry that
    penalised player-owned bases receiving the relocated headquarters.
    */
    int na_hq_escape_policy = 0;
    /*
    Neural Amplifier: deterministic tier for unit.odp_attack. One conservative
    orbital strike per faction-turn, and only against an existing vendetta.
    0 preserves stock behaviour (AI factions never launch ODP attacks).
    */
    int na_odp_attack_policy = 0;
    /* Default-off deterministic response for an offered technology purchase. */
    int na_tech_trade_policy = 0;
    /* Default-off deterministic response for an offered energy loan. */
    int na_energy_loan_policy = 0;
    /* Default-off deterministic response for a priced base purchase. */
    int na_base_swap_policy = 0;
    /*
    Neural Amplifier: publish this faction's own bases in every world view, so the
    orchestrator's board guard has entities to evaluate policies over. 0 keeps the
    payload exactly as it was, which is why it is off by default -- the array rides
    on every decision and the world view IS the prompt.

    Own bases only. Publishing another faction's would be an information cheat of
    the same kind the diplomacy fog gate exists to stop, and a quieter one, because
    nothing downstream would flag it.

    The fields are the ENGINE'S OWN numbers, never a derived boolean. "Is this a
    border base" is a judgement with a threshold in it, and a threshold compiled into
    the DLL is one nobody can change without a rebuild; published as defend_range it
    is a fact, and the policy that reads it owns the threshold.
    */
    int na_board_state = 0;
    /*
    Neural Amplifier: emit a base.retool observation when a production switch would cost
    banked minerals. 0 keeps stock behaviour, so an unconfigured build is unchanged.

    Observation only — this surface's deterministic tier already exists inside select_build,
    which is what na-lnv established. What was missing was the record, and without one the
    surface is invisible to coverage and has no baseline for na-6db to A/B the brain against.
    */
    int na_retool_observe = 0;
    /*
    Neural Amplifier: record in-game dialogs as decision points (invariant 7, na-4lr). 0 keeps
    stock behaviour.

    Observation only, and it NEVER suppresses — the engine's answer is passed through unchanged
    on every path, including for a dialog the table does not recognise. Invariant 7 is explicit
    that dialogs are decision points to be intercepted, not hidden.
    */
    int na_dialog_observe = 0;
    /*
    Neural Amplifier: record base.staple decisions — nerve stapling (na-yd4). 0 keeps stock
    behaviour.

    Observation only. consider_staple already decides and keeps deciding; this writes down what
    it chose. Records only when its eligibility gate opened, so a row is always a decision that
    was actually available.
    */
    int na_staple_observe = 0;
    /*
    Neural Amplifier: record econ.corner_market and council.call — two AI-only, very
    low-frequency, very high-stakes turn-scope decisions (na-yd4). 0 keeps stock behaviour.

    One flag for both because they sit in the same function, fire on the same cadence, and
    neither is useful without the other when reading a turn: a game where the council convened
    and the market was cornered is a different game from one where only one happened.
    */
    int na_endgame_observe = 0;
    /*
    Neural Amplifier: record base.satellite — which orbital a base builds (na-yd4). 0 keeps
    stock behaviour.

    Observation only. Recorded even when the chooser declines, because by the time
    find_satellite runs the gate has already opened, so "no orbital this turn" is an answer
    rather than an absent decision.
    */
    int na_satellite_observe = 0;
    /*
    Neural Amplifier: record base.project — which secret project a base starts (na-yd4). 0
    keeps stock behaviour.

    Observation only, and the richest action space of the bucket: every buildable project with
    the engine's own facility_score under this base's governor weights.
    */
    int na_project_observe = 0;
    int social_ai = 1;
    int social_ai_bias = 10;
    int tech_balance = 0;
    int base_hurry = 0;
    int base_spacing = 3;
    int base_nearby_limit = -1;
    int expansion_limit = 100;
    int expansion_autoscale = 0;
    int limit_project_start = 0;
    int max_satellites = 20;
    int new_world_builder = 1;
    int world_sea_levels[3] = {46,58,70};
    int world_hills_mod = 40;
    int world_ocean_mod = 40;
    int world_islands_mod = 16;
    int world_continents = 0;
    int world_polar_caps = 1;
    int world_mirror_x = 0;
    int world_mirror_y = 0;
    int modified_landmarks = 0;
    int time_warp_mod = 1;
    int time_warp_techs = 5;
    int time_warp_projects = 1;
    int time_warp_start_turn = 40;
    int spawn_free_units[9] = {0,0,1,0,0,1,1,0,1};
    int player_colony_pods = 0;
    int computer_colony_pods = 0;
    int player_formers = 0;
    int computer_formers = 0;
    int player_satellites[3] = {0,0,0};
    int computer_satellites[3] = {0,0,0};
    int faction_placement = 1;
    int nutrient_bonus = 0;
    int rare_supply_pods = 0;
    int simple_cost_factor = 0;
    int revised_tech_cost = 1;
    int tech_rate_modifier = 100; // internal variable
    int tech_stagnate_rate = 200;
    int fast_fungus_movement = 0;
    int magtube_movement_rate = 0;
    int road_movement_rate = 1; // internal variable
    int max_movement_rate = 255; // internal variable
    int chopper_attack_rate = 1;
    int base_event_turns = 10;
    int base_psych = 1;
    int nerve_staple_turns = 10;
    int nerve_staple_mod = -10;
    int delay_drone_riots = 0;
    int activate_skipped_units = 1; // unlisted option
    int probe_action_fix = 1; // unlisted option
    int counter_espionage = 0;
    int ignore_reactor_power = 0;
    int long_range_artillery = 0;
    int modify_upgrade_cost = 0;
    int modify_unit_support = 0;
    int modify_unit_limit = 0;
    int max_veh_num = MaxVehNum; // internal variable
    int skip_default_balance = 1; // unlisted option
    int early_research_start = 1; // unlisted option
    int base_capture_fix = 1; // unlisted option
    int facility_capture_fix = 1; // unlisted option
    int territory_border_fix = 1;
    int auto_relocate_hq = 1;
    int rebuild_secret_projects = 0;
    int steal_energy_rate = 100;
    int simple_hurry_cost = 1;
    int eco_damage_fix = 1;
    int clean_minerals = 16;
    int biology_lab_bonus = 2;
    int spawn_fungal_towers = 1;
    int spawn_spore_launchers = 1;
    int spawn_sealurks = 1;
    int spawn_battle_ogres = 1;
    int planetpearls = 1;
    int altitude_limit = 6; // internal variable
    int tile_output_limit[3] = {2,2,2};
    int soil_improve_value = 0;
    int aquatic_bonus_minerals = 1;
    int alien_guaranteed_techs = 1;
    int alien_early_start = 0;
    int cult_early_start = 0;
    int normal_elite_moves = 1;
    int native_elite_moves = 0;
    int native_weak_until_turn = -1;
    int native_lifecycle_levels[6] = {40,80,120,160,200,240};
    int cost_factor[MaxDiffNum] = {13,12,11,10,8,7};
    int tech_cost_factor[MaxDiffNum] = {124,116,108,100,84,76};
    int content_pop_player[MaxDiffNum] = {6,5,4,3,2,1};
    int content_pop_computer[MaxDiffNum] = {3,3,3,3,3,3};
    int unit_support_bonus[MaxDiffNum] = {0,0,0,0,0,0};
    int facility_talent_value[4] = {1,2,1,1};
    int facility_defense_value[4] = {100,100,100,100};
    int dream_twister_bonus = 50;
    int neural_amplifier_bonus = 50;
    int fungal_tower_bonus = 50;
    int planet_defense_bonus = 0;
    int sensor_defense_ocean = 0;
    int intercept_max_range = 2;
    int collateral_damage_value = 3;
    int repair_minimal = 1;
    int repair_fungus = 2;
    int repair_friendly = 1;
    int repair_airbase = 1;
    int repair_bunker = 1;
    int repair_base = 1;
    int repair_base_native = 10;
    int repair_base_facility = 10;
    int repair_nano_factory = 10;
    int repair_battle_ogre = 0;
    LMConfig landmarks;
    int minimal_popups = 0; // unlisted option
    int diplo_patience = 0; // internal variable
    uint32_t skip_random_events = 0; // internal variable
    uint32_t skip_random_factions = 0; // internal variable
    uint64_t skip_gov_facility = 0; // internal variable
    int faction_file_count = 14; // internal variable
    int reduced_mode = 0; // internal variable
    int debug_mode = DEBUG; // internal variable
    int debug_verbose = DEBUG; // internal variable
};

/*
AIPlans contains several general purpose variables for AI decision-making
that are recalculated each turn. These values are not stored in the save game.
*/
struct AIPlans {
    int main_region = -1;
    int main_region_x = -1;
    int main_region_y = -1;
    int main_sea_region = -1;
    int target_land_region = -1;
    int prioritize_naval = 0;
    int naval_scout_x = -1;
    int naval_scout_y = -1;
    int naval_airbase_x = -1;
    int naval_airbase_y = -1;
    int naval_start_x = -1;
    int naval_start_y = -1;
    int naval_end_x = -1;
    int naval_end_y = -1;
    int naval_beach_x = -1;
    int naval_beach_y = -1;
    int land_combat_units = 0;
    int sea_combat_units = 0;
    int air_combat_units = 0;
    int probe_units = 0;
    int missile_units = 0;
    int transport_units = 0;
    int unknown_factions = 0;
    int contacted_factions = 0;
    int enemy_factions = 0;
    int build_tubes = 0;
    /*
    Amount of minerals a base needs to produce before it is allowed to build secret projects.
    All faction-owned bases are ranked each turn based on the surplus mineral production,
    and only the top third are selected for project building.
    */
    int project_limit = 5;
    int median_limit = 5;
    int energy_limit = 15;
    /*
    PSI combat units are only selected for production if this score is higher than zero.
    Higher values will make the prototype picker choose these units more often.
    */
    int psi_score = 0;
    int keep_fungus = 0;
    int plant_fungus = 0;
    int satellite_goal = 0;
    int enemy_odp = 0;
    int enemy_sat = 0;
    int mil_strength = 0;
    int defense_modifier = 0;
    int max_offense_value = 0;
    int max_defense_value = 0;
    float enemy_base_range = 0;
    float enemy_mil_factor = 0;
    int enemy_bases = 0;
    int captured_bases = 0;
};

#include "config.h"
#include "strings.h"
#include "savegame.h"
#include "patch.h"
#include "game.h"
#include "random.h"
#include "faction.h"
#include "base.h"
#include "basewin.h"
#include "build.h"
#include "gui.h"
#include "gui_dialog.h"
#include "veh.h"
#include "veh_turn.h"
#include "veh_action.h"
#include "veh_combat.h"
#include "net.h"
#include "map.h"
#include "mapgen.h"
#include "probe.h"
#include "path.h"
#include "plan.h"
#include "goal.h"
#include "move.h"
#include "tech.h"
#include "test.h"
#include "debug.h"

extern FILE* debug_log;
extern Config conf;
extern AIPlans plans[MaxPlayerNum];
extern set_str_t movedlabels;
/*
Neural Amplifier: base URL of the orchestrator that answers /decide.
A std::string rather than a Config member because Config is all scalars.
*/
extern std::string llm_endpoint;
/*
Neural Amplifier: savegame to load automatically at startup, from -na-autoload.
Empty means boot to the main menu as normal.

A path rather than a save slot because the file we want is Thinker's own
saves/auto/Autosave_<year>.sav, which is not addressable as a slot.
*/
extern std::string na_autoload;
/*
Neural Amplifier: stop the process once this many turns have been played, from
-na-exit-turn. Zero — the default — means play until somebody quits, which is the
only sane default for a game and the reason an unattended run needs the flag at
all.

An int rather than a Config member for the same reason as the two above: these
are properties of one launch, not of the mod's configuration, and putting them in
thinker.ini would make an unattended run's terminating condition survive into the
next interactive one.
*/
extern int na_exit_turn;
/*
Neural Amplifier: seconds of a live session with no turn change after which the
run ends its own turn, from -na-auto-turn. Zero — the default — never does.

This is the OTHER thing the human at the keyboard was doing. -na-autoload
replaces the human who starts the game and -na-headless the human who dismisses
its errors, but a loaded save resumes at the player's turn and simply waits, so
an unattended run without this advances no turns at all — which also means
mod_turn_upkeep never runs and -na-exit-turn can never fire. Measured 2026-08-01:
a 7-minute headless session at turn 44 produced no autosave, no decision record
and no exit record, because nothing ever ended turn 44.

A STALL threshold rather than a period, because "the turn has not changed in N
seconds" is an observation about the game and "end a turn every N seconds" is an
assumption about it. The engine spends real time on the other factions' turns and
on its own animations; a periodic timer would fire during those, whereas a stall
timer re-arms whenever the turn number moves and so only ever speaks when nothing
else is happening.
*/
extern int na_auto_turn;
extern int na_enter_arg;
extern map_str_t musiclabels;

DLL_EXPORT DWORD ThinkerModule();
bool FileExists(const char* path);
void exit_fail(int32_t addr);
void exit_fail();
int opt_handle_error(const char* section, const char* name);
int opt_list_parse(int32_t* dst, char* src, int num, int min_val, int max_val);
