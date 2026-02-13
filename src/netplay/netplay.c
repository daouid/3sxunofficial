#include "netplay/netplay.h"
#include "netplay/game_state.h"
#include "sf33rd/Source/Game/Game.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/main.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/utils/djb2_hash.h"
#include "types.h"

#include <stdbool.h>

#define Game GekkoGame // workaround: upstream GekkoSessionType::Game collides with void Game()
#include "gekkonet.h"
#undef Game
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define INPUT_HISTORY_MAX 120

// Uncomment to enable packet drops
// #define LOSSY_ADAPTER

typedef enum SessionState {
    SESSION_IDLE,
    SESSION_TRANSITIONING,
    SESSION_CONNECTING,
    SESSION_RUNNING,
    SESSION_EXITING,
} SessionState;

typedef struct EffectState {
    s16 frwctr;
    s16 frwctr_min;
    s16 head_ix[8];
    s16 tail_ix[8];
    s16 exec_tm[8];
    uintptr_t frw[EFFECT_MAX][448];
    s16 frwque[EFFECT_MAX];
} EffectState;

typedef struct State {
    GameState gs;
    EffectState es;
} State;

static GekkoSession* session = NULL;
static unsigned short local_port = 0;
static unsigned short remote_port = 0;
static const char* remote_ip = NULL;
static int player_number = 0;
static int player_handle = 0;
static SessionState session_state = SESSION_IDLE;
static u16 input_history[2][INPUT_HISTORY_MAX] = { 0 };
static float frames_behind = 0;
static int frame_skip_timer = 0;

#if defined(DEBUG)
#define STATE_BUFFER_MAX 20
#endif

#if defined(LOSSY_ADAPTER)
static GekkoNetAdapter* base_adapter = NULL;
static GekkoNetAdapter lossy_adapter = { 0 };

static float random_float() {
    return (float)rand() / RAND_MAX;
}

static void LossyAdapter_SendData(GekkoNetAddress* addr, const char* data, int length) {
    const float number = random_float();

    // Adjust this number to change drop probability
    if (number <= 0.25) {
        return;
    }

    base_adapter->send_data(addr, data, length);
}
#endif

static void clean_input_buffers() {
    p1sw_0 = 0;
    p2sw_0 = 0;
    p1sw_1 = 0;
    p2sw_1 = 0;
    p1sw_buff = 0;
    p2sw_buff = 0;
    SDL_zeroa(PLsw);
    SDL_zeroa(plsw_00);
    SDL_zeroa(plsw_01);
}

static void setup_vs_mode() {
    // This is pretty much a copy of logic from menu.c
    task[TASK_MENU].r_no[0] = 5; // go to idle routine (doing nothing)
    cpExitTask(TASK_SAVER);
    plw[0].wu.operator = 1;
    plw[1].wu.operator = 1;
    Operator_Status[0] = 1;
    Operator_Status[1] = 1;
    grade_check_work_1st_init(0, 0);
    grade_check_work_1st_init(0, 1);
    grade_check_work_1st_init(1, 0);
    grade_check_work_1st_init(1, 1);
    Setup_Training_Difficulty();

    G_No[1] = 12;
    G_No[2] = 1;
    Mode_Type = MODE_NETWORK;
    cpExitTask(TASK_MENU);

    E_Timer = 0; // E_Timer can have different values depending on when the session was initiated

    Deley_Shot_No[0] = 0;
    Deley_Shot_No[1] = 0;
    Deley_Shot_Timer[0] = 15;
    Deley_Shot_Timer[1] = 15;
    Random_ix16 = 0;
    Random_ix32 = 0;
    Clear_Flash_Init(4);

    // Ensure both peers start with identical timer state regardless of local DIP switch settings.
    // Without this, save_w[Present_Mode].Time_Limit can differ per player's config.
    Counter_hi = 99;
    Counter_low = 60;

    // Flash_Complete runs during the character select screen at slightly different
    // speeds per peer depending on when they connected. Zero it to sync.
    Flash_Complete[0] = 0;
    Flash_Complete[1] = 0;

    // BG scroll positions and parameters evolve independently during the transition
    // phase before synced gameplay. Zero them so both peers start identical.
    SDL_zeroa(bg_pos);
    SDL_zeroa(fm_pos);
    SDL_zeroa(bg_prm);
    system_timer = 0;

    clean_input_buffers();
}

