#include "netplay/netplay.h"
#include "netplay/game_state.h"
#include "netplay/saved_work.h"
#include "sf33rd/Source/Game/Game.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/main.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/select_timer.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/utils/djb2_hash.h"
#include "types.h"

#include <stdbool.h>

#include "gekkonet.h"
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
} SessionState;

typedef struct EffectState {
    s16 frwctr;
    s16 frwctr_min;
    s16 head_ix[8];
    s16 tail_ix[8];
    s16 exec_tm[8];
    SavedWORK frw[EFFECT_MAX];
    s16 frwque[EFFECT_MAX];
} EffectState;

typedef struct State {
    GameState gs;
    EffectState es;
} State;

static GekkoSession* session = NULL;
static unsigned short local_port = 0;
static unsigned short remote_port = 0;
static int player_number = 0;
static int player_handle = 0;
static SessionState session_state = SESSION_IDLE;
static u16 input_history[2][INPUT_HISTORY_MAX] = { 0 };
static float frames_behind = 0;
static int frame_skip_timer = 0;

#if defined(DEBUG)
#define STATE_BUFFER_MAX 20

static State state_buffer[STATE_BUFFER_MAX] = { 0 };
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

static void setup_vs_mode() {
    // This is pretty much a copy of logic from menu.c
    task[TASK_MENU].r_no[0] = 5; // go to idle routine (doing nothing)
    Menu_Suicide[0] = 1;
    cpExitTask(TASK_SAVER);

    // From Mode_Select in menu.c
    Present_Mode = 1;
    E_No[0] = 1;
    E_No[1] = 2;
    E_No[2] = 2;
    E_No[3] = 0;

    for (int i = 0; i < 4; i++) {
        Menu_Suicide[i] = 0;
    }

    Clear_Personal_Data(0);
    Clear_Personal_Data(1);
    Menu_Cursor_Y[0] = 0;
    Cursor_Y_Pos[0][1] = 0;
    Cursor_Y_Pos[0][2] = 0;
    Cursor_Y_Pos[0][3] = 0;

    for (int i = 0; i < 4; i++) {
        Vital_Handicap[i][0] = 7;
        Vital_Handicap[i][1] = 7;
    }
    VS_Stage = 0x14;

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

    // Stop game task. We'll run game logic manually
    task[TASK_GAME].condition = 3;

    E_Timer = 0; // E_Timer can have different values depending on when the session was initiated
    select_timer_state_reset();
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

    gekko_create(&session);
    gekko_start(session, &config);

#if defined(LOSSY_ADAPTER)
    configure_lossy_adapter();
    gekko_net_adapter_set(session, &lossy_adapter);
#else
    gekko_net_adapter_set(session, gekko_default_adapter(local_port));
#endif

    G_Timer = 0;

    printf("starting a session for player %d at port %hu\n", player_number, local_port);

    char remote_address_str[100];
    SDL_snprintf(remote_address_str, sizeof(remote_address_str), "127.0.0.1:%hu", remote_port);
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
static uint32_t calculate_checksum(const State* state) {
    uint32_t hash = djb2_init();
    hash = djb2_updatep(hash, state);
    return hash;
}

/// Zero out all pointers in WORK for dumping
static void clean_work_pointers(WORK* work) {
    work->target_adrs = NULL;
    work->hit_adrs = NULL;
    work->dmg_adrs = NULL;
    work->suzi_offset = NULL;
    SDL_zeroa(work->char_table);
    work->se_random_table = NULL;
    work->step_xy_table = NULL;
    work->move_xy_table = NULL;
    work->overlap_char_tbl = NULL;
    work->olc_ix_table = NULL;
    work->rival_catch_tbl = NULL;
    work->curr_rca = NULL;
    work->set_char_ad = NULL;
    work->hit_ix_table = NULL;
    work->body_adrs = NULL;
    work->h_bod = NULL;
    work->hand_adrs = NULL;
    work->h_han = NULL;
    work->dumm_adrs = NULL;
    work->h_dumm = NULL;
    work->catch_adrs = NULL;
    work->h_cat = NULL;
    work->caught_adrs = NULL;
    work->h_cau = NULL;
    work->attack_adrs = NULL;
    work->h_att = NULL;
    work->h_eat = NULL;
    work->hosei_adrs = NULL;
    work->h_hos = NULL;
    work->att_ix_table = NULL;
    work->my_effadrs = NULL;
}

static void clean_plw_pointers(PLW* plw) {
    clean_work_pointers(&plw->wu);
    plw->cp = NULL;
    plw->dm_step_tbl = NULL;
    plw->as = NULL;
    plw->sa = NULL;
    plw->py = NULL;
}

static void clean_state_pointers(State* state) {
    for (int i = 0; i < 2; i++) {
        clean_plw_pointers(&state->gs.plw[i]);
    }

    for (int i = 0; i < EFFECT_MAX; i++) {
        WORK* work = &state->es.frw[i];
        clean_work_pointers(work);

        WORK_Other* work_big = (WORK_Other*)&state->es.frw[i];
        work_big->my_master = NULL;
    }
}

/// Save state in state buffer.
/// @return Pointer to state as it has been saved.
static const State* note_state(const State* state, int frame) {
    if (frame < 0) {
        frame += STATE_BUFFER_MAX;
    }

    State* dst = &state_buffer[frame % STATE_BUFFER_MAX];
    SDL_memcpy(dst, state, sizeof(State));
    clean_state_pointers(dst);
    return dst;
}

static void dump_state(int frame) {
    State* src = &state_buffer[frame % STATE_BUFFER_MAX];

    char filename[100];
    SDL_snprintf(filename, sizeof(filename), "states/%d_%d", player_handle, frame);

    SDL_IOStream* io = SDL_IOFromFile(filename, "w");
    SDL_WriteIO(io, src, sizeof(State));
    SDL_CloseIO(io);
}
#endif

#define SDL_copya(dst, src) SDL_memcpy(dst, src, sizeof(src))

static void save_state(GekkoGameEvent* event) {
    *event->data.save.state_len = sizeof(State);
    State* dst = (State*)event->data.save.state;

    // GameState
    GameState* gs = &dst->gs;
    GameState_Save(gs);

    // EffectState
    EffectState* es = &dst->es;
    for (int i = 0; i < EFFECT_MAX; i++) {
        SavedWORK* saved_work = &es->frw[i];
        const WORK* work = (WORK*)frw[i];

        if (work == NULL) {
            SDL_zero(*saved_work);
            continue;
        }

        saved_work->be_flag = work->be_flag;
        saved_work->disp_flag = work->disp_flag;
        saved_work->blink_timing = work->blink_timing;
        saved_work->operator = work->operator;
        saved_work->type = work->type;
        saved_work->charset_id = work->charset_id;
        saved_work->work_id = work->work_id;
        saved_work->id = work->id;
        saved_work->rl_flag = work->rl_flag;
        saved_work->rl_waza = work->rl_waza;
        saved_work->before = work->before;
        saved_work->myself = work->myself;
        saved_work->behind = work->behind;
        saved_work->listix = work->listix;
        saved_work->dead_f = work->dead_f;
        saved_work->timing = work->timing;
        SDL_copya(saved_work->routine_no, work->routine_no);
        SDL_copya(saved_work->old_rno, work->old_rno);
        saved_work->hit_stop = work->hit_stop;
        saved_work->hit_quake = work->hit_quake;
        saved_work->cgromtype = work->cgromtype;
        saved_work->kage_flag = work->kage_flag;
        saved_work->kage_hx = work->kage_hx;
        saved_work->kage_hy = work->kage_hy;
        saved_work->kage_prio = work->kage_prio;
        saved_work->kage_width = work->kage_width;
        saved_work->kage_char = work->kage_char;
        saved_work->position_x = work->position_x;
        saved_work->position_y = work->position_y;
        saved_work->position_z = work->position_z;
        saved_work->next_x = work->next_x;
        saved_work->next_y = work->next_y;
        saved_work->next_z = work->next_z;
        SDL_copya(saved_work->xyz, work->xyz);
        SDL_copya(saved_work->old_pos, work->old_pos);
        saved_work->sync_suzi = work->sync_suzi;
        saved_work->mvxy = work->mvxy;
        saved_work->direction = work->direction;
        saved_work->dir_old = work->dir_old;
        saved_work->dir_step = work->dir_step;
        saved_work->dir_timer = work->dir_timer;
        saved_work->vitality = work->vitality;
        saved_work->vital_new = work->vital_new;
        saved_work->vital_old = work->vital_old;
        saved_work->dm_vital = work->dm_vital;
        saved_work->dmcal_m = work->dmcal_m;
        saved_work->dmcal_d = work->dmcal_d;
        saved_work->weight_level = work->weight_level;
        saved_work->cmoa = work->cmoa;
        saved_work->cmsw = work->cmsw;
        saved_work->cmlp = work->cmlp;
        saved_work->cml2 = work->cml2;
        saved_work->cmja = work->cmja;
        saved_work->cmj2 = work->cmj2;
        saved_work->cmj3 = work->cmj3;
        saved_work->cmj4 = work->cmj4;
        saved_work->cmj5 = work->cmj5;
        saved_work->cmj6 = work->cmj6;
        saved_work->cmj7 = work->cmj7;
        saved_work->cmms = work->cmms;
        saved_work->cmmd = work->cmmd;
        saved_work->cmyd = work->cmyd;
        saved_work->cmcf = work->cmcf;
        saved_work->cmcr = work->cmcr;
        saved_work->cmbk = work->cmbk;
        saved_work->cmb2 = work->cmb2;
        saved_work->cmb3 = work->cmb3;
        saved_work->cmhs = work->cmhs;
        saved_work->cmr0 = work->cmr0;
        saved_work->cmr1 = work->cmr1;
        saved_work->cmr2 = work->cmr2;
        saved_work->cmr3 = work->cmr3;
        SDL_copya(saved_work->cmwk, work->cmwk);
        saved_work->cg_olc = work->cg_olc;
        saved_work->cg_ix = work->cg_ix;
        saved_work->now_koc = work->now_koc;
        saved_work->char_index = work->char_index;
        saved_work->current_colcd = work->current_colcd;
        saved_work->cgd_type = work->cgd_type;
        saved_work->pat_status = work->pat_status;
        saved_work->kind_of_waza = work->kind_of_waza;
        saved_work->hit_range = work->hit_range;
        saved_work->total_paring = work->total_paring;
        saved_work->total_att_set = work->total_att_set;
        saved_work->sp_tech_id = work->sp_tech_id;
        saved_work->cg_type = work->cg_type;
        saved_work->cg_ctr = work->cg_ctr;
        saved_work->cg_se = work->cg_se;
        saved_work->cg_olc_ix = work->cg_olc_ix;
        saved_work->cg_number = work->cg_number;
        saved_work->cg_hit_ix = work->cg_hit_ix;
        saved_work->cg_att_ix = work->cg_att_ix;
        saved_work->cg_extdat = work->cg_extdat;
        saved_work->cg_cancel = work->cg_cancel;
        saved_work->cg_effect = work->cg_effect;
        saved_work->cg_eftype = work->cg_eftype;
        saved_work->cg_zoom = work->cg_zoom;
        saved_work->cg_rival = work->cg_rival;
        saved_work->cg_add_xy = work->cg_add_xy;
        saved_work->cg_next_ix = work->cg_next_ix;
        saved_work->cg_status = work->cg_status;
        saved_work->cg_wca_ix = work->cg_wca_ix;
        saved_work->cg_jphos = work->cg_jphos;
        saved_work->cg_meoshi = work->cg_meoshi;
        saved_work->cg_prio = work->cg_prio;
        saved_work->cg_flip = work->cg_flip;
        saved_work->old_cgnum = work->old_cgnum;
        saved_work->floor = work->floor;
        saved_work->ccoff = work->ccoff;
        saved_work->colcd = work->colcd;
        saved_work->my_col_mode = work->my_col_mode;
        saved_work->my_col_code = work->my_col_code;
        saved_work->my_priority = work->my_priority;
        saved_work->my_family = work->my_family;
        saved_work->my_ext_pri = work->my_ext_pri;
        saved_work->my_bright_type = work->my_bright_type;
        saved_work->my_bright_level = work->my_bright_level;
        saved_work->my_clear_level = work->my_clear_level;
        saved_work->my_mts = work->my_mts;
        saved_work->my_mr_flag = work->my_mr_flag;
        saved_work->my_mr.size.x = work->my_mr.size.x;
        saved_work->my_mr.size.y = work->my_mr.size.y;
        saved_work->my_trans_mode = work->my_trans_mode;
        saved_work->waku_work_index = work->waku_work_index;
        SDL_copya(saved_work->olc_work_ix, work->olc_work_ix);
        saved_work->cg_ja = work->cg_ja;
        saved_work->att = work->att;
        saved_work->zu_flag = work->zu_flag;
        saved_work->at_attribute = work->at_attribute;
        saved_work->kezuri_pow = work->kezuri_pow;
        saved_work->add_arts_point = work->add_arts_point;
        saved_work->buttobi_type = work->buttobi_type;
        saved_work->att_zuru = work->att_zuru;
        saved_work->at_ten_ix = work->at_ten_ix;
        saved_work->dir_atthit = work->dir_atthit;
        saved_work->vs_id = work->vs_id;
        saved_work->att_hit_ok = work->att_hit_ok;
        saved_work->meoshi_hit_flag = work->meoshi_hit_flag;
        saved_work->at_koa = work->at_koa;
        saved_work->paring_attack_flag = work->paring_attack_flag;
        saved_work->no_death_attack = work->no_death_attack;
        saved_work->jump_att_flag = work->jump_att_flag;
        saved_work->shell_vs_refrect = work->shell_vs_refrect;
        saved_work->renew_attack = work->renew_attack;
        saved_work->attack_num = work->attack_num;
        SDL_copya(saved_work->uketa_att, work->uketa_att);
        saved_work->hf.hit.player = work->hf.hit.player;
        saved_work->hf.hit.effect = work->hf.hit.effect;
        saved_work->hit_mark_x = work->hit_mark_x;
        saved_work->hit_mark_y = work->hit_mark_y;
        saved_work->hit_mark_z = work->hit_mark_z;
        saved_work->kohm = work->kohm;
        saved_work->dm_fushin = work->dm_fushin;
        saved_work->dm_weight = work->dm_weight;
        saved_work->dm_butt_type = work->dm_butt_type;
        saved_work->dm_zuru = work->dm_zuru;
        saved_work->dm_attribute = work->dm_attribute;
        saved_work->dm_guard_success = work->dm_guard_success;
        saved_work->dm_plnum = work->dm_plnum;
        saved_work->dm_attlv = work->dm_attlv;
        saved_work->dm_dir = work->dm_dir;
        saved_work->dm_rl = work->dm_rl;
        saved_work->dm_impact = work->dm_impact;
        saved_work->dm_stop = work->dm_stop;
        saved_work->dm_quake = work->dm_quake;
        saved_work->dm_piyo = work->dm_piyo;
        saved_work->dm_ten_ix = work->dm_ten_ix;
        saved_work->dm_koa = work->dm_koa;
        saved_work->dm_work_id = work->dm_work_id;
        saved_work->dm_arts_point = work->dm_arts_point;
        saved_work->dm_jump_att_flag = work->dm_jump_att_flag;
        saved_work->dm_free = work->dm_free;
        saved_work->dm_count_up = work->dm_count_up;
        saved_work->dm_nodeathattack = work->dm_nodeathattack;
        saved_work->dm_exdm_ix = work->dm_exdm_ix;
        saved_work->dm_dip = work->dm_dip;
        saved_work->dm_kind_of_waza = work->dm_kind_of_waza;
        saved_work->attpow = work->attpow;
        saved_work->defpow = work->defpow;
        SDL_copya(saved_work->shell_ix, work->shell_ix);
        saved_work->hm_dm_side = work->hm_dm_side;
        saved_work->extra_col = work->extra_col;
        saved_work->extra_col_2 = work->extra_col_2;
        saved_work->original_vitality = work->original_vitality;
        saved_work->hit_work_id = work->hit_work_id;
        saved_work->dmg_work_id = work->dmg_work_id;
        saved_work->K5_init_flag = work->K5_init_flag;
        saved_work->K5_exec_ok = work->K5_exec_ok;
        saved_work->kow = work->kow;
        saved_work->swallow_no_effect = work->swallow_no_effect;
        saved_work->E3_work_index = work->E3_work_index;
        saved_work->E4_work_index = work->E4_work_index;
        saved_work->kezurare_flag = work->kezurare_flag;
        SDL_copya(saved_work->wrd_free, work->wrd_free);
    }
    SDL_copya(es->exec_tm, exec_tm);
    SDL_copya(es->frwque, frwque);
    SDL_copya(es->head_ix, head_ix);
    SDL_copya(es->tail_ix, tail_ix);
    es->frwctr = frwctr;
    es->frwctr_min = frwctr_min;

#if defined(DEBUG)
    const int frame = event->data.save.frame;
    const State* saved_state = note_state(dst, frame);
    *event->data.save.checksum = calculate_checksum(saved_state);
#endif
}

static void load_state(GekkoGameEvent* event) {
    const State* src = (State*)event->data.load.state;

    // GameState
    const GameState* gs = &src->gs;
    GameState_Load(gs);

    // EffectState
    const EffectState* es = &src->es;
    for (int i = 0; i < EFFECT_MAX; i++) {
        const SavedWORK* saved_work = &es->frw[i];
        WORK* work = (WORK*)frw[i];

        if (work == NULL) {
            continue;
        }

        work->be_flag = saved_work->be_flag;
        work->disp_flag = saved_work->disp_flag;
        work->blink_timing = saved_work->blink_timing;
        work->operator = saved_work->operator;
        work->type = saved_work->type;
        work->charset_id = saved_work->charset_id;
        work->work_id = saved_work->work_id;
        work->id = saved_work->id;
        work->rl_flag = saved_work->rl_flag;
        work->rl_waza = saved_work->rl_waza;
        work->before = saved_work->before;
        work->myself = saved_work->myself;
        work->behind = saved_work->behind;
        work->listix = saved_work->listix;
        work->dead_f = saved_work->dead_f;
        work->timing = saved_work->timing;
        SDL_copya(work->routine_no, saved_work->routine_no);
        SDL_copya(work->old_rno, saved_work->old_rno);
        work->hit_stop = saved_work->hit_stop;
        work->hit_quake = saved_work->hit_quake;
        work->cgromtype = saved_work->cgromtype;
        work->kage_flag = saved_work->kage_flag;
        work->kage_hx = saved_work->kage_hx;
        work->kage_hy = saved_work->kage_hy;
        work->kage_prio = saved_work->kage_prio;
        work->kage_width = saved_work->kage_width;
        work->kage_char = saved_work->kage_char;
        work->position_x = saved_work->position_x;
        work->position_y = saved_work->position_y;
        work->position_z = saved_work->position_z;
        work->next_x = saved_work->next_x;
        work->next_y = saved_work->next_y;
        work->next_z = saved_work->next_z;
        SDL_copya(work->xyz, saved_work->xyz);
        SDL_copya(work->old_pos, saved_work->old_pos);
        work->sync_suzi = saved_work->sync_suzi;
        work->mvxy = saved_work->mvxy;
        work->direction = saved_work->direction;
        work->dir_old = saved_work->dir_old;
        work->dir_step = saved_work->dir_step;
        work->dir_timer = saved_work->dir_timer;
        work->vitality = saved_work->vitality;
        work->vital_new = saved_work->vital_new;
        work->vital_old = saved_work->vital_old;
        work->dm_vital = saved_work->dm_vital;
        work->dmcal_m = saved_work->dmcal_m;
        work->dmcal_d = saved_work->dmcal_d;
        work->weight_level = saved_work->weight_level;
        work->cmoa = saved_work->cmoa;
        work->cmsw = saved_work->cmsw;
        work->cmlp = saved_work->cmlp;
        work->cml2 = saved_work->cml2;
        work->cmja = saved_work->cmja;
        work->cmj2 = saved_work->cmj2;
        work->cmj3 = saved_work->cmj3;
        work->cmj4 = saved_work->cmj4;
        work->cmj5 = saved_work->cmj5;
        work->cmj6 = saved_work->cmj6;
        work->cmj7 = saved_work->cmj7;
        work->cmms = saved_work->cmms;
        work->cmmd = saved_work->cmmd;
        work->cmyd = saved_work->cmyd;
        work->cmcf = saved_work->cmcf;
        work->cmcr = saved_work->cmcr;
        work->cmbk = saved_work->cmbk;
        work->cmb2 = saved_work->cmb2;
        work->cmb3 = saved_work->cmb3;
        work->cmhs = saved_work->cmhs;
        work->cmr0 = saved_work->cmr0;
        work->cmr1 = saved_work->cmr1;
        work->cmr2 = saved_work->cmr2;
        work->cmr3 = saved_work->cmr3;
        SDL_copya(work->cmwk, saved_work->cmwk);
        work->cg_olc = saved_work->cg_olc;
        work->cg_ix = saved_work->cg_ix;
        work->now_koc = saved_work->now_koc;
        work->char_index = saved_work->char_index;
        work->current_colcd = saved_work->current_colcd;
        work->cgd_type = saved_work->cgd_type;
        work->pat_status = saved_work->pat_status;
        work->kind_of_waza = saved_work->kind_of_waza;
        work->hit_range = saved_work->hit_range;
        work->total_paring = saved_work->total_paring;
        work->total_att_set = saved_work->total_att_set;
        work->sp_tech_id = saved_work->sp_tech_id;
        work->cg_type = saved_work->cg_type;
        work->cg_ctr = saved_work->cg_ctr;
        work->cg_se = saved_work->cg_se;
        work->cg_olc_ix = saved_work->cg_olc_ix;
        work->cg_number = saved_work->cg_number;
        work->cg_hit_ix = saved_work->cg_hit_ix;
        work->cg_att_ix = saved_work->cg_att_ix;
        work->cg_extdat = saved_work->cg_extdat;
        work->cg_cancel = saved_work->cg_cancel;
        work->cg_effect = saved_work->cg_effect;
        work->cg_eftype = saved_work->cg_eftype;
        work->cg_zoom = saved_work->cg_zoom;
        work->cg_rival = saved_work->cg_rival;
        work->cg_add_xy = saved_work->cg_add_xy;
        work->cg_next_ix = saved_work->cg_next_ix;
        work->cg_status = saved_work->cg_status;
        work->cg_wca_ix = saved_work->cg_wca_ix;
        work->cg_jphos = saved_work->cg_jphos;
        work->cg_meoshi = saved_work->cg_meoshi;
        work->cg_prio = saved_work->cg_prio;
        work->cg_flip = saved_work->cg_flip;
        work->old_cgnum = saved_work->old_cgnum;
        work->floor = saved_work->floor;
        work->ccoff = saved_work->ccoff;
        work->colcd = saved_work->colcd;
        work->my_col_mode = saved_work->my_col_mode;
        work->my_col_code = saved_work->my_col_code;
        work->my_priority = saved_work->my_priority;
        work->my_family = saved_work->my_family;
        work->my_ext_pri = saved_work->my_ext_pri;
        work->my_bright_type = saved_work->my_bright_type;
        work->my_bright_level = saved_work->my_bright_level;
        work->my_clear_level = saved_work->my_clear_level;
        work->my_mts = saved_work->my_mts;
        work->my_mr_flag = saved_work->my_mr_flag;
        work->my_mr.size.x = saved_work->my_mr.size.x;
        work->my_mr.size.y = saved_work->my_mr.size.y;
        work->my_trans_mode = saved_work->my_trans_mode;
        work->waku_work_index = saved_work->waku_work_index;
        SDL_copya(work->olc_work_ix, saved_work->olc_work_ix);
        work->cg_ja = saved_work->cg_ja;
        work->att = saved_work->att;
        work->zu_flag = saved_work->zu_flag;
        work->at_attribute = saved_work->at_attribute;
        work->kezuri_pow = saved_work->kezuri_pow;
        work->add_arts_point = saved_work->add_arts_point;
        work->buttobi_type = saved_work->buttobi_type;
        work->att_zuru = saved_work->att_zuru;
        work->at_ten_ix = saved_work->at_ten_ix;
        work->dir_atthit = saved_work->dir_atthit;
        work->vs_id = saved_work->vs_id;
        work->att_hit_ok = saved_work->att_hit_ok;
        work->meoshi_hit_flag = saved_work->meoshi_hit_flag;
        work->at_koa = saved_work->at_koa;
        work->paring_attack_flag = saved_work->paring_attack_flag;
        work->no_death_attack = saved_work->no_death_attack;
        work->jump_att_flag = saved_work->jump_att_flag;
        work->shell_vs_refrect = saved_work->shell_vs_refrect;
        work->renew_attack = saved_work->renew_attack;
        work->attack_num = saved_work->attack_num;
        SDL_copya(work->uketa_att, saved_work->uketa_att);
        work->hf.hit.player = saved_work->hf.hit.player;
        work->hf.hit.effect = saved_work->hf.hit.effect;
        work->hit_mark_x = saved_work->hit_mark_x;
        work->hit_mark_y = saved_work->hit_mark_y;
        work->hit_mark_z = saved_work->hit_mark_z;
        work->kohm = saved_work->kohm;
        work->dm_fushin = saved_work->dm_fushin;
        work->dm_weight = saved_work->dm_weight;
        work->dm_butt_type = saved_work->dm_butt_type;
        work->dm_zuru = saved_work->dm_zuru;
        work->dm_attribute = saved_work->dm_attribute;
        work->dm_guard_success = saved_work->dm_guard_success;
        work->dm_plnum = saved_work->dm_plnum;
        work->dm_attlv = saved_work->dm_attlv;
        work->dm_dir = saved_work->dm_dir;
        work->dm_rl = saved_work->dm_rl;
        work->dm_impact = saved_work->dm_impact;
        work->dm_stop = saved_work->dm_stop;
        work->dm_quake = saved_work->dm_quake;
        work->dm_piyo = saved_work->dm_piyo;
        work->dm_ten_ix = saved_work->dm_ten_ix;
        work->dm_koa = saved_work->dm_koa;
        work->dm_work_id = saved_work->dm_work_id;
        work->dm_arts_point = saved_work->dm_arts_point;
        work->dm_jump_att_flag = saved_work->dm_jump_att_flag;
        work->dm_free = saved_work->dm_free;
        work->dm_count_up = saved_work->dm_count_up;
        work->dm_nodeathattack = saved_work->dm_nodeathattack;
        work->dm_exdm_ix = saved_work->dm_exdm_ix;
        work->dm_dip = saved_work->dm_dip;
        work->dm_kind_of_waza = saved_work->dm_kind_of_waza;
        work->attpow = saved_work->attpow;
        work->defpow = saved_work->defpow;
        SDL_copya(work->shell_ix, saved_work->shell_ix);
        work->hm_dm_side = saved_work->hm_dm_side;
        work->extra_col = saved_work->extra_col;
        work->extra_col_2 = saved_work->extra_col_2;
        work->original_vitality = saved_work->original_vitality;
        work->hit_work_id = saved_work->hit_work_id;
        work->dmg_work_id = saved_work->dmg_work_id;
        work->K5_init_flag = saved_work->K5_init_flag;
        work->K5_exec_ok = saved_work->K5_exec_ok;
        work->kow = saved_work->kow;
        work->swallow_no_effect = saved_work->swallow_no_effect;
        work->E3_work_index = saved_work->E3_work_index;
        work->E4_work_index = saved_work->E4_work_index;
        work->kezurare_flag = saved_work->kezurare_flag;
        SDL_copya(work->wrd_free, saved_work->wrd_free);
    }
    SDL_copya(exec_tm, es->exec_tm);
    SDL_copya(frwque, es->frwque);
    SDL_copya(head_ix, es->head_ix);
    SDL_copya(tail_ix, es->tail_ix);
    frwctr = es->frwctr;
    frwctr_min = es->frwctr_min;
}

static bool game_ready_to_run_character_select() {
    return G_No[1] == 1;
}

static bool need_to_catch_up() {
    return frames_behind >= 1;
}

static void step_game(bool render) {
    if (render) {
        init_color_trans_req();
    }

    No_Trans = !render;
    Play_Game = 0;

    init_texcash_before_process();
    seqsBeforeProcess();

    Game();

    seqsAfterProcess();
    texture_cash_update();
    move_pulpul_work();
    Check_LDREQ_Queue();
}

static void advance_game(GekkoGameEvent* event, bool last_advance) {
    const u16* inputs = (u16*)event->data.adv.inputs;
    const int frame = event->data.adv.frame;
    p1sw_0 = inputs[0];
    p2sw_0 = inputs[1];

    p1sw_1 = recall_input(0, frame - 1);
    p2sw_1 = recall_input(1, frame - 1);

    note_input(inputs[0], 0, frame);
    note_input(inputs[1], 1, frame);

    step_game(last_advance);
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

        case DesyncDetected:
            const int frame = event->data.desynced.frame;
            printf("⚠️ desync detected at frame %d\n", frame);

#if defined(DEBUG)
            dump_state(frame);
#endif
            break;

        case EmptySessionEvent:
        case SpectatorPaused:
        case SpectatorUnpaused:
            // Do nothing
            break;
        }
    }
}

