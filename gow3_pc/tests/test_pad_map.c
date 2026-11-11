/* GOW3_PAD_MAP / GOW3_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int gow3gpu_overlay_captures_input(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

/* The remap specs below target either the keyboard or the controller; these name the kind. */
static int map_keys(ButtonMap *map, StickKeyMap *sticks, const char *spec) {
    return apply_button_map(map, sticks, spec, MAP_KEYBOARD);
}
static int map_pad(ButtonMap *map, StickKeyMap *sticks, const char *spec) {
    return apply_button_map(map, sticks, spec, MAP_PAD);
}
/* Asserts that host `source` drives exactly the PS4 `buttons` in `map`. */
static void expect_press(const ButtonMap *map, int source, uint32_t buttons) {
    assert(pressed_by(map, source) == buttons);
}

int main(void) {
    StickKeyMap sticks = default_stick_keys;  /* keyboard stick keys; every remap below shares them */
    ButtonMap keys = default_key_map;         /* copy of the stock keyboard layout */
    /* With no spec, or an empty one, nothing is mapped and the layout stays stock. */
    assert(map_keys(&keys, &sticks, NULL) == 0);
    assert(map_keys(&keys, &sticks, "") == 0);
    assert(default_key_map.count == keys.count);
    /* Z takes over Cross and Space takes Triangle, so Space stops giving Cross. Blanks around
     * names and '=' are allowed. */
    assert(map_keys(&keys, &sticks, " cross = Z , triangle=Space") == 2);
    expect_press(&keys, SDL_SCANCODE_Z, BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    expect_press(&keys, SDL_SCANCODE_F, BTN_SQUARE);  /* entries not named keep their key */

    /* Button names ignore case and key names may hold spaces. Naming a button a second time
     * adds another source for it rather than replacing the first. */
    ButtonMap again = default_key_map;  /* fresh stock copy */
    assert(map_keys(&again, &sticks, "CIRCLE=left shift,circle=Left Ctrl") == 2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    expect_press(&again, SDL_SCANCODE_LSHIFT, BTN_CIRCLE | BTN_L3);  /* Shift keeps its stock L3 */

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap junk = default_key_map;
    assert(map_keys(&junk, &sticks, "bogus=Q,cross=NotAKey,circle,=Q,square=NotAKey,,") == 0);
    assert(junk.count == default_key_map.count);
    expect_press(&junk, SDL_SCANCODE_SPACE, BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad = default_pad_map;  /* copy of the stock controller layout */
    assert(map_pad(&pad, &sticks, "cross=b,circle=a,touchpad=misc1") == 3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(map_pad(&pad, &sticks, "touchpad_right=guide,touchpad_left=paddle1") == 2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(map_pad(&pad, &sticks, "cross=Space") == 0);  /* Space names a keyboard key, never a pad button */
    /* Keyboard: each side has a key of its own. */
    expect_press(&default_key_map, SDL_SCANCODE_TAB, CLICK_LEFT);
    expect_press(&default_key_map, SDL_SCANCODE_BACKSPACE, CLICK_RIGHT);
    ButtonMap sides = default_key_map;  /* fresh stock copy for the click keys */
    assert(map_keys(&sides, &sticks, "touchpad_right=Delete") == 1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);
    /* Triggers: sources and targets like buttons (L2/R2 default to them). */
    ButtonMap triggers=default_pad_map;
    assert(pressed_by(&triggers,PAD_SRC_LT)==BTN_L2 && pressed_by(&triggers,PAD_SRC_RT)==BTN_R2);
    assert(apply_button_map(&triggers,&sticks,"l2=leftshoulder,l1=lefttrigger,cross=RightTrigger,r2=a",MAP_PAD)==4);
    assert(pressed_by(&triggers,PAD_SRC_LT)==BTN_L1 && pressed_by(&triggers,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)==BTN_L2);
    assert(pressed_by(&triggers,PAD_SRC_RT)==BTN_CROSS && pressed_by(&triggers,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_R2);
    /* Sticks: Move and Look take a whole stick; a stick is no button source, a button no stick. */
    ButtonMap swapped=default_pad_map;
    assert(swapped.sticks[0]==PAD_SRC_LSTICK && swapped.sticks[1]==PAD_SRC_RSTICK);
    assert(apply_button_map(&swapped,&sticks,"ls_up=rstick,ls_down=rstick,rs_up=lstick,rs_left=",MAP_PAD)==4);
    assert(swapped.sticks[0]==PAD_SRC_RSTICK && swapped.sticks[1]==-1);
    assert(apply_button_map(&swapped,&sticks,"cross=lstick,ls_up=a",MAP_PAD)==0);
    assert(pressed_by(&swapped,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CROSS && swapped.sticks[0]==PAD_SRC_RSTICK);
    /* The keyboard's stick keys are untouched by the controller's stick entries. */
    assert(sticks.count==default_stick_keys.count);
    /* The complete map names every source back, so applied to the defaults it gives the same map. */
    char pad_out[2048], key_out[2048], mouse_out[2048];
    runtime_pad_effective_maps("l2=leftshoulder,ls_up=rstick,rs_up=lstick", "", "", pad_out, key_out, mouse_out, sizeof(pad_out));
    assert(strstr(pad_out,"ls_up=rstick,ls_down=rstick,ls_left=rstick,ls_right=rstick,rs_up=lstick"));
    assert(strstr(pad_out,"l2=leftshoulder,r2=righttrigger"));
    ButtonMap round=default_pad_map;
    StickKeyMap round_sticks=default_stick_keys;
    apply_button_map(&round,&round_sticks,pad_out,MAP_PAD);
    assert(round.sticks[0]==PAD_SRC_RSTICK && round.sticks[1]==PAD_SRC_LSTICK);
    assert(pressed_by(&round,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)==(BTN_L1|BTN_L2) && pressed_by(&round,PAD_SRC_LT)==0);
    runtime_pad_effective_maps("", "", "", pad_out, key_out, mouse_out, sizeof(pad_out));
    assert(strstr(pad_out,"ls_up=lstick") && strstr(pad_out,"l2=lefttrigger,r2=righttrigger"));
    puts("PASS: pad and key maps");
}