#if defined(LOSSY_ADAPTER)
static void configure_lossy_adapter() {
    base_adapter = gekko_default_adapter(local_port);
    lossy_adapter.send_data = LossyAdapter_SendData;
    lossy_adapter.receive_data = base_adapter->receive_data;
    lossy_adapter.free_data = base_adapter->free_data;
}
#endif

static void configure_gekko() {
    GekkoConfig config;
    SDL_zero(config);

    config.num_players = 2;
    config.input_size = sizeof(u16);
    config.state_size = sizeof(State);
    config.max_spectators = 0;
    config.input_prediction_window = 10;

#if defined(DEBUG)
    config.desync_detection = true;
#endif

    if (gekko_create(&session, GekkoGame)) {
        gekko_start(session, &config);
    } else {
        printf("Session is already running! probably incorrect.\n");
    }

#if defined(LOSSY_ADAPTER)
    configure_lossy_adapter();
    gekko_net_adapter_set(session, &lossy_adapter);
#else
    gekko_net_adapter_set(session, gekko_default_adapter(local_port));
#endif

    printf("starting a session for player %d at port %hu\n", player_number, local_port);

    char remote_address_str[100];
    SDL_snprintf(remote_address_str, sizeof(remote_address_str), "%s:%hu", remote_ip, remote_port);
    GekkoNetAddress remote_address = { .data = remote_address_str, .size = strlen(remote_address_str) };

    if (player_number == 0) {
        player_handle = gekko_add_actor(session, LocalPlayer, NULL);
        gekko_add_actor(session, RemotePlayer, &remote_address);
    } else {
        gekko_add_actor(session, RemotePlayer, &remote_address);
        player_handle = gekko_add_actor(session, LocalPlayer, NULL);
    }
}

static u16 get_inputs() {
    // The game doesn't differentiate between controllers and players.
    // That's why we OR the inputs of both local controllers together to get
    // local inputs.
    u16 inputs = 0;
    inputs = p1sw_buff | p2sw_buff;
    return inputs;
}

static void note_input(u16 input, int player, int frame) {
    if (frame < 0) {
        return;
    }

    input_history[player][frame % INPUT_HISTORY_MAX] = input;
}

static u16 recall_input(int player, int frame) {
    if (frame < 0) {
        return 0;
    }

    return input_history[player][frame % INPUT_HISTORY_MAX];
}

#if defined(DEBUG)
// Per-subsystem checksums for faster desync triage — when a desync fires,
// we can immediately tell which section (player, bg, effects...) diverged.
typedef struct {
    uint32_t plw0;
    uint32_t plw1;
    uint32_t bg;
    uint32_t tasks;
    uint32_t effects;
    uint32_t globals;
    uint32_t combined;
} SectionedChecksum;

static SectionedChecksum calculate_sectioned_checksums(const State* state) {
    SectionedChecksum sc;

    uint32_t h;

    h = djb2_init();
    h = djb2_update_mem(h, (const uint8_t*)&state->gs.plw[0], sizeof(PLW));
    sc.plw0 = h;

    h = djb2_init();
    h = djb2_update_mem(h, (const uint8_t*)&state->gs.plw[1], sizeof(PLW));
    sc.plw1 = h;

    h = djb2_init();
    h = djb2_update_mem(h, (const uint8_t*)&state->gs.bg_w, sizeof(state->gs.bg_w));
    sc.bg = h;

    h = djb2_init();
    h = djb2_update_mem(h, (const uint8_t*)&state->gs.task, sizeof(state->gs.task));
    sc.tasks = h;

    h = djb2_init();
    h = djb2_update_mem(h, (const uint8_t*)&state->es, sizeof(EffectState));
    sc.effects = h;

    // Combined hash covers the entire state (for GekkoNet exchange)
    h = djb2_init();
    h = djb2_updatep(h, state);
    sc.combined = h;

    // Rough diagnostic only: XOR is not a proper remainder hash,
    // but good enough to spot which broad area drifted.
    sc.globals = sc.combined ^ sc.plw0 ^ sc.plw1 ^ sc.bg ^ sc.tasks ^ sc.effects;

    return sc;
}


static State state_buffer[STATE_BUFFER_MAX];

static void dump_state(const State* src, const char* filename) {
    SDL_IOStream* io = SDL_IOFromFile(filename, "w");
    SDL_WriteIO(io, src, sizeof(State));
    SDL_CloseIO(io);
}