static int get_last_advance_index(GekkoGameEvent** events, int event_count) {
    for (int i = event_count - 1; i >= 0; i--) {
        if (events[i]->type == AdvanceEvent) {
            return i;
        }
    }

    return -1;
}

static void process_events() {
    int game_event_count = 0;
    GekkoGameEvent** game_events = gekko_update_session(session, &game_event_count);
    const int last_advance_index = get_last_advance_index(game_events, game_event_count);

    for (int i = 0; i < game_event_count; i++) {
        const GekkoGameEvent* event = game_events[i];

        switch (event->type) {
        case LoadEvent:
            load_state(event);
            break;

        case AdvanceEvent:
            const bool last_advance = (i == last_advance_index);
            advance_game(event, last_advance);
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

static void step_logic() {
    process_session();
    process_events();
}

static void run_netplay() {
    step_logic();

    if (need_to_catch_up() && (frame_skip_timer == 0)) {
        step_logic();
        frame_skip_timer = 60; // Allow skipping a frame roughly every second
    }

    frame_skip_timer -= 1;

    if (frame_skip_timer < 0) {
        frame_skip_timer = 0;
    }
}

void Netplay_SetPlayer(int player) {
    if (player == 1) {
        local_port = 50000;
        remote_port = 50001;
        player_number = 0;
    } else {
        local_port = 50001;
        remote_port = 50000;
        player_number = 1;
    }
}

int Netplay_GetPlayer() {
    return player_number + 1;
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

    case SESSION_IDLE:
        // Do nothing
        break;
    }
}
