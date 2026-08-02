
#include "main.h"
#include "neural.h"
#include "lib/ini.h"

FILE* debug_log = NULL;
Config conf;
AIPlans plans[MaxPlayerNum];
set_str_t movedlabels;
std::string llm_endpoint = "http://127.0.0.1:8000";
std::string na_autoload;
int na_exit_turn = 0;
int na_auto_turn = 0;
int na_enter_arg = 0;
map_str_t musiclabels;


int option_handler(void* user, const char* section, const char* name, const char* value) {
    #define MATCH(n) strcmp(name, n) == 0
    char buf[INI_MAX_LINE];
    Config* cf = (Config*)user;
    strcpy_n(buf, INI_MAX_LINE, value);

    if (strcmp(section, "thinker") != 0) {
        return opt_handle_error(section, name);
    } else if (MATCH("DirectDraw")) {
        cf->directdraw = atoi(value);
    } else if (MATCH("DisableOpeningMovie")) {
        cf->disable_opening_movie = atoi(value);
    } else if (MATCH("video_mode")) {
        cf->video_mode = clamp(atoi(value), 0, 2);
    } else if (MATCH("window_width")) {
        cf->window_width = atoi(value);
    } else if (MATCH("window_height")) {
        cf->window_height = atoi(value);
    } else if (MATCH("smac_only")) {
        cf->smac_only = atoi(value);
    } else if (MATCH("smooth_scrolling")) {
        cf->smooth_scrolling = atoi(value);
    } else if (MATCH("scroll_area")) {
        cf->scroll_area = max(0, atoi(value));
    } else if (MATCH("auto_minimise")) {
        cf->auto_minimise = atoi(value);
    } else if (MATCH("render_base_info")) {
        cf->render_base_info = atoi(value);
    } else if (MATCH("render_high_detail")) {
        cf->render_high_detail = atoi(value);
    } else if (MATCH("editor_free_units")) {
        cf->editor_free_units = atoi(value);
    } else if (MATCH("autosave_interval")) {
        cf->autosave_interval = atoi(value);
    } else if (MATCH("warn_on_former_replace")) {
        cf->warn_on_former_replace = atoi(value);
    } else if (MATCH("llm_factions")) {
        cf->llm_factions = atoi(value);
    } else if (MATCH("llm_timeout_ms")) {
        cf->llm_timeout_ms = atoi(value);
    } else if (MATCH("na_governor_policy")) {
        cf->na_governor_policy = atoi(value);
    } else if (MATCH("na_abandon_policy")) {
        cf->na_abandon_policy = atoi(value);
    } else if (MATCH("na_hq_escape_policy")) {
        cf->na_hq_escape_policy = atoi(value);
    } else if (MATCH("llm_endpoint")) {
        char* p = strtrim(buf);
        if (strlen(p)) {
            llm_endpoint = p;
        }
        debug("llm_endpoint %s\n", llm_endpoint.c_str());
    } else if (MATCH("manage_player_bases")) {
        cf->manage_player_bases = atoi(value);
    } else if (MATCH("manage_player_units")) {
        cf->manage_player_units = atoi(value);
    } else if (MATCH("render_probe_labels")) {
        cf->render_probe_labels = atoi(value);
    } else if (MATCH("foreign_treaty_popup")) {
        cf->foreign_treaty_popup = atoi(value);
    } else if (MATCH("game_event_popup")) {
        cf->game_event_popup = atoi(value);
    } else if (MATCH("new_base_names")) {
        cf->new_base_names = atoi(value);
    } else if (MATCH("new_unit_names")) {
        cf->new_unit_names = atoi(value);
    } else if (MATCH("design_units")) {
        cf->design_units = atoi(value);
    } else if (MATCH("factions_enabled")) {
        cf->factions_enabled = atoi(value);
    } else if (MATCH("social_ai")) {
        cf->social_ai = atoi(value);
    } else if (MATCH("social_ai_bias")) {
        cf->social_ai_bias = clamp(atoi(value), 0, 1000);
    } else if (MATCH("tech_balance")) {
        cf->tech_balance = atoi(value);
    } else if (MATCH("base_hurry")) {
        cf->base_hurry = atoi(value);
    } else if (MATCH("base_spacing")) {
        cf->base_spacing = clamp(atoi(value), 2, 8);
    } else if (MATCH("base_nearby_limit")) {
        cf->base_nearby_limit = atoi(value);
    } else if (MATCH("expansion_limit")) {
        cf->expansion_limit = atoi(value);
    } else if (MATCH("expansion_autoscale")) {
        cf->expansion_autoscale = atoi(value);
    } else if (MATCH("limit_project_start")) {
        cf->limit_project_start = atoi(value);
    } else if (MATCH("max_satellites")) {
        cf->max_satellites = max(0, atoi(value));
    } else if (MATCH("new_world_builder")) {
        cf->new_world_builder = atoi(value);
    } else if (MATCH("world_continents")) {
        cf->world_continents = atoi(value);
    } else if (MATCH("world_polar_caps")) {
        cf->world_polar_caps = atoi(value);
    } else if (MATCH("world_hills_mod")) {
        cf->world_hills_mod = clamp(atoi(value), 0, 100);
    } else if (MATCH("world_ocean_mod")) {
        cf->world_ocean_mod = clamp(atoi(value), 0, 100);
    } else if (MATCH("world_islands_mod")) {
        cf->world_islands_mod = atoi(value);
    } else if (MATCH("world_mirror_x")) {
        cf->world_mirror_x = atoi(value);
    } else if (MATCH("world_mirror_y")) {
        cf->world_mirror_y = atoi(value);
    } else if (MATCH("modified_landmarks")) {
        cf->modified_landmarks = atoi(value);
    } else if (MATCH("world_sea_levels")) {
        opt_list_parse(cf->world_sea_levels, buf, 3, 0, 100);
    } else if (MATCH("time_warp_mod")) {
        cf->time_warp_mod = atoi(value);
    } else if (MATCH("time_warp_techs")) {
        cf->time_warp_techs = atoi(value);
    } else if (MATCH("time_warp_projects")) {
        cf->time_warp_projects = atoi(value);
    } else if (MATCH("time_warp_start_turn")) {
        cf->time_warp_start_turn = clamp(atoi(value), 0, 500);
    } else if (MATCH("spawn_free_units")) {
        opt_list_parse(cf->spawn_free_units, buf, 9, 0, 1000);
    } else if (MATCH("player_colony_pods")) {
        cf->player_colony_pods = atoi(value);
    } else if (MATCH("computer_colony_pods")) {
        cf->computer_colony_pods = atoi(value);
    } else if (MATCH("player_formers")) {
        cf->player_formers = atoi(value);
    } else if (MATCH("computer_formers")) {
        cf->computer_formers = atoi(value);
    } else if (MATCH("player_satellites")) {
        opt_list_parse(cf->player_satellites, buf, 3, 0, 1000);
    } else if (MATCH("computer_satellites")) {
        opt_list_parse(cf->computer_satellites, buf, 3, 0, 1000);
    } else if (MATCH("faction_placement")) {
        cf->faction_placement = atoi(value);
    } else if (MATCH("nutrient_bonus")) {
        cf->nutrient_bonus = atoi(value);
    } else if (MATCH("rare_supply_pods")) {
        cf->rare_supply_pods = atoi(value);
    } else if (MATCH("simple_cost_factor")) {
        cf->simple_cost_factor = atoi(value);
    } else if (MATCH("revised_tech_cost")) {
        cf->revised_tech_cost = atoi(value);
    } else if (MATCH("tech_stagnate_rate")) {
        cf->tech_stagnate_rate = max(1, atoi(value));
    } else if (MATCH("fast_fungus_movement")) {
        cf->fast_fungus_movement = atoi(value);
    } else if (MATCH("magtube_movement_rate")) {
        cf->magtube_movement_rate = atoi(value);
    } else if (MATCH("chopper_attack_rate")) {
        cf->chopper_attack_rate = atoi(value);
    } else if (MATCH("base_event_turns")) {
        cf->base_event_turns = clamp(atoi(value), 1, 1000);
    } else if (MATCH("base_psych")) {
        cf->base_psych = atoi(value);
    } else if (MATCH("nerve_staple_turns")) {
        cf->nerve_staple_turns = clamp(atoi(value), 0, 1000);
    } else if (MATCH("nerve_staple_mod")) {
        cf->nerve_staple_mod = atoi(value);
    } else if (MATCH("delay_drone_riots")) {
        cf->delay_drone_riots = atoi(value);
    } else if (MATCH("activate_skipped_units")) {
        cf->activate_skipped_units = atoi(value);
    } else if (MATCH("probe_action_fix")) {
        cf->probe_action_fix = atoi(value);
    } else if (MATCH("counter_espionage")) {
        cf->counter_espionage = atoi(value);
    } else if (MATCH("ignore_reactor_power")) {
        cf->ignore_reactor_power = atoi(value);
    } else if (MATCH("long_range_artillery")) {
        cf->long_range_artillery = atoi(value);
    } else if (MATCH("modify_upgrade_cost")) {
        cf->modify_upgrade_cost = atoi(value);
    } else if (MATCH("modify_unit_support")) {
        cf->modify_unit_support = atoi(value);
    } else if (MATCH("modify_unit_limit")) {
        cf->modify_unit_limit = atoi(value);
    } else if (MATCH("skip_default_balance")) {
        cf->skip_default_balance = atoi(value);
    } else if (MATCH("early_research_start")) {
        cf->early_research_start = atoi(value);
    } else if (MATCH("base_capture_fix")) {
        cf->base_capture_fix = atoi(value);
    } else if (MATCH("facility_capture_fix")) {
        cf->facility_capture_fix = atoi(value);
    } else if (MATCH("territory_border_fix")) {
        cf->territory_border_fix = atoi(value);
    } else if (MATCH("auto_relocate_hq")) {
        cf->auto_relocate_hq = atoi(value);
    } else if (MATCH("rebuild_secret_projects")) {
        cf->rebuild_secret_projects = atoi(value);
    } else if (MATCH("steal_energy_rate")) {
        cf->steal_energy_rate = clamp(atoi(value), 0, 1000);
    } else if (MATCH("simple_hurry_cost")) {
        cf->simple_hurry_cost = atoi(value);
    } else if (MATCH("eco_damage_fix")) {
        cf->eco_damage_fix = atoi(value);
    } else if (MATCH("clean_minerals")) {
        cf->clean_minerals = clamp(atoi(value), 0, 1000);
    } else if (MATCH("biology_lab_bonus")) {
        cf->biology_lab_bonus = clamp(atoi(value), 0, 1000);
    } else if (MATCH("spawn_fungal_towers")) {
        cf->spawn_fungal_towers = atoi(value);
    } else if (MATCH("spawn_spore_launchers")) {
        cf->spawn_spore_launchers = atoi(value);
    } else if (MATCH("spawn_sealurks")) {
        cf->spawn_sealurks = atoi(value);
    } else if (MATCH("spawn_battle_ogres")) {
        cf->spawn_battle_ogres = atoi(value);
    } else if (MATCH("planetpearls")) {
        cf->planetpearls = atoi(value);
    } else if (MATCH("modify_altitude_limit")) {
        cf->altitude_limit = (atoi(value) ? ALT_FOUR_ABOVE_SEA : ALT_THREE_ABOVE_SEA);
    } else if (MATCH("tile_output_limit")) {
        opt_list_parse(cf->tile_output_limit, buf, 3, 0, 100);
    } else if (MATCH("soil_improve_value")) {
        cf->soil_improve_value = clamp(atoi(value), 0, 10);
    } else if (MATCH("aquatic_bonus_minerals")) {
        cf->aquatic_bonus_minerals = atoi(value);
    } else if (MATCH("alien_guaranteed_techs")) {
        cf->alien_guaranteed_techs = atoi(value);
    } else if (MATCH("alien_early_start")) {
        cf->alien_early_start = atoi(value);
    } else if (MATCH("cult_early_start")) {
        cf->cult_early_start = atoi(value);
    } else if (MATCH("normal_elite_moves")) {
        cf->normal_elite_moves = atoi(value);
    } else if (MATCH("native_elite_moves")) {
        cf->native_elite_moves = atoi(value);
    } else if (MATCH("native_weak_until_turn")) {
        cf->native_weak_until_turn = clamp(atoi(value), 0, 1000);
    } else if (MATCH("native_lifecycle_levels")) {
        opt_list_parse(cf->native_lifecycle_levels, buf, 6, 0, 1000);
    } else if (MATCH("cost_factor")) {
        opt_list_parse(cf->cost_factor, buf, MaxDiffNum, 1, 100);
    } else if (MATCH("tech_cost_factor")) {
        opt_list_parse(cf->tech_cost_factor, buf, MaxDiffNum, 1, 1000);
    } else if (MATCH("content_pop_player")) {
        opt_list_parse(cf->content_pop_player, buf, MaxDiffNum, 0, 1000);
    } else if (MATCH("content_pop_computer")) {
        opt_list_parse(cf->content_pop_computer, buf, MaxDiffNum, 0, 1000);
    } else if (MATCH("unit_support_bonus")) {
        opt_list_parse(cf->unit_support_bonus, buf, MaxDiffNum, 0, 1000);
    } else if (MATCH("facility_talent_value")) {
        opt_list_parse(cf->facility_talent_value, buf, 6, 0, 1000);
    } else if (MATCH("facility_defense_value")) {
        opt_list_parse(cf->facility_defense_value, buf, 4, 0, 1000);
    } else if (MATCH("dream_twister_bonus")) {
        cf->dream_twister_bonus = clamp(atoi(value), 0, 1000);
    } else if (MATCH("neural_amplifier_bonus")) {
        cf->neural_amplifier_bonus = clamp(atoi(value), 0, 1000);
    } else if (MATCH("fungal_tower_bonus")) {
        cf->fungal_tower_bonus = clamp(atoi(value), 0, 1000);
    } else if (MATCH("planet_defense_bonus")) {
        cf->planet_defense_bonus = atoi(value);
    } else if (MATCH("sensor_defense_ocean")) {
        cf->sensor_defense_ocean = atoi(value);
    } else if (MATCH("intercept_max_range")) {
        cf->intercept_max_range = clamp(atoi(value), 0, 8);
    } else if (MATCH("collateral_damage_value")) {
        cf->collateral_damage_value = clamp(atoi(value), 0, 100);
    } else if (MATCH("repair_minimal")) {
        cf->repair_minimal = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_fungus")) {
        cf->repair_fungus = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_friendly")) {
        cf->repair_friendly = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_airbase")) {
        cf->repair_airbase = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_bunker")) {
        cf->repair_bunker = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_base")) {
        cf->repair_base = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_base_native")) {
        cf->repair_base_native = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_base_facility")) {
        cf->repair_base_facility = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_nano_factory")) {
        cf->repair_nano_factory = clamp(atoi(value), 0, 10);
    } else if (MATCH("repair_battle_ogre")) {
        cf->repair_battle_ogre = clamp(atoi(value), 0, 10);
    } else if (MATCH("minimal_popups")) {
        if (DEBUG) {
            cf->minimal_popups = atoi(value);
            cf->debug_verbose = !atoi(value);
        }
    } else if (MATCH("skip_event")) {
        int val = clamp(atoi(value), 0, 32);
        cf->skip_random_events = (!val ? 0 : cf->skip_random_events | (1 << (val - 1)));
    } else if (MATCH("skip_faction")) {
        int val = clamp(atoi(value), 0, 32);
        cf->skip_random_factions = (!val ? 0 : cf->skip_random_factions | (1 << (val - 1)));
    } else if (MATCH("skip_facility")) {
        int val = clamp(atoi(value), 0, 64);
        cf->skip_gov_facility = (!val ? 0 : cf->skip_gov_facility | (1 << (val - 1)));
    } else if (MATCH("crater")) {
        cf->landmarks.crater = max(0, atoi(value));
    } else if (MATCH("volcano")) {
        cf->landmarks.volcano = max(0, atoi(value));
    } else if (MATCH("jungle")) {
        cf->landmarks.jungle = max(0, atoi(value));
    } else if (MATCH("uranium")) {
        cf->landmarks.uranium = max(0, atoi(value));
    } else if (MATCH("sargasso")) {
        cf->landmarks.sargasso = max(0, atoi(value));
    } else if (MATCH("ruins")) {
        cf->landmarks.ruins = max(0, atoi(value));
    } else if (MATCH("dunes")) {
        cf->landmarks.dunes = max(0, atoi(value));
    } else if (MATCH("fresh")) {
        cf->landmarks.fresh = max(0, atoi(value));
    } else if (MATCH("mesa")) {
        cf->landmarks.mesa = max(0, atoi(value));
    } else if (MATCH("canyon")) {
        cf->landmarks.canyon = max(0, atoi(value));
    } else if (MATCH("geothermal")) {
        cf->landmarks.geothermal = max(0, atoi(value));
    } else if (MATCH("ridge")) {
        cf->landmarks.ridge = max(0, atoi(value));
    } else if (MATCH("borehole")) {
        cf->landmarks.borehole = max(0, atoi(value));
    } else if (MATCH("nexus")) {
        cf->landmarks.nexus = max(0, atoi(value));
    } else if (MATCH("unity")) {
        cf->landmarks.unity = max(0, atoi(value));
    } else if (MATCH("fossil")) {
        cf->landmarks.fossil = max(0, atoi(value));
    } else if (MATCH("label_pop_size")) {
        parse_format_args(label_pop_size, value, 4, StrBufLen);
    } else if (MATCH("label_pop_boom")) {
        parse_format_args(label_pop_boom, value, 0, StrBufLen);
    } else if (MATCH("label_nerve_staple")) {
        parse_format_args(label_nerve_staple, value, 1, StrBufLen);
    } else if (MATCH("label_captured_base")) {
        parse_format_args(label_captured_base, value, 1, StrBufLen);
    } else if (MATCH("label_stockpile_energy")) {
        parse_format_args(label_stockpile_energy, value, 1, StrBufLen);
    } else if (MATCH("label_sat_nutrient")) {
        parse_format_args(label_sat_nutrient, value, 1, StrBufLen);
    } else if (MATCH("label_sat_mineral")) {
        parse_format_args(label_sat_mineral, value, 1, StrBufLen);
    } else if (MATCH("label_sat_energy")) {
        parse_format_args(label_sat_energy, value, 1, StrBufLen);
    } else if (MATCH("label_eco_damage")) {
        parse_format_args(label_eco_damage, value, 2, StrBufLen);
    } else if (MATCH("label_base_surplus")) {
        parse_format_args(label_base_surplus, value, 3, StrBufLen);
    } else if (MATCH("label_unit_reactor")) {
        int len = strlen(buf);
        int j = 0;
        int k = 0;
        for (int i = 0; i < len && i < StrBufLen && k < 4; i++) {
            bool last = i == len - 1;
            if (buf[i] == ',' || last) {
                strncpy(label_unit_reactor[k], buf+j, i-j+last);
                label_unit_reactor[k][i-j+last] = '\0';
                j = i + 1;
                k++;
            }
        }
    } else if (MATCH("script_label")) {
        char* p = strupr(strtrim(buf));
        debug("script_label %s\n", p);
        movedlabels.insert(p);
    } else if (MATCH("music_label")) {
        char *p, *s, *k, *v;
        if ((p = strtok_r(buf, ",", &s)) != NULL) {
            k = strtrim(p);
            if ((p = strtok_r(NULL, ",", &s)) != NULL) {
                v = strtrim(p);
                if (strlen(k) && strlen(v)) {
                    debug("music_label %s = %s\n", k, v);
                    musiclabels[k] = v;
                }
            }
        }
    } else {
        return opt_handle_error(section, name);
    }
    return 1;
}