static void dump_saved_state(int frame) {
    const State* src = &state_buffer[frame % STATE_BUFFER_MAX];

    char filename[100];
    SDL_snprintf(filename, sizeof(filename), "states/%d_%d", player_handle, frame);

    dump_state(src, filename);
}
#endif

#define SDL_copya(dst, src) SDL_memcpy(dst, src, sizeof(src))

static void gather_state(State* dst) {
    // GameState
    GameState* gs = &dst->gs;
    GameState_Save(gs);

    // EffectState
    EffectState* es = &dst->es;
    SDL_copya(es->frw, frw);
    SDL_copya(es->exec_tm, exec_tm);
    SDL_copya(es->frwque, frwque);
    SDL_copya(es->head_ix, head_ix);
    SDL_copya(es->tail_ix, tail_ix);
    es->frwctr = frwctr;
    es->frwctr_min = frwctr_min;
}



// These effect IDs use the WORK_Other_CONN layout (variable-length conn[] tail).
// Derived by auditing every effXX.c that casts to WORK_Other_CONN*.
static bool is_work_other_conn(int id) {
    switch (id) {
        case 16:  // eff16 (Score Breakdown) - Caused F1549 Desync
        case 160: // effg0
        case 170: // effh0
        case 179: // effh9
        case 192: // effj2
        case 211: // effL1
        case 223: // effm3
            return true;
        default:
            return false;
    }
}

// Zero the unused tail of conn[] — entries past num_of_conn are uninitialized
// heap data that differs between peers. Root cause of the F1549 desync.
static void sanitize_work_other_conn(WORK* w) {
    WORK_Other_CONN* wc = (WORK_Other_CONN*)w;
    int count = wc->num_of_conn;
    // Safety check: count should be within bounds [0, 108].
    // 108 is the compile-time size of the conn[] array in WORK_Other_CONN.
    enum { CONN_MAX = 108 };
    if (count >= 0 && count < CONN_MAX) {
        size_t bytes = (CONN_MAX - count) * sizeof(CONN);
        if (bytes > 0) {
            SDL_memset(&wc->conn[count], 0, bytes);
        }
    }
}

/// Zero pointer fields so they don't pollute checksums (ASLR makes them differ).
/// Only ever called on a scratch copy — never on a state Gekko will restore.
static void sanitize_work_pointers(WORK* w) {
    w->target_adrs = NULL;
    w->hit_adrs = NULL;
    w->dmg_adrs = NULL;
    w->suzi_offset = NULL;
    SDL_zeroa(w->char_table);
    w->se_random_table = NULL;
    w->step_xy_table = NULL;
    w->move_xy_table = NULL;
    w->overlap_char_tbl = NULL;
    w->olc_ix_table = NULL;
    w->rival_catch_tbl = NULL;
    w->curr_rca = NULL;
    w->set_char_ad = NULL;
    w->hit_ix_table = NULL;
    w->body_adrs = NULL;
    w->h_bod = NULL;
    w->hand_adrs = NULL;
    w->h_han = NULL;
    w->dumm_adrs = NULL;
    w->h_dumm = NULL;
    w->catch_adrs = NULL;
    w->h_cat = NULL;
    w->caught_adrs = NULL;
    w->h_cau = NULL;
    w->attack_adrs = NULL;
    w->h_att = NULL;
    w->h_eat = NULL;
    w->hosei_adrs = NULL;
    w->h_hos = NULL;
    w->att_ix_table = NULL;
    w->my_effadrs = NULL;
}

/// Mask rendering-only bits/fields from WORK color fields.
/// - current_colcd, my_col_code: strip 0x2000 player-side palette flag
/// - colcd: fully zeroed (derived from current_colcd by rendering, can differ entirely)
/// - extra_col, extra_col_2: strip 0x2000 palette flag
static void sanitize_work_rendering(WORK* w) {
    w->current_colcd &= ~0x2000;
    w->my_col_code   &= ~0x2000;
    w->colcd          = 0;         // Rendering-derived, not gameplay state
    w->extra_col     &= ~0x2000;
    w->extra_col_2   &= ~0x2000;
}

/// Zero all pointer fields and mask rendering bits in a PLW struct.
static void sanitize_plw_pointers(PLW* p) {
    sanitize_work_pointers(&p->wu);
    sanitize_work_rendering(&p->wu);
    p->cp = NULL;
    p->dm_step_tbl = NULL;
    p->as = NULL;
    p->sa = NULL;
    p->py = NULL;
}

