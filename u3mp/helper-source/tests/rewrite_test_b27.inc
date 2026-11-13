// v7.140 call sequences, part 1: they call the game, so they cannot run here. Stand-ins for the C part's u3h_* exports
// let the test link; the other checks keep running unchanged.
static uint8_t t27_ext[0x6000];
extern "C" {
int u3h_show(uintptr_t, void *, uint32_t page, int silent) { rd_log("show", page, static_cast<uint32_t>(silent)); return 1; }
static uint32_t t27_show_dialog_calls;
void u3h_show_dialog(uintptr_t, void *, uint32_t body, uint32_t title, uint32_t action, int32_t buttons, uint32_t a, uint32_t b) { rd_log("dialog", static_cast<uint64_t>(body) << 32 | title, static_cast<uint64_t>(action) << 32 | static_cast<uint32_t>(buttons) ^ a ^ b); ++t27_show_dialog_calls; }
void u3h_lan_chosen(void) {}
int u3h_offline_find(uintptr_t, void *) { return 0; }
void u3h_onm_state(uintptr_t, uint32_t) {}
void u3h_fj_note(const char *a, const uint8_t *id, const char *b, int32_t v) { rd_log("fj_note", rd_text(a) ^ rd_text(b) * 3, rd_bytes(id, 0x14) ^ static_cast<uint32_t>(v)); }
uint8_t *u3h_ext(void) { return t27_ext; }
int u3h_party_send(uintptr_t, uint32_t, const void *, uint32_t) { return 0; }
uint8_t *u3h_nmh(uintptr_t) { return t27_ext; }
int u3h_lan_backend(void) { return 1; }
void *u3h_online_backend(uintptr_t) { return nullptr; }
void u3h_jip_started(void) {}
}
static void batch27() {}
extern "C" {
void u3h_fi_send(uintptr_t) {}
int32_t u3h_fi_selected(void) { return -1; }
void u3h_fi_select(int32_t) {}
void u3h_fi_delete(uintptr_t, int32_t) {}
int32_t u3h_mq_send(uintptr_t, const uint8_t *to, uint32_t type, const void *data, uint32_t length) { rd_log("mq_send", static_cast<uint64_t>(type) << 32 | length, rd_bytes(to, 0x14) ^ rd_bytes(data, length) * 3); return 0; }
void u3h_join_reset(void) {}
void u3h_join_waiting(uintptr_t) {}
void u3h_wait_popup(uintptr_t, uint32_t, uintptr_t) {}
int32_t u3h_presence(uintptr_t, const uint8_t *, uint8_t *) { return -1; }
int u3h_key_ok(uintptr_t, uint32_t) { return 0; }
}
extern "C" {
int u3h_rpm_catch(uintptr_t, void *, uint32_t) { return 0; }
int u3h_fr_catch(uintptr_t, void *, uint32_t, uintptr_t) { return 0; }
int u3h_start_match_catch(uintptr_t, void *, uint32_t) { return 0; }
int u3h_film_action(uintptr_t, const int32_t *) { return 0; }
int u3h_community_catch(uintptr_t, void *, uint32_t) { return 0; }
void u3h_treasure_first_row(void *) {}
void u3h_motd_ok(uintptr_t, void *) {}
void u3h_mission_take(uintptr_t, void *) {}
void u3h_party_kick_promote(uintptr_t, int) {}
void u3h_report_popup(uintptr_t, void *) {}
void u3h_report_pick(uintptr_t, void *, uint32_t) {}
void u3h_view_stats(uintptr_t, void *) {}
void u3h_lan_delete_profile(uintptr_t, void *, uint32_t) {}
const char *u3h_text(uintptr_t, void *, uint32_t id) {
    if (!rd_on) return "";
    rd_log("text", id);
    static char pool[64][96];
    char *t = pool[id % 64];   // the same text id gives the same pointer
    std::snprintf(t, sizeof pool[0], "t%08x", id);
    for (int k = 9; k < rd_text_len && k < 95; ++k) t[k] = static_cast<char>('a' + k % 26);
    t[rd_text_len > 9 && rd_text_len < 95 ? rd_text_len : 9] = 0;
    return t;
}
int u3h_publish(uintptr_t, void *, uint8_t *p) { rd_log("publish", p ? 1 : 0); return 0; }
}
extern "C" {
int u3h_mp_write(uintptr_t, void *, uint32_t, const uint8_t *) { return 0; }
void u3h_rank_fields(uint8_t *, uint32_t, int) {}
void u3h_server_queue(const uint8_t *) {}
int u3h_online_profile(void) { return 0; }
static uint32_t t27_m03[10];
uint32_t *u3h_m03(void) { return t27_m03; }
volatile uint32_t *u3h_m03_diag(void) { return t27_m03; }
void u3h_hud_alias_names(uintptr_t, void *) { rd_log("alias_names"); }
void u3h_stage_tick(uintptr_t, void *) { rd_log("stage_tick"); }
void u3h_loadout_tick(uintptr_t, void *) { rd_log("loadout_tick"); }
void u3h_rank_icon(uintptr_t, void *, int32_t *ev) { rd_log("rank_icon", static_cast<uint32_t>(ev[6])); }
uint32_t u3h_team_colour(uintptr_t, int32_t) { return 0; }
}
extern "C" {
int32_t u3h_cm_get(uintptr_t, const char *) { return -1; }
int32_t u3h_cm_release(uintptr_t, int32_t) { return 0; }
int32_t u3h_msg_send(uintptr_t, const uint8_t *id, uint32_t type, const void *, uint32_t n) { rd_log("msg_send", id ? id[0] : 0, (uint64_t)type << 16 | n); return -1; }
int u3h_gate_closed(uintptr_t, int32_t *) { return 1; }
uint8_t *u3h_cin_state(uintptr_t) { return t27_ext; }
char *u3h_cin_path(char *o, int, const char *, const char *) { o[0] = 0; return o; }
void u3h_cin_scan(uintptr_t, void *, uint8_t *) {}
void u3h_cin_flags(uint8_t *) {}
int u3h_cin_game(uintptr_t, void *, int16_t *, uint8_t *) { return 0; }
}
extern "C" {
uint8_t u3h_spot_flag(uintptr_t, uint8_t *) { return 0; }
int u3h_onb_mode(void) { return 0; }
uint32_t u3h_active_page(uintptr_t) { return rd_active_page; }
static volatile uint32_t t27_w10[4];
volatile uint32_t *u3h_w10_diag(void) { return t27_w10; }
uint64_t u3h_game_call(void *) { return 0; }
}
extern "C" {
void u3h_decode(void *, uint8_t *, uint8_t *, int32_t, int32_t) {}
int32_t u3h_pick_snapshot(uintptr_t, uint8_t *, uint32_t *) { return -1; }
uint64_t u3h_call_f32(uintptr_t f, uint32_t, uint64_t, uint64_t, uint64_t a, uint64_t) { rd_log("call_f32", f, a); return rd_call_f32_ret; }
uint32_t u3h_anim_pack(uintptr_t, uint8_t *, uint8_t *, uint8_t *, uint8_t **) { return 0; }
static uint32_t t27_err, t27_ew[4];
uint32_t *u3h_repl_error(void) { return &t27_err; }
void u3h_repl_loop(uintptr_t, uint8_t *pl, uint8_t **, uint8_t *, uint32_t bits) { rd_log("repl_loop", (uintptr_t)pl, bits); }
uint8_t *u3h_emb_team_player(uintptr_t, int32_t) { return nullptr; }
int u3h_emb_is_record(uint64_t) { return 0; }
uint32_t *u3h_emb_wall_diag(void) { return t27_ew; }
void u3h_rp_assert(uintptr_t) {}
}
extern "C" {
void u3h_v_pose(void *, uint32_t, uint8_t *, uint32_t) {}
void u3h_v_blend(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void u3h_v_ice(void *, uint16_t, uint32_t, uint64_t, uint64_t, uint64_t) {}
void u3h_v_bind(void *, uint32_t) {}
void u3h_v_error(void *, int) {}
uint8_t *u3h_net_art_group(uintptr_t, uint32_t, uint32_t) { return nullptr; }
uint8_t *u3h_anim_by_sid(uint8_t *, uint32_t) { return nullptr; }
int32_t u3h_skeleton(uint8_t *) { return 0; }
int u3h_join_locked(uintptr_t) { return 0; }
int u3h_join_can_accept(uint8_t *, uint32_t) { return 0; }
uint64_t u3h_join_party_reply(uintptr_t) { return 0; }
void u3h_join_info_send(uintptr_t, const uint8_t *) {}
void u3h_join_answer(uintptr_t, const uint8_t *, uint64_t, uint8_t) {}
static uint8_t t27_req[0x26];
uint8_t *u3h_join_request(void) { return t27_req; }
uint8_t *u3h_join_request_on(void) { return t27_req + 0x24; }
uint8_t *u3h_join_request_pc(void) { return t27_req + 0x25; }
int32_t u3h_msg_event(uintptr_t, uint8_t *, uint32_t *, uint8_t *, uint32_t) { return -1; }
int u3h_join_host_answer(uintptr_t, const uint8_t *, const uint8_t *, int32_t) { return 0; }
int u3h_join_msg_in(uintptr_t, const uint8_t *, uint32_t, const uint8_t *, int32_t) { return 0; }
}
extern "C" {
static uint8_t t27_rc[0x400];
void *u3h_rc(void *, int field) { return t27_rc + field * 16; }
uint64_t u3h_net_connect(uint32_t, uint32_t) { return ~uint64_t{0}; }
int u3h_net_sendall(uint64_t, const uint8_t *, int) { return 0; }
int u3h_net_recv(uint64_t, void *, int, int) { return 0; }
void u3h_net_close(uint64_t) {}
int u3h_net_error(void) { return 0; }
void u3h_net_nonblock(uint64_t, uint32_t) {}
void u3h_utc(uint16_t *) {}
int u3h_unseal(uint8_t *) { return 0; }
const char *u3h_profile_host(void) { return ""; }
int u3h_sm_recv(void *, uint64_t, uint8_t *, int, int) { return 0; }
uint32_t u3h_dns(void) { return 0; }
void u3h_sleep_ms(uint32_t) {}
int u3h_resolve(const char *, uint32_t *) { return -1; }
uint32_t u3h_parse_ipv4(const char *) { return 0; }
int u3h_hello(void *, uint64_t, const char *) { return -1; }
int u3h_xchg(void *, uint64_t, const uint8_t *, uint32_t, uint8_t *, uint32_t) { return -1; }
void u3h_so_note(const char *a, const char *b, int32_t v, const char *c) { rd_log("note", rd_text(a) ^ static_cast<uint32_t>(v), rd_text(b) ^ rd_text(c) * 3); }
void u3h_pf_fetch(void *, uint32_t) {}
void u3h_pf_note(void *, const char *) {}
}
extern "C" {
void u3h_cin_netinit(uintptr_t, void *) {}
void u3h_j07_rec(uintptr_t) {}
void u3h_cin_scan_free(uintptr_t, void *, uint8_t *) {}
void u3h_j07_zeroed(void) {}
int u3h_lan_party_size(uintptr_t) { return 1; }
const uint32_t *u3h_lobby_rows(uintptr_t, uint32_t *n) { *n = 0; return nullptr; }
void u3h_lnb_find_party(uintptr_t) {}
void u3h_splitscreen(uintptr_t, void *) {}
}
extern "C" {
void u3h_net_pick(void) {}
}
extern "C" {
static uint32_t t27_fr_tab_value;
uint32_t u3h_fr_tab(uintptr_t, void *) { rd_log("fr_tab"); return t27_fr_tab_value; }
void u3h_fi_mark_seen(uintptr_t, void *) {}
void u3h_fi_refresh(uintptr_t, void *, int) {}
static int (*t27_strcmp_fn)(const void *, const void *);
static uint8_t *rd_friends = nullptr;
uint8_t *u3h_fr_ent(uintptr_t, uint32_t k) { if (rd_on && rd_friends) { rd_log("fr_ent", k); return rd_friends + k * 0x20; } return t27_ext; }
int u3h_fr_strcmp(uintptr_t, const void *a, const void *b) {
    if (rd_on) return std::strcmp(static_cast<const char *>(a), static_cast<const char *>(b));
    return t27_strcmp_fn ? t27_strcmp_fn(a, b) : 1;
}
void u3h_fr_icon22(int32_t *, int32_t) {}
int32_t u3h_fr_state_icon(uintptr_t, uint8_t *) { return 0; }
static char t27_buf[0x100];
char *u3h_fr_buffer(void) { return t27_buf; }
uint64_t u3h_fi_time(int32_t) { return 0; }
static int32_t t27_sel;
int32_t *u3h_fi_list_selection(void) { return &t27_sel; }
const char *u3h_fi_ago(uintptr_t, void *, uint32_t) { return ""; }
}
extern "C" {
static uint8_t t27_f4[0x2600];
static uint32_t t27_f4c[16][8];
static uint8_t t27_spot[10];
uint8_t *u3h_f4_state(void) { return t27_f4; }
uint32_t *u3h_f4_current(void) { return &t27_f4c[0][0]; }
volatile uint8_t *u3h_f4_entry(uint8_t *) { return t27_f4 + 0x4d0; }
uint8_t *u3h_spot_flags(void) { return t27_spot; }
uint8_t *u3h_netgame(uintptr_t, void *) { return nullptr; }
}
extern "C" {
static uint8_t t27_adv[32];
static volatile uint8_t t27_elim;
int64_t u3h_lnb_call(void *, int) { return 0; }
void u3h_cm_lanaddr(uintptr_t, const char *, uint32_t) {}
volatile uint8_t *u3h_elim_received(void) { return &t27_elim; }
uint8_t *u3h_adv_rules(void) { return t27_adv; }
uint8_t *u3h_adv_received(void) { return t27_adv + 24; }
int u3h_sym_ready(uintptr_t, void *, uint32_t) { return 0; }
uint8_t *u3h_coop_weapon(void *, uint32_t) { return nullptr; }
void u3h_coop_mod_sound(uintptr_t, void *, uint32_t, uint32_t, uint8_t *, uint32_t, int32_t) {}
uint32_t u3h_mod_name(uint32_t) { return 0; }
uint32_t u3h_mod_desc(uint32_t) { return 0; }
int u3h_unlock_status(uintptr_t, void *, uint32_t, uint32_t, uint32_t) { return 0; }
void u3h_money_text(uintptr_t, void *, int32_t *) {}
uint8_t *u3h_weapon_def(void *, uint32_t) { return nullptr; }
void u3h_set_icon(uintptr_t, int32_t *, uint32_t) {}
uint32_t u3h_mod_icon(void *, uint32_t) { return 0; }
void u3h_lock_overlay(int32_t *) {}
void u3h_preview_mods(uintptr_t, uint32_t) {}
}
extern "C" {
static uint32_t t27_t009[16];
uint32_t u3h_sincos(uint32_t x, uint32_t cosine) { if (!rd_on) return 0; rd_log("sincos", x, cosine); return (x * 2654435761u) & 0x3f7fffffu; }   // a float in [0, 1) for the differential tests
void u3h_ring_colour(uint32_t *) {}
uint32_t *u3h_t009_diag(void) { return t27_t009; }
}
extern "C" {
static uint32_t t27_was;
uint8_t *u3h_netgame_entry(uintptr_t, void *) { return nullptr; }
int u3h_coop(uintptr_t, void *) { return 0; }
uint32_t *u3h_scene_was(void) { return &t27_was; }
uint8_t *u3h_cm_entry(uintptr_t, int i) { return t27_ext + i * 0x220; }
static uint8_t *rd_members[10];
static uint32_t rd_members_n = 0;
uint32_t u3h_members(uint8_t *, uint8_t **out) {
    rd_log("members");
    if (!rd_on) return 0;
    for (uint32_t i = 0; i < rd_members_n; ++i) out[i] = rd_members[i];
    return rd_members_n;
}
int u3h_link_timeout(uintptr_t, uint8_t *) { return 0; }
void u3h_sync_request(uintptr_t) {}
}
extern "C" {
uint32_t u3h_player_fill(uintptr_t, void *, uint8_t *, uint8_t *) { return 0; }
uint8_t *u3h_camera_manager(uintptr_t) { return nullptr; }
uint64_t u3h_rp_fcall(uint64_t *) { return 0; }
}
extern "C" {
uint8_t *u3h_friend_storage(int) { return t27_ext; }
int32_t u3h_friend_count(uintptr_t) { return -1; }
int32_t u3h_friend_get(uintptr_t, uint32_t, uint8_t *, uint8_t *) { return -1; }
int32_t u3h_presence_set(uintptr_t, const uint8_t *, uint32_t) { return -1; }
void u3h_fr_sort(uint8_t *, int64_t) {}
void u3h_fr_strncpy(char *, const char *, uint32_t) {}
static uint32_t t27_fr_key_value;
static uint8_t *t27_lg_profile_ptr, *t27_lg_rec_ptr;
uint32_t u3h_fr_key(uintptr_t) { rd_log("fr_key"); return t27_fr_key_value; }
uint8_t *u3h_lg_profile(uintptr_t, uint32_t) { return t27_lg_profile_ptr; }
uint8_t *u3h_lg_rec(uintptr_t, void *, uint8_t *) { rd_log("lg_rec"); return t27_lg_rec_ptr; }
int u3h_match_end_time(uintptr_t, void *, int64_t *) { return 0; }
}
extern "C" {
uint32_t u3h_kr_statw(uintptr_t, uint32_t w) { rd_log("kr_statw", w); return 0; }
static uint8_t *rd_kr_nps = nullptr;   // rd29: the net players (0x9c0 each) and their score records (0x80 each)
static uint8_t *rd_kr_recs = nullptr;
static int rd_kr_30feec = 0;
static int32_t rd_kr_mode_type = -1;
static uint64_t rd_np_index(const uint8_t *np) { return np && rd_kr_nps ? static_cast<uint64_t>((np - rd_kr_nps) / 0x9c0) : 0xff; }
void u3h_kr_stat(uintptr_t, void *, uint32_t id, uint32_t amount, uint8_t *np) { rd_log("kr_stat", id, static_cast<uint64_t>(amount) << 8 | rd_np_index(np)); }
uint8_t *u3h_kr_prec(uint8_t *, uint8_t *np) {
    if (!rd_on || !rd_kr_recs) return t27_ext;
    rd_log("kr_prec", rd_np_index(np));
    return np ? rd_kr_recs + rd_np_index(np) * 0x80 : rd_kr_recs + 10 * 0x80;
}
uint8_t *u3h_kr_np_by_id(uintptr_t, uint32_t id) {
    if (!rd_on || !rd_kr_nps) return nullptr;
    rd_log("kr_np", id);
    return id >= 0x40 && id < 0x4a ? rd_kr_nps + (id - 0x40) * 0x9c0 : nullptr;
}
}
extern "C" {
uint8_t *u3h_np119(void) { return t27_ext; }
void u3h_taunt_want(uintptr_t, uint32_t) {}
}
extern "C" {
void u3h_text_num(uintptr_t, void *, char *, uint32_t, uint32_t, int32_t) {}
int u3h_fr_in_session(uintptr_t, uint8_t *) { return 0; }
int u3h_fr_in_title(uint8_t *) { return 0; }
uint8_t *u3h_fr_pdata(uintptr_t, uint8_t *) { return nullptr; }
void u3h_fr_refresh(uintptr_t) {}
}
extern "C" {
int u3h_net_coop_mode(uintptr_t, void *) { return 0; }
int u3h_np_alive(void *, uint8_t *) { return 0; }
uint32_t u3h_respawn_delay(void *, uint8_t *) { return 0; }
int64_t u3h_f_ticks(uint32_t, int64_t) { return 0; }
}
extern "C" {
uint8_t *u3h_fr_find(uintptr_t, const uint8_t *) { return nullptr; }
int32_t u3h_pm_add(uintptr_t, const uint8_t *, const char *) { return 0; }
int32_t u3h_pm_count(uintptr_t) { return 0; }
int32_t u3h_pm_get(uintptr_t, uint32_t, uint8_t *) { return 0; }
int32_t u3h_fr_event(uintptr_t, uint8_t *) { return -1; }
int u3h_take_rebuild(void) { return 0; }
}
extern "C" {
static volatile uint32_t t27_s16[16];
static uint32_t t27_h08[5][16];
uint8_t *u3h_s16_char(uintptr_t, uint8_t *) { return nullptr; }
volatile uint32_t *u3h_s16_diag(void) { return t27_s16; }
static int rd_code_is = 0;   // differential tests: the answer while rd_on
int u3h_mp_code_is(uintptr_t, uint32_t off, const char *hex) { rd_log("code_is", off, rd_text(hex)); return rd_on ? rd_code_is : 0; }
static volatile uint32_t t27_i03_d[8];
volatile uint32_t *u3h_i03_d(void) { return t27_i03_d; }
uint32_t u3h_crc_sid(const char *p) { uint32_t c = 0; for (; *p; p++) { c ^= (uint32_t)(uint8_t)*p << 24; for (int b = 0; b < 8; b++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1); } return c; }
void u3h_h01_tint(uintptr_t, uint8_t *, uint8_t *, int32_t) {}
uint32_t *u3h_h08_state(int which) { return t27_h08[which]; }
}
extern "C" {
static uint32_t t27_sync[8][32];
uint32_t *u3h_time_sync(int which) { return t27_sync[which]; }
void u3h_sync_reset(void) {}
}
extern "C" {
void u3h_set_icon_plain(uintptr_t, int32_t *, uint32_t) {}
}
extern "C" {
static uint8_t t27_l01[6][0x80];
uint8_t *u3h_l01_state(int which) { return t27_l01[which]; }
int32_t u3h_icon_index(uintptr_t, uint32_t) { return -1; }
}
extern "C" {
static uint32_t t27_w03[8];
uint32_t u3h_h13_mode(void) { return 0; }
uint32_t *u3h_w03_diag(void) { return t27_w03; }
}
extern "C" {
int32_t u3h_kr_mode_type(uintptr_t) { rd_log("kr_mode_type"); return rd_on ? rd_kr_mode_type : -1; }
int u3h_kr_30feec(uintptr_t) { rd_log("kr_30feec"); return rd_on ? rd_kr_30feec : 0; }
}
extern "C" {
int u3h_cam_fixed(uintptr_t, void *, uint8_t *, int32_t) { return 0; }
}
extern "C" {
void *u3h_lnb_setup(uintptr_t, void *) { return nullptr; }
static void *rd_lnb_object = nullptr;
void *u3h_lnb_object(uintptr_t) { rd_log("lnb_object"); return rd_on ? rd_lnb_object : nullptr; }
int32_t u3h_gt_open(uintptr_t) { return 0; }
void u3h_gt_close(uintptr_t) {}
void u3h_cm_init_lan(uintptr_t) {}
void u3h_cm_shutdown(uintptr_t) {}
void *u3h_online_manager(uintptr_t) { return nullptr; }
int u3h_onb_lan_flag(void) { return 0; }
void u3h_lnb_frame_parts(uintptr_t, void *, int) {}
}
extern "C" {
int u3h_stage_unwant(uintptr_t, uint32_t) { return 0; }
}
extern "C" {
int32_t u3h_tab_find(uint8_t *, uint32_t) { return -1; }
static uint8_t *rd_art_group = nullptr;
uint8_t *u3h_find_art_group_name(uintptr_t, const char *name) { rd_log("art_group", name ? static_cast<uint8_t>(name[0]) : 0); return rd_on ? rd_art_group : nullptr; }
}
extern "C" {
void u3h_mp_quit(uintptr_t, void *) {}
int32_t u3h_server_notice(uintptr_t, int32_t *) { return -1; }
int u3h_chat_restricted(uintptr_t) { return 0; }
}
extern "C" {
static char t27_cin[2][0x200];
char *u3h_cin_cat(char *o, char *, const char *) { return o; }
char *u3h_cin_num(char *o, char *, int64_t, int) { return o; }
const int32_t *u3h_cin_localtime(const int64_t *) { return nullptr; }
void u3h_cin_megabytes(uintptr_t, void *, char *b, const uint8_t *) { b[0] = 0; }
char *u3h_cin_buffer(int which) { return t27_cin[which]; }
}
extern "C" {
int u3h_onb_shell_route(uintptr_t) { return 0; }
int u3h_online_config(void *) { return 0; }
int u3h_online_cfg_ok(void) { return 0; }
int32_t u3h_online_status(void) { return 0; }
static int rd_smn_up = 0, rd_ol_up_value = 1;
static const uint8_t *rd_smn_feed = nullptr;   // bytes the session socket delivers (rd_smn_feed_n)
static uint32_t rd_smn_feed_n = 0;
int u3h_smn_up(void) { rd_log("smn_up"); return rd_on ? rd_smn_up : 0; }
int u3h_rc_signing_in(void) { return 0; }
int u3h_offline_ok(void) { return 0; }
void u3h_go_offline(void) {}
void u3h_set_onb_mode(int) {}
int u3h_onb_mode_get(void) { return 0; }
int u3h_k01_play_online(uintptr_t, void *) { return 0; }
void u3h_k01_lan(void) {}
uint32_t u3h_k01_back_page(void) { return 0; }
void u3h_onp_name_apply(uintptr_t) {}
void u3h_k43_step(uintptr_t, uint32_t) {}
int u3h_onp_pending(void) { return 0; }
int u3h_srv_retry_pending(void) { return 0; }
int32_t u3h_rc_state(void) { return 0; }
uint32_t u3h_profile_state(int *h) { *h = 0; return 1; }
int u3h_rc_server_ready(void) { return 0; }
void u3h_profile_retry(void) {}
void *u3h_onb_setup(uintptr_t) { return nullptr; }
void u3h_cm_online_reset(uintptr_t) {}
void u3h_pl_open(uintptr_t) {}
}
extern "C" {
static uint8_t t27_er[24 * 0x50], t27_flag;
uint8_t *u3h_emb_np(uintptr_t, uint32_t) { return nullptr; }
void u3h_emb_new_records(uintptr_t, uint32_t, uint32_t) {}
void u3h_emb_set_record(uintptr_t, uint8_t *, int32_t, uint64_t) {}
void u3h_emb_free_record(uintptr_t, uint64_t) {}
uint8_t *u3h_emb_profile(uintptr_t, uint32_t) { return nullptr; }
void u3h_emb_bind(uintptr_t, void *) {}
void u3h_emb_frame_parts(uintptr_t, void *, int) {}
uint64_t u3h_emb_alloc_default(uintptr_t) { return 0; }
uint8_t *u3h_emb_records(uint8_t **flag) { *flag = &t27_flag; return t27_er; }
}
extern "C" {
static volatile uint32_t t27_bc[2];
void u3h_gt_stamp(uintptr_t base, uint8_t *sub, int off) { rd_log("gt_stamp", reinterpret_cast<uintptr_t>(sub) - base, static_cast<uint32_t>(off)); }
void u3h_gt_free(uintptr_t, void *) {}
volatile uint32_t *u3h_bc_state(int which) { return &t27_bc[which]; }
}
extern "C" {
int u3h_np_has_effect(void *, uint8_t *, uint32_t) { return 0; }
uint8_t *u3h_screen_player(uintptr_t, uint8_t *) { return nullptr; }
int u3h_net_late_joining(uintptr_t) { return 0; }
void u3h_spawn_fade(uintptr_t, void *, uint8_t *, int) {}
int u3h_spawn_locator(uintptr_t, void *, uint8_t *, void *, uint32_t, uint32_t) { return 0; }
void u3h_spawn_region_request(uintptr_t, void *, uint8_t *, uint32_t) {}
int u3h_spawn_levels_ready(uintptr_t, void *, uint32_t) { return 0; }
static int32_t t27_netgame_type_value;
int32_t u3h_netgame_type(uintptr_t, void *) { rd_log("ng_type"); return t27_netgame_type_value; }
int u3h_vis_oob_tick(uintptr_t, void *, uint8_t *, uint8_t *, int32_t, int) { return 0; }
void u3h_camera_tick(uintptr_t, void *, uint8_t *, int) {}
}
extern "C" {
static volatile uint32_t t27_m04[8];
int u3h_m04_lan(uintptr_t) { return 0; }
volatile uint32_t *u3h_m04_diag(void) { return t27_m04; }
}
extern "C" {
int u3h_profile_new_marker(uintptr_t, int32_t *) { return 0; }
const char *u3h_onp_id(void *) { return ""; }
int u3h_onm_players(uintptr_t, int32_t *, void *, const char *) { return 0; }
int u3h_i01_party(uintptr_t, int32_t *, void *, const char *) { return 0; }
}
extern "C" {
static void *rd_onb_mgr = nullptr;
void *u3h_onb_mgr(uintptr_t) { rd_log("onb_mgr"); return rd_on ? rd_onb_mgr : nullptr; }
void u3h_net_shutdown_parts(uintptr_t, void *) {}
void u3h_multi_all(uintptr_t, void *, int) {}
int u3h_stage_logout_hold(uintptr_t, void *, const char *) { return 0; }
void u3h_match_stage_cleanup(uintptr_t, void *, void *, void *) {}
}
extern "C" {
int u3h_j11_rowimg(int32_t *, uint32_t) { return 0; }
}
extern "C" {
int u3h_register_lobby_click(uintptr_t) { return 0; }
int u3h_onb_signin_wait(uintptr_t, void *) { return 0; }
void u3h_ngm_init(uintptr_t) {}
void u3h_netinit_services(uintptr_t, void *) {}
void u3h_lnb_netinit(uintptr_t, void *) {}
int u3h_onb_netinit(uintptr_t, void *) { return 0; }
void u3h_mp_profile_start(uintptr_t, void *) {}
int u3h_onp_fail_take(void) { return 0; }
int u3h_emb_netinit(uintptr_t, void *) { return 0; }
int u3h_k43_gate(uintptr_t, void *) { return 0; }
}
extern "C" {
uint8_t *u3h_emb_h(uintptr_t, void *, uintptr_t) { return nullptr; }
uint8_t *u3h_emb_cache(uintptr_t, const char *) { return nullptr; }
void *u3h_emb_alloc(uintptr_t, uint64_t) { return nullptr; }
void u3h_emb_free(uintptr_t, void *) {}
int u3h_emb_dds(uintptr_t, const char *, void *) { return 0; }
void u3h_emb_quad(uint8_t *, uint32_t, const void *, const uint32_t *, const void *, const uint32_t *) {}
void u3h_emb_decal(uint8_t *, uint32_t, const void *, const uint32_t *, const uint32_t *) {}
int u3h_emb_texcreate(uintptr_t, void *, const uint8_t *, uint32_t) { return 0; }
void *u3h_emb_ralloc(uint32_t) { return nullptr; }
uint32_t u3h_h16q16(uint32_t) { return 0; }
}
extern "C" {
uint32_t u3h_current_tab(uintptr_t, void *) { rd_log("current_tab"); return rd_current_tab; }
}
extern "C" {
uint8_t *u3h_stream_root(uintptr_t, uint8_t *, uint32_t *) { return nullptr; }
}
extern "C" {
void u3h_j05_inc(int) {}
void u3h_booster_icons(uintptr_t, void *, int32_t *, uint8_t *, uint32_t, uint32_t, int32_t, uint8_t *) {}
int u3h_k46_op4(uintptr_t, void *, int32_t *, uint8_t *, uint32_t, uint32_t, int32_t, uint8_t *) { return 0; }
void u3h_k49_booster(uintptr_t, void *, uint8_t *, uint32_t, uint32_t, int32_t, uint8_t *, int32_t) {}
}
extern "C" {
void u3h_onp_be2le(uint8_t *, const uint8_t *) {}
void u3h_pu_seal(uint8_t *) {}
int u3h_pf_unseal(uint8_t *) { return 0; }
const char *u3h_pu_key(int) { return ""; }
uint64_t u3h_nl_connect(uint32_t, uint32_t) { return ~0ull; }
int u3h_nl_sendall(uint64_t, const uint8_t *, int) { return 0; }
int u3h_sock_recv(uint64_t, void *, int) { return 0; }
void u3h_sock_close(uint64_t) {}
char *u3h_pu_why(void) { static char w[48]; return w; }
void *u3h_rc_mem(void) { return nullptr; }
void u3h_s19_count(int, uint32_t) {}
}
extern "C" {
void u3h_cs_tick(uintptr_t, void *) {}
uint8_t *u3h_custom_map(uintptr_t, void *, int32_t *) { return nullptr; }
}
extern "C" {
void u3h_rp_cam_mode12(uintptr_t, uint8_t *, uint8_t *) {}
void u3h_rp_render_vars(uintptr_t, uint8_t *) {}
void u3h_rp_cam_target(uintptr_t, uint8_t *) {}
}
extern "C" {
const char *u3h_fmt(void *, const char *f, int64_t a0, const char *, const char *, const char *) {
    if (!rd_on) return "";
    rd_log("fmt", f ? static_cast<uint64_t>(f[1]) << 32 | static_cast<uint8_t>(f[2]) : 0, static_cast<uint64_t>(a0));
    return "formatted";
}
void u3h_k49_kickback(uintptr_t, void *, uint32_t, int, int32_t) {}
}
extern "C" {
void *u3h_ona(void) { return nullptr; }
uint8_t *u3h_lb_buf(int) { return nullptr; }
int u3h_m02_lb_local(uintptr_t, void *, uint8_t *, uint32_t, int32_t) { return 0; }
uint32_t u3h_fr_lb_names(uintptr_t, char *, uint32_t) { return 0; }
void u3h_pm119(uintptr_t, void *, uint8_t *) {}
}
extern "C" {
void u3h_lnb_follow_tick(void *, uintptr_t, void *) {}
void u3h_lnb_cli_update(uintptr_t, void *) {}
void u3h_lnb_send_join(void *, void *, uint8_t *, int, int32_t *r) { *r = 0; }
void u3h_lnb_rank_stamp(void *, uint8_t *, uint32_t) {}
uint64_t *u3h_lnb_match_room(void) { static uint64_t r; return &r; }
uint8_t *u3h_lnb_udpbuf(void) { static uint8_t b[0x1000]; return b; }
}
extern "C" {
int32_t u3h_pm_type(uintptr_t, void *) { return 0; }
int u3h_pm_coop(uintptr_t, void *) { return 0; }
void u3h_ol01_tick(uintptr_t) {}
void u3h_rc_auto_tick(uintptr_t) {}
}
extern "C" {
void *u3h_emb_fallback(void) { static uint8_t f[80]; return f; }
void *u3h_emb_retire_queue(void) { static uint8_t q[16 * 24]; return q; }
const uint8_t *u3h_emb_zero(void) { static uint8_t z[64]; return z; }
}
extern "C" {
void u3h_n13_rearm(uintptr_t, void *) {}
}
// v7.211: the online backend's exports; the message buffer and the frames sent are kept for the b41 check
static uint8_t t41_msg[0x100], t41_object[0x1060], t41_frame[0x100];
static uint32_t t41_type, t41_len, t41_frames;
static uint64_t t41_pj[4];
static void *t41_vt[32];
extern "C" {
void *u3h_onb_object(void) { return t41_object; }
uint8_t *u3h_onb_msg(void) { return t41_msg; }
uint64_t *u3h_onb_pj(void) { return t41_pj; }
void **u3h_onb_vt(void) { return t41_vt; }
int u3h_smn_send(const void *, uint32_t) { return -1; }
void u3h_smn_attach(int) {}
int32_t u3h_smn_pull(uint8_t *rx, uint32_t *len, uint32_t cap) {
    rd_log("smn_pull", *len, cap);
    if (!rd_on) return -1;
    for (uint32_t i = 0; i < rd_smn_feed_n && *len < cap; ++i) rx[(*len)++] = rd_smn_feed[i];
    return 0;
}
// the framed size of a message (as the session wire would say): its room-message size; 999 unknown, 998 too big
static uint32_t rd_message_size(const uint8_t *m) {
    const uint32_t t = *reinterpret_cast<const uint32_t *>(m);
    switch (t) {
    case 305: return 160 + *reinterpret_cast<const uint16_t *>(m + 26) * 104u;
    case 306: return 120 + (m[7] & 8);   // sometimes framed larger than the message
    case 308: return 24;
    case 310: return 16 + *reinterpret_cast<const uint32_t *>(m + 12) * 56u;
    case 315: return 80;
    case 324: return 144;
    case 999: return 0;
    case 998: return 0x2000;
    case 997: return 0x1000;   // exactly the receive buffer
    default: return 16;
    }
}
uint32_t u3h_smn_len(const uint8_t *m, uint32_t n) { rd_log("smn_len", n); return rd_on ? rd_message_size(m) : 0; }
uint32_t u3h_smn_swap(uint8_t *m, uint32_t len) { rd_log("smn_swap", len); return rd_on ? *reinterpret_cast<uint32_t *>(m) ^ 0x1000u : 0; }
void u3h_smn_got(const uint8_t *, uint32_t id) { rd_log("smn_got", id); }
void u3h_smn_unknown(const uint8_t *, uint32_t n) { rd_log("smn_unknown", n); }
int u3h_onb_pl_np(uintptr_t base, uint8_t *e, const char *name) { rd_log("pl_np", reinterpret_cast<uintptr_t>(e) - base, rd_text(name)); return 0; }
int u3h_ol_up(void) { rd_log("ol_up"); return rd_on ? rd_ol_up_value : 1; }
int u3h_ol_frame(uint32_t type, const void *m, uint32_t n) { t41_type = type; t41_len = n; memcpy(t41_frame, m, n); t41_frames++; return 1; }
void u3h_fw_line(const char *a, uint32_t hex, int digits, const char *b, int32_t v) { rd_log("fw_line", rd_text(a) ^ rd_text(b) * 3, static_cast<uint64_t>(hex) << 32 ^ static_cast<uint32_t>(digits) << 16 ^ static_cast<uint32_t>(v)); }
const char *u3h_fw_sess(uintptr_t, uint8_t *) { return "other"; }
uint32_t u3h_fw_game_ra(uintptr_t, uintptr_t, uintptr_t) { return 0; }
int32_t u3h_fw_host_link(uintptr_t) { return -1; }
void u3h_cg_log(const char *t, int32_t v) { rd_log("cg_log", std::strlen(t) << 16 | static_cast<uint8_t>(t[0]) << 8 | static_cast<uint8_t>(t[std::strlen(t) - 2]), static_cast<uint32_t>(v)); }
void *u3h_fl_state(void) { return nullptr; }
void u3h_ng_stamp(uintptr_t, void *, uint8_t *) {}
void u3h_lnb_marks(uint8_t *a, uint8_t *b, uint8_t *c, uint64_t *d) { *a = *b = *c = 0; *d = 0; }
}