int opt_handle_error(const char* section, const char* name) {
    static bool unknown_option = false;
    char msg[1024] = {};
    if (!unknown_option) {
        snprintf(msg, sizeof(msg),
            "Unknown configuration option detected in thinker.ini.\n"
            "Game might not work as intended.\n"
            "Header: %s\n"
            "Option: %s\n",
            section, name);
        // Advisory, not fatal: the option is skipped and the game runs on, so under
        // -na-headless this becomes a log line and the run continues exactly as an
        // operator clicking OK would have made it continue.
        na_message_box(0, msg, MOD_VERSION, MB_OK | MB_ICONWARNING);
    }
    unknown_option = true;
    return 0;
}

int opt_list_parse(int32_t* dst, char* src, int num, int min_val, int max_val) {
    const char *d=",";
    char *s, *p;
    p = strtok_r(src, d, &s);
    for (int i = 0; i < num && p != NULL; i++, p = strtok_r(NULL, d, &s)) {
        dst[i] = clamp(atoi(p), min_val, max_val);
    }
    return 0;
}

int cmd_parse(Config* cf) {
    int argc;
    LPWSTR* argv;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-smac") == 0) {
            cf->smac_only = 1;
        } else if (wcscmp(argv[i], L"-native") == 0) {
            cf->video_mode = VM_Native;
        } else if (wcscmp(argv[i], L"-screen") == 0) {
            cf->video_mode = VM_Custom;
        } else if (wcscmp(argv[i], L"-windowed") == 0) {
            cf->video_mode = VM_Window;
        } else if (wcscmp(argv[i], L"-na-autoload") == 0 && i + 1 < argc) {
            /*
            Neural Amplifier: load this savegame instead of waiting at the main
            menu. Takes the next argument as a path, so it consumes i+1.

            Narrowed with WideCharToMultiByte rather than assuming ASCII: the
            path routinely contains a Steam library directory, which is outside
            our control and may not be ASCII.
            */
            char buf[1024] = {};
            int n = WideCharToMultiByte(CP_ACP, 0, argv[i+1], -1,
                                        buf, sizeof(buf)-1, NULL, NULL);
            if (n > 0) {
                na_autoload = buf;
            }
            i++;
        } else if (wcscmp(argv[i], L"-na-exit-turn") == 0 && i + 1 < argc) {
            /*
            Neural Amplifier: end the process cleanly once this many turns have
            been played, so an unattended run terminates on its own instead of
            being killed by the harness timeout. The check itself is
            na_exit_turn_check, called from mod_turn_upkeep — see neural.cpp for
            why that site and not one of the three more obvious ones.

            _wtoi rather than the WideCharToMultiByte dance -na-autoload needs: a
            turn count is digits, so there is no encoding to get wrong, and a
            malformed value yields 0, which na_exit_turn_check reads as "no
            limit". That is the right way for this to fail — a run that does not
            stop is recoverable by the harness timeout, whereas a run that stops
            at turn 0 because the argument was mistyped looks like a working
            harness reporting a broken game.
            */
            na_exit_turn = _wtoi(argv[i+1]);
            i++;
        } else if (wcscmp(argv[i], L"-na-auto-turn") == 0 && i + 1 < argc) {
            /*
            Neural Amplifier: end our own turn after this many seconds of a live
            session in which the turn number has not moved. See na_auto_turn_tick.

            Seconds rather than milliseconds because the value is a patience, not
            a timing: it has to be longer than the engine's slowest legitimate
            pause, which is other factions' turns, and nobody tuning that is
            thinking in milliseconds.

            Same _wtoi and the same failure direction as -na-exit-turn: a
            malformed value yields 0, which means "never end my turn". A run that
            does not advance is a visibly stalled run the harness timeout will
            kill; a run that ended turns every 0 seconds would fire continuously
            and look like the game playing itself.
            */
            na_auto_turn = _wtoi(argv[i+1]);
            i++;
        } else if (wcscmp(argv[i], L"-na-headless") == 0) {
            /*
            Neural Amplifier: assert that nobody is watching, so Thinker's modal
            error boxes are routed to stderr and the observation log rather than
            waiting under Xvfb for an OK that never comes.

            Parsed here only so that the flag is documented alongside its
            siblings and does not read as an unknown argument. The gate that acts
            on it is na_headless(), which re-reads the command line itself,
            because four of the dialogs it covers fire before this function runs.
            -na-autoload implies it; see na_headless() for why -na-exit-turn does
            not.
            */
        } else if (wcscmp(argv[i], L"-na-enter-arg") == 0 && i + 1 < argc) {
            // Second argument to the 0x58F450 transition. Sweepable from the command
            // line because its meaning is unknown and each candidate needs a restart.
            na_enter_arg = _wtoi(argv[i+1]);
            i++;
        }
    }
    LocalFree(argv);
    return 1;
}