/// Save state in state buffer.
/// @return Mutable pointer to state as it has been saved.
static State* note_state(const State* state, int frame) {
    if (frame < 0) {
        frame += STATE_BUFFER_MAX;
    }

    State* dst = &state_buffer[frame % STATE_BUFFER_MAX];
    SDL_memcpy(dst, state, sizeof(State));
    return dst;
}

static void save_state(GekkoGameEvent* event) {
    *event->data.save.state_len = sizeof(State);
    State* dst = (State*)event->data.save.state;

    gather_state(dst);

#if defined(DEBUG)
    const int frame = event->data.save.frame;

    // Wait for battle to actually start (G_No[1] == 2 means Game2_0 has run)
    // before checksumming. Menu-to-battle transition leaves dozens of globals
    // in flight for several frames; checksumming during that window = false desyncs.
    static int battle_start_frame = -1;
    enum { BATTLE_SETTLE_FRAMES = 10 };

    if (battle_start_frame < 0 && G_No[1] == 2) {
        battle_start_frame = frame;
        // Menu-phase globals that battle logic never clears
        Next_Demo = 0;
        gather_state(dst);  // Re-gather with zeroed value
        SDL_Log("[P%d] battle detected at frame %d, checksumming starts at frame %d",
                local_port, frame, frame + BATTLE_SETTLE_FRAMES);
    }

    const bool checksumming_active =
        battle_start_frame >= 0 && frame >= battle_start_frame + BATTLE_SETTLE_FRAMES;

    // BACKUP the current (forward) state in this slot before overwriting it,
    // so we can dump it if a desync is detected.
    static State forward_backup;
    bool has_forward_backup = false;
    if (frame > -1) {
        int idx = frame % STATE_BUFFER_MAX;
        if (idx < 0) idx += STATE_BUFFER_MAX;
        SDL_memcpy(&forward_backup, &state_buffer[idx], sizeof(State));
        has_forward_backup = true;
    }

    note_state(dst, frame); // Backup current state to buffer

    // Sanitize ONLY non-functional data in dst (safe for rollback restore):
    // - Inactive effect slots: zero everything (be_flag == 0 means unused)
    // - Padding arrays: wrd_free, et_free (never read by game logic)
    // - WORK_Other_CONN unused tail entries (beyond num_of_conn)
    // IMPORTANT: Do NOT zero pointer fields in dst — Gekko loads this state
    // on rollback, and NULL pointers would crash the game immediately.
    {
        EffectState* es = &dst->es;
        for (int i = 0; i < EFFECT_MAX; i++) {
            WORK* w = (WORK*)es->frw[i];
            if (w->be_flag == 0) {
                // Slot unused — zero everything except linked-list pointers
                // (before/behind/myself) which the effect system needs intact.
                s16 before = w->before;
                s16 behind = w->behind;
                s16 myself = w->myself;
                SDL_memset(es->frw[i], 0, sizeof(es->frw[i]));
                w->before = before;
                w->behind = behind;
                w->myself = myself;
            } else {
                // Active slot: zero only padding (safe for rollback)
                SDL_zeroa(w->wrd_free);

                WORK_Other* wo = (WORK_Other*)w;
                SDL_zeroa(wo->et_free);

                if (is_work_other_conn(w->id)) {
                    sanitize_work_other_conn(w);
                }
            }
        }

        note_state(dst, frame);
    }

    if (checksumming_active) {
        // Pointer fields (PLW pointers, WORK pointers, WORK_Other.my_master)
        // differ between processes because each has its own address space.
        // Sanitize a COPY for checksumming — never modify dst.
        static State checksum_scratch;
        SDL_memcpy(&checksum_scratch, dst, sizeof(State));

        // Zero PLW pointers in the copy
        sanitize_plw_pointers(&checksum_scratch.gs.plw[0]);
        sanitize_plw_pointers(&checksum_scratch.gs.plw[1]);

        // Zero EffectState WORK/WORK_Other pointers in the copy
        EffectState* es = &checksum_scratch.es;
        for (int i = 0; i < EFFECT_MAX; i++) {
            WORK* w = (WORK*)es->frw[i];
            if (w->be_flag != 0) {
                sanitize_work_pointers(w);
                sanitize_work_rendering(w);
                // WORK_Other variants all have my_master right after WORK
                WORK_Other* wo = (WORK_Other*)w;
                wo->my_master = NULL;
            }
        }

        // Zero GameState pointer fields that differ due to ASLR
        checksum_scratch.gs.ci_pointer = NULL;
        for (int i = 0; i < SDL_arraysize(checksum_scratch.gs.task); i++) {
            checksum_scratch.gs.task[i].func_adrs = NULL;
        }

        // Zero viewport/resolution-dependent background rendering state
        // (bg_pos, bg_prm, BgMATRIX differ between peers due to window size)
        SDL_zeroa(checksum_scratch.gs.bg_pos);
        SDL_zeroa(checksum_scratch.gs.bg_prm);
        SDL_zeroa(checksum_scratch.gs.BgMATRIX);

        SectionedChecksum sc = calculate_sectioned_checksums(&checksum_scratch);
        *event->data.save.checksum = sc.combined;

        // Track forward fx hash per frame using a ringbuffer.
        // When rollback replays a frame, compare & dump.
        enum { FX_RING_SIZE = 32 };
        static uint32_t fx_ring[FX_RING_SIZE] = {0};
        static int max_forward_frame = -1;

        if (frame > max_forward_frame) {
            // Forward simulation: record the fx hash
            fx_ring[frame % FX_RING_SIZE] = sc.effects;
            max_forward_frame = frame;
        } else {
            // Rollback replay: compare with stored forward hash
            uint32_t fwd = fx_ring[frame % FX_RING_SIZE];
            bool matches = (sc.effects == fwd);
            SDL_Log("[P%d F%d] ROLLBACK fx=%08X (fwd=%08X) %s",
                    local_port, frame, sc.effects, fwd,
                    matches ? "OK" : "DIVERGED!");
            if (!matches) {
                // Dump the ROLLBACK state for offline diff
                char fname[100];
                SDL_snprintf(fname, sizeof(fname), "states/rollback_%d_%d", local_port, frame);
                dump_state(dst, fname);
                // Also save the forward state from backup
                if (has_forward_backup) {
                    SDL_snprintf(fname, sizeof(fname), "states/forward_%d_%d", local_port, frame);
                    dump_state(&forward_backup, fname);
                }
            }
            // Update the ringbuffer with the rollback hash (GekkoNet uses this one)
            fx_ring[frame % FX_RING_SIZE] = sc.effects;
        }
    }
#endif
}

static void load_state(const State* src) {
    // GameState
    const GameState* gs = &src->gs;
    GameState_Load(gs);

    // EffectState
    const EffectState* es = &src->es;
    SDL_copya(frw, es->frw);
    SDL_copya(exec_tm, es->exec_tm);
    SDL_copya(frwque, es->frwque);
    SDL_copya(head_ix, es->head_ix);
    SDL_copya(tail_ix, es->tail_ix);
    frwctr = es->frwctr;
    frwctr_min = es->frwctr_min;
}

static void load_state_from_event(GekkoGameEvent* event) {
    const State* src = (State*)event->data.load.state;
    load_state(src);
}

static bool game_ready_to_run_character_select() {
    return G_No[1] == 1;
}

static bool need_to_catch_up() {
    return frames_behind >= 1;
}

static void step_game(bool render) {
    No_Trans = !render;

    njUserMain();
    seqsBeforeProcess();
    njdp2d_draw();
    seqsAfterProcess();
}

static void advance_game(GekkoGameEvent* event, bool render) {
    const u16* inputs = (u16*)event->data.adv.inputs;
    const int frame = event->data.adv.frame;

    p1sw_0 = PLsw[0][0] = inputs[0];
    p2sw_0 = PLsw[1][0] = inputs[1];
    p1sw_1 = PLsw[0][1] = recall_input(0, frame - 1);
    p2sw_1 = PLsw[1][1] = recall_input(1, frame - 1);

    note_input(inputs[0], 0, frame);
    note_input(inputs[1], 1, frame);

    step_game(render);
}