bool FileExists(const char* path) {
    return GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
}

void exit_fail(int32_t addr) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "Error while patching address %08X in the game binary.\n"
        "This mod requires Alien Crossfire v2.0 terranx.exe in the same folder.", addr);
    // Fatal. The exit below is what makes the headless path safe: suppressing the
    // box does not suppress the failure, it only changes where the failure is
    // reported. A patch that did not apply must never be played through.
    na_message_box(0, buf, MOD_VERSION, MB_OK | MB_ICONSTOP);
    exit(EXIT_FAILURE);
}

void exit_fail() {
    exit(EXIT_FAILURE);
}

DLL_EXPORT DWORD ThinkerModule() {
    return 0;
}

DLL_EXPORT BOOL APIENTRY DllMain(HINSTANCE UNUSED(hinstDLL), DWORD fdwReason, LPVOID UNUSED(lpvReserved)) {
    size_t seed;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            /*
            All four of these are fatal, and all four run BEFORE cmd_parse — which
            is why na_headless() reads the command line for itself rather than
            waiting to be told. Under Xvfb these were the worst dialogs in the mod:
            they fire before a window exists, so the failure looked like a process
            that started and then did nothing at all, with no box to read even if
            somebody had attached a viewer.

            Each still calls exit_fail() immediately, headless or not. Suppression
            changes only where the message is written.
            */
            if (DEBUG && !(debug_log = fopen("debug.txt", "w"))) {
                na_message_box(0, "Error while opening debug.txt file.",
                    MOD_VERSION, MB_OK | MB_ICONSTOP);
                exit_fail();
            }
            if (ini_parse("thinker.ini", option_handler, &conf) < 0) {
                na_message_box(0, "Error while opening thinker.ini file.",
                    MOD_VERSION, MB_OK | MB_ICONSTOP);
                exit_fail();
            }
            if (FileExists("thinker_user.ini")
            && ini_parse("thinker_user.ini", option_handler, &conf) < 0) {
                na_message_box(0, "Error while opening thinker_user.ini file.",
                    MOD_VERSION, MB_OK | MB_ICONSTOP);
                exit_fail();
            }
            if (!cmd_parse(&conf) || !patch_setup(&conf)) {
                na_message_box(0, "Error while loading the game.",
                    MOD_VERSION, MB_OK | MB_ICONSTOP);
                exit_fail();
            }
            *EngineVersion = MOD_VERSION;
            *EngineDate = MOD_DATE;
            seed = GetTickCount();
            random_reseed(seed);
            map_rand.reseed(seed ^ 0xffff);
            debug("random_reseed %u\n", seed);
            flushlog();
            break;

        case DLL_PROCESS_DETACH:
            if (debug_log) {
                fclose(debug_log);
            }
            break;

        case DLL_THREAD_ATTACH:
            break;

        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}