static void process_session() {
    frames_behind = -gekko_frames_ahead(session);

    gekko_network_poll(session);

    // GekkoNetworkStats stats;
    // gekko_network_stats(session, (player_handle == 0) ? 1 : 0, &stats);
    // printf("🛜 ping: %hu, avg ping: %.2f, jitter: %.2f\n", stats.last_ping, stats.avg_ping, stats.jitter);

    u16 local_inputs = get_inputs();
    gekko_add_local_input(session, player_handle, &local_inputs);

    int session_event_count = 0;
    GekkoSessionEvent** session_events = gekko_session_events(session, &session_event_count);

    for (int i = 0; i < session_event_count; i++) {
        const GekkoSessionEvent* event = session_events[i];

        switch (event->type) {
        case PlayerSyncing:
            printf("🔴 player syncing\n");
            // FIXME: Show status to the player
            break;

        case PlayerConnected:
            printf("🔴 player connected\n");
            break;

        case PlayerDisconnected:
            printf("🔴 player disconnected\n");
            // FIXME: Handle disconnection
            break;

        case SessionStarted:
            printf("🔴 session started\n");
            session_state = SESSION_RUNNING;
            break;

        case DesyncDetected: {
            const int frame = event->data.desynced.frame;
            printf("⚠️ desync detected at frame %d (local: 0x%08x, remote: 0x%08x)\n",
                   frame, event->data.desynced.local_checksum, event->data.desynced.remote_checksum);

#if defined(DEBUG)
            // Log per-section checksums to help narrow down the diverging subsystem
            const State* saved = &state_buffer[frame % STATE_BUFFER_MAX];
            SectionedChecksum sc = calculate_sectioned_checksums(saved);
            printf("  sections: plw0=0x%08x plw1=0x%08x bg=0x%08x tasks=0x%08x fx=0x%08x globals=0x%08x\n",
                   sc.plw0, sc.plw1, sc.bg, sc.tasks, sc.effects, sc.globals);
            dump_saved_state(frame);
#endif

            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Netplay",
                "Desync detected — the session will be terminated.", NULL);
            session_state = SESSION_EXITING;
            break;
        }

        case EmptySessionEvent:
        case SpectatorPaused:
        case SpectatorUnpaused:
            // Do nothing
            break;
        }
    }
}

static void process_events(bool drawing_allowed) {
    int game_event_count = 0;
    GekkoGameEvent** game_events = gekko_update_session(session, &game_event_count);

    for (int i = 0; i < game_event_count; i++) {
        const GekkoGameEvent* event = game_events[i];

        switch (event->type) {
        case LoadEvent:
            load_state_from_event(event);
            break;

        case AdvanceEvent:
            advance_game(event, drawing_allowed && !event->data.adv.rolling_back);
            break;

        case SaveEvent:
            save_state(event);
            break;

        case EmptyGameEvent:
            // Do nothing
            break;
        }
    }
}

static void step_logic(bool drawing_allowed) {
    process_session();
    process_events(drawing_allowed);
}

static void run_netplay() {
    const bool catch_up = need_to_catch_up() && (frame_skip_timer == 0);
    step_logic(!catch_up);

    if (catch_up) {
        step_logic(true);
        frame_skip_timer = 60; // Allow skipping a frame roughly every second
    }

    frame_skip_timer -= 1;

    if (frame_skip_timer < 0) {
        frame_skip_timer = 0;
    }
}

void Netplay_SetParams(int player, const char* ip) {
    SDL_assert(player == 1 || player == 2);
    player_number = player - 1;
    remote_ip = ip;

    if (SDL_strcmp(ip, "127.0.0.1") == 0) {
        switch (player_number) {
        case 0:
            local_port = 50000;
            remote_port = 50001;
            break;

        case 1:
            local_port = 50001;
            remote_port = 50000;
            break;
        }
    } else {
        local_port = 50000;
        remote_port = 50000;
    }
}

void Netplay_Begin() {
    setup_vs_mode();
    session_state = SESSION_TRANSITIONING;
}

void Netplay_Run() {
    switch (session_state) {
    case SESSION_TRANSITIONING:
        if (!game_ready_to_run_character_select()) {
            step_game(true);
        } else {
            configure_gekko();
            session_state = SESSION_CONNECTING;
        }

        break;

    case SESSION_CONNECTING:
    case SESSION_RUNNING:
        run_netplay();
        break;

    case SESSION_EXITING:
        if (session != NULL) {
            // cleanup session and then return to idle
            gekko_destroy(&session);
            // also cleanup default socket.
            #ifndef LOSSY_ADAPTER
            gekko_default_adapter_destroy();
            #endif
            
        }
        session_state = SESSION_IDLE;
        break;

    case SESSION_IDLE:
        break;
    }
}

bool Netplay_IsRunning() {
    return session_state != SESSION_IDLE;
}

void Netplay_HandleMenuExit() {
    session_state = SESSION_EXITING;
}
