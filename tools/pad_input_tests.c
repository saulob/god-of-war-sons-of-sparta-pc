/* Headless test of the pad the game reads: the real runtime_pad.c, built from the path in PAD_SOURCE, on a fake SDL gamepad.
 * Prints one line per sampled state, so the working tree's pad and the parent commit's can be compared line by line, and
 * checks the values themselves. No window, SDL library, controller or audio device is touched. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include PAD_SOURCE

#if defined(PAD_KIND_TEST) || defined(PAD_BACK_TEST)
#pragma clang diagnostic ignored "-Wunused-function"
#endif

static struct {
    bool present, touchpad, finger, buttons[SDL_GAMEPAD_BUTTON_COUNT];
    int16_t axes[SDL_GAMEPAD_AXIS_COUNT];
} fake;
static int capture;
static unsigned checks, failures;
static SDL_GamepadType fake_type = SDL_GAMEPAD_TYPE_XBOXONE;
static int published_kind = -1;
static unsigned publishes;

uint32_t SDL_WasInit(uint32_t flags) { (void)flags; return 0; }
bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
const char *SDL_GetHint(const char *name) { (void)name; return NULL; }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
SDL_JoystickID *SDL_GetGamepads(int *count) {
    SDL_JoystickID *ids = malloc(2 * sizeof(*ids));
    ids[0] = 7;
    ids[1] = 0;
    *count = fake.present ? 1 : 0;
    return ids;
}
void SDL_free(void *memory) { free(memory); }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { return id == 7 && fake.present ? (SDL_Gamepad *)&fake : NULL; }
void SDL_CloseGamepad(SDL_Gamepad *gamepad) { (void)gamepad; }
bool SDL_GamepadConnected(SDL_Gamepad *gamepad) { (void)gamepad; return fake.present; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad) { (void)gamepad; return 7; }
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) { (void)gamepad; return fake.buttons[button]; }
int16_t SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) { (void)gamepad; return fake.axes[axis]; }
int SDL_GetNumGamepadTouchpads(SDL_Gamepad *gamepad) { (void)gamepad; return fake.touchpad ? 1 : 0; }
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *gamepad, int touchpad) { (void)gamepad; (void)touchpad; return fake.touchpad ? 1 : 0; }
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *gamepad, int touchpad, int finger, bool *down, float *x, float *y, float *pressure) {
    (void)gamepad; (void)touchpad; (void)finger; (void)pressure;
    *down = fake.finger;
    *x = *y = 0.5f;
    return fake.touchpad;
}
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) { (void)gamepad; return "Xbox Series X Controller"; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { (void)gamepad; return fake_type; }
void sos_pad_kind_publish(int pad_kind) {
    published_kind = pad_kind;
    ++publishes;
}
uint16_t SDL_GetGamepadVendor(SDL_Gamepad *gamepad) { (void)gamepad; return 0x045e; }
uint16_t SDL_GetGamepadProduct(SDL_Gamepad *gamepad) { (void)gamepad; return 0x0b12; }
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, uint16_t low, uint16_t high, uint32_t ms) { (void)gamepad; (void)low; (void)high; (void)ms; return true; }
bool SDL_SetGamepadLED(SDL_Gamepad *gamepad, uint8_t r, uint8_t g, uint8_t b) { (void)gamepad; (void)r; (void)g; (void)b; return true; }
const bool *SDL_GetKeyboardState(int *count) { (void)count; return NULL; }
uint64_t SDL_GetTicksNS(void) { return 0; }
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *name) {
    return !strcmp(name, "back") ? SDL_GAMEPAD_BUTTON_BACK : !strcmp(name, "start") ? SDL_GAMEPAD_BUTTON_START : SDL_GAMEPAD_BUTTON_INVALID;
}
SDL_Scancode SDL_GetScancodeFromName(const char *name) { (void)name; return SDL_SCANCODE_UNKNOWN; }
int SDL_strcasecmp(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        const int x = *a | 0x20, y = *b | 0x20;
        if (x != y) return x - y;
    }
    return *a - *b;
}

int dcpgpu_overlay_captures_input(void) { return capture; }
void dcpgpu_choice_set_input(uint32_t (*read)(void)) { (void)read; }
int dcpgpu_latency_trace_enabled(void) { return 0; }
uint64_t dcpgpu_latency_press_ns(void) { return 0; }
void sos_keys_load(const char *file, SosKeyMap *map) { (void)file; memset(map, 0, sizeof(*map)); }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
void runtime_user_login(int player) { (void)player; }
int runtime_user_player(int32_t user) { return user == 1 ? 1 : 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vprintf(format, args);
    va_end(args);
    return n;
}

static void check(int ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("  FAIL %s\n", what);
    }
}

static int32_t handle;
static PadData read_pad(const char *label) {
    PadData d;
    memset(&d, 0xaa, sizeof(d));
    const int32_t result = pad_read_state(handle, &d);
    check(result == 0, "scePadReadState succeeds");
    printf("read %s buttons=0x%x stick=%u,%u,%u,%u triggers=%u,%u connected=%u count=%u touches=%u\n", label, d.buttons, d.left_x, d.left_y,
           d.right_x, d.right_y, d.l2, d.r2, d.connected, d.connected_count, d.touch_count);
    return d;
}
static void release_all(void) {
    memset(fake.buttons, 0, sizeof(fake.buttons));
    memset(fake.axes, 0, sizeof(fake.axes));
}

/* What the title's PRESS ANY BUTTON handler takes from a state: the first pressed button that is not a d-pad direction, with no
 * d-pad direction or stick deflection in the same state (MainMenuState_WaitForInputPress.CheckForInputPress and
 * IsDpadPressedInEvent). */
static int title_confirms(const PadData *d) {
    const uint32_t directions = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
    const int deflected = abs(d->left_x - 128) > 40 || abs(d->left_y - 128) > 40 || abs(d->right_x - 128) > 40 || abs(d->right_y - 128) > 40;
    return (d->buttons & ~directions) != 0 && !(d->buttons & directions) && !deflected;
}

static const struct { const char *name; SDL_GamepadButton button; uint32_t ps; int confirms; } kButtons[] = {
    {"A", SDL_GAMEPAD_BUTTON_SOUTH, BTN_CROSS, 1},          {"B", SDL_GAMEPAD_BUTTON_EAST, BTN_CIRCLE, 1},
    {"X", SDL_GAMEPAD_BUTTON_WEST, BTN_SQUARE, 1},          {"Y", SDL_GAMEPAD_BUTTON_NORTH, BTN_TRIANGLE, 1},
    {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L1, 1},    {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R1, 1},
    {"Start", SDL_GAMEPAD_BUTTON_START, BTN_OPTIONS, 1},    {"Back", SDL_GAMEPAD_BUTTON_BACK, BTN_TOUCHPAD, 1},
    {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_L3, 1},       {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_R3, 1},
    {"DPadUp", SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP, 0},      {"DPadRight", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT, 0},
    {"DPadDown", SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN, 0}, {"DPadLeft", SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT, 0},
};

static void buttons_test(void) {
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(*kButtons); ++i) {
        release_all();
        fake.buttons[kButtons[i].button] = true;
        const PadData d = read_pad(kButtons[i].name);
        char what[96];
        snprintf(what, sizeof(what), "%s reaches the game as its PlayStation button", kButtons[i].name);
        check((d.buttons & kButtons[i].ps) == kButtons[i].ps, what);
        snprintf(what, sizeof(what), "%s is the only button down", kButtons[i].name);
        check((d.buttons & ~(kButtons[i].ps | BTN_TOUCHPAD)) == 0, what);
        snprintf(what, sizeof(what), "%s %s the title", kButtons[i].name, kButtons[i].confirms ? "confirms" : "is ignored by");
        check(title_confirms(&d) == kButtons[i].confirms, what);
    }
    release_all();
    const PadData idle = read_pad("idle");
    check(idle.buttons == 0 && idle.left_x == 128 && idle.left_y == 128 && idle.right_x == 128 && idle.right_y == 128, "idle is neutral");
}

static void sticks_test(void) {
    release_all();
    fake.axes[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
    PadData d = read_pad("left stick right");
    check(d.left_x == 255 && d.left_y == 128 && d.buttons == 0, "left stick right reaches the game and presses no button");
    check(!title_confirms(&d), "a stick alone does not confirm the title");
    release_all();
    fake.axes[SDL_GAMEPAD_AXIS_LEFTY] = -32768;
    d = read_pad("left stick up");
    check(d.left_y == 0 && d.left_x == 128, "left stick up reaches the game");
    release_all();
    fake.axes[SDL_GAMEPAD_AXIS_RIGHTX] = -32768;
    fake.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 32767;
    d = read_pad("right stick");
    check(d.right_x == 0 && d.right_y == 255, "right stick reaches the game");
    release_all();
    fake.axes[SDL_GAMEPAD_AXIS_LEFTX] = 2000;
    d = read_pad("left stick drift");
    check(d.left_x == 128, "a drifting stick inside the dead zone is neutral");
    release_all();
    fake.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    fake.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 16000;
    d = read_pad("triggers");
    check((d.buttons & BTN_L2) && (d.buttons & BTN_R2) && d.l2 == 255 && d.r2 > 100, "the triggers reach the game");
    release_all();
}

/* Presses and holds reach every read as a level: the number of reads per game frame never changes what the game sees. */
static void edges_test(void) {
    static const unsigned reads_per_frame[] = {1, 2, 3, 6};
    for (size_t r = 0; r < sizeof(reads_per_frame) / sizeof(*reads_per_frame); ++r) {
        release_all();
        uint32_t previous = 0;
        unsigned rising = 0, falling = 0, held_frames = 0;
        const unsigned plan[] = {0, 0, 1, 1, 1, 1, 0, 0, 1, 0};
        for (size_t frame = 0; frame < sizeof(plan) / sizeof(*plan); ++frame) {
            fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = plan[frame] != 0;
            uint32_t seen = 0;
            for (unsigned k = 0; k < reads_per_frame[r]; ++k) {
                const PadData d = read_pad("edge");
                if (k == 0) seen = d.buttons & BTN_CROSS;
                check((d.buttons & BTN_CROSS) == seen, "every read of a frame holds the same state");
            }
            rising += seen && !previous;
            falling += !seen && previous;
            held_frames += seen != 0;
            previous = seen;
        }
        check(rising == 2 && falling == 2 && held_frames == 5, "one press is one rising edge, however many reads a frame holds");
    }
    release_all();
}

/* A press of P ms seen by a game that polls at a vblank rate: it is one rising edge when it spans a frame boundary. */
static void frame_rate_test(void) {
    static const unsigned rates[] = {30, 60, 90, 120, 144, 240};
    for (size_t r = 0; r < sizeof(rates) / sizeof(*rates); ++r) {
        for (unsigned press_ms = 50; press_ms <= 150; press_ms += 50) {
            release_all();
            const double period = 1000.0 / rates[r];
            uint32_t previous = 0;
            unsigned rising = 0;
            for (unsigned frame = 0; frame * period < 400.0; ++frame) {
                const double t = frame * period;
                fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = t >= 100.0 && t < 100.0 + press_ms;
                const PadData d = read_pad("rate");
                const uint32_t seen = d.buttons & BTN_CROSS;
                rising += seen && !previous;
                previous = seen;
            }
            char what[96];
            snprintf(what, sizeof(what), "a %u ms press at %u Hz is one rising edge", press_ms, rates[r]);
            check(rising == 1, what);
        }
    }
    release_all();
}

static void connection_test(void) {
    release_all();
    uint64_t last = 0;
    unsigned count = 0;
    int stable = 1;
    for (int i = 0; i < 1000; ++i) {
        PadData d;
        pad_read_state(handle, &d);
        if (i == 0) count = d.connected_count;
        stable &= d.connected == 1 && d.connected_count == count && d.timestamp >= last;
        last = d.timestamp;
    }
    printf("connection count=%u\n", count);
    check(stable && count == 1, "connected, a steady connected count and a non-decreasing timestamp over 1000 reads");
    ControllerInfo info;
    check(pad_info(handle, &info) == 0 && info.connected == 1 && info.connected_count == count, "controller information agrees with the pad data");
}

static void capture_test(void) {
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    capture = 1;
    PadData d = read_pad("captured");
    check(d.buttons == 0, "input is neutral while the overlay holds it");
    capture = 0;
    d = read_pad("after capture, still held");
    check(d.buttons == 0, "a button still held when the overlay closes stays hidden");
    release_all();
    d = read_pad("released");
    fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    d = read_pad("pressed again");
    check(d.buttons & BTN_CROSS, "the next press reaches the game");
    release_all();
}

#ifdef PAD_BACK_TEST
static const struct { SDL_GamepadType type; const char *name; int playstation; } kPads[] = {
    {SDL_GAMEPAD_TYPE_XBOX360, "Xbox360", 0},    {SDL_GAMEPAD_TYPE_XBOXONE, "XboxSeries", 0},
    {SDL_GAMEPAD_TYPE_PS4, "DualShock4", 1},     {SDL_GAMEPAD_TYPE_PS5, "DualSense", 1},
    {SDL_GAMEPAD_TYPE_STANDARD, "Generic", 0},   {SDL_GAMEPAD_TYPE_UNKNOWN, "Unknown", 0},
    {SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, "SwitchPro", 0},
};
#define PAD_COUNT (sizeof(kPads) / sizeof(*kPads))

/* The same sample through both exported reads; they must agree. */
static PadData read_both(const char *label) {
    const PadData state = read_pad(label);
    PadData buffered;
    memset(&buffered, 0xaa, sizeof(buffered));
    check(pad_read(handle, &buffered, 1) == 1, "scePadRead returns one entry");
    check(buffered.buttons == state.buttons && buffered.touch_count == state.touch_count && buffered.left_x == state.left_x &&
              buffered.left_y == state.left_y && buffered.l2 == state.l2 && buffered.r2 == state.r2,
          "scePadRead and scePadReadState agree");
    return state;
}
static PadData sample_as(size_t pad, const char *what, uint32_t expected, const char *tag) {
    char label[96], text[160];
    snprintf(label, sizeof(label), "%s/%s", kPads[pad].name, what);
    const PadData d = read_both(label);
    snprintf(text, sizeof(text), "%s %s: buttons 0x%x, expected 0x%x", kPads[pad].name, tag, d.buttons, expected);
    check(d.buttons == expected, text);
    return d;
}

/* Back is the Touchpad button except on a DualShock 4 or DualSense, whose Share/Create it is. */
static void back_test(size_t pad) {
    const uint32_t view = kPads[pad].playstation ? 0 : BTN_TOUCHPAD;
    fake_type = kPads[pad].type;
    fake.touchpad = kPads[pad].playstation;
    release_all();
    sample_as(pad, "idle", 0, "idle");
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(*kButtons); ++i) {
        release_all();
        fake.buttons[kButtons[i].button] = true;
        sample_as(pad, kButtons[i].name, kButtons[i].button == SDL_GAMEPAD_BUTTON_BACK ? view : kButtons[i].ps, kButtons[i].name);
    }
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = fake.buttons[SDL_GAMEPAD_BUTTON_START] = true;
    sample_as(pad, "BACK+START", BTN_OPTIONS | view, "Back and Start together");
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    sample_as(pad, "BACK+A", BTN_CROSS | view, "Back and A together");
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = fake.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
    sample_as(pad, "BACK+DPAD", BTN_UP | view, "Back and d-pad up together");
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = true;
    fake.axes[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
    fake.axes[SDL_GAMEPAD_AXIS_RIGHTY] = -32768;
    fake.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 32767;
    PadData d = sample_as(pad, "BACK+STICKS", BTN_R2 | view, "Back with sticks and a trigger");
    check(d.left_x == 255 && d.left_y == 128 && d.right_y == 0 && d.r2 == 255, "Back leaves the sticks and triggers as they are");
    release_all();

    if (kPads[pad].playstation) {
        fake.buttons[SDL_GAMEPAD_BUTTON_TOUCHPAD] = true;
        fake.finger = true;
        d = sample_as(pad, "TOUCHPAD", BTN_TOUCHPAD, "the real touchpad press");
        check(d.touch_count == 1, "the real touchpad press keeps its finger");
        fake.finger = false;
        sample_as(pad, "TOUCHPAD-click", BTN_TOUCHPAD, "the real touchpad click without a finger");
        fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = true;
        sample_as(pad, "TOUCHPAD+BACK", BTN_TOUCHPAD, "the real touchpad with Share/Create");
        release_all();
    }

    static const unsigned plan[] = {0, 1, 1, 1, 0, 0};
    unsigned rising = 0, falling = 0, held = 0;
    uint32_t previous = 0;
    for (size_t frame = 0; frame < sizeof(plan) / sizeof(*plan); ++frame) {
        fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = plan[frame] != 0;
        for (int k = 0; k < 2; ++k) {
            d = sample_as(pad, "BACK-hold", plan[frame] ? view : 0, "Back press, hold and release follow the held state");
            if (k == 0) {
                rising += (d.buttons & BTN_TOUCHPAD) && !previous;
                falling += !(d.buttons & BTN_TOUCHPAD) && previous;
                held += (d.buttons & BTN_TOUCHPAD) != 0;
                previous = d.buttons & BTN_TOUCHPAD;
            }
        }
    }
    check(view ? (rising == 1 && falling == 1 && held == 3) : (rising == 0 && falling == 0 && held == 0), "Back is one level-held press, or none on a PlayStation pad");
    release_all();
}

/* An SOS_PAD_MAP value set by the caller; argv[1] repeats it so the expectations follow it. */
static void map_test(const char *map) {
    for (size_t pad = 0; pad < PAD_COUNT; ++pad) {
        const uint32_t view = kPads[pad].playstation ? 0 : BTN_TOUCHPAD;
        fake_type = kPads[pad].type;
        fake.touchpad = kPads[pad].playstation;
        release_all();
        fake.buttons[SDL_GAMEPAD_BUTTON_BACK] = true;
        if (!strcmp(map, "touchpad=back")) sample_as(pad, "BACK", BTN_TOUCHPAD, "an explicit touchpad=back");
        if (!strcmp(map, "cross=back")) sample_as(pad, "BACK", BTN_CROSS | view, "cross=back keeps the default Back rule");
        if (!strcmp(map, "touchpad=start")) sample_as(pad, "BACK", 0, "touchpad=start frees Back");
        release_all();
        fake.buttons[SDL_GAMEPAD_BUTTON_START] = true;
        if (!strcmp(map, "touchpad=start")) sample_as(pad, "START", BTN_OPTIONS | BTN_TOUCHPAD, "touchpad=start");
        release_all();
    }
}

int main(int argc, char **argv) {
    fake.present = true;
    check(pad_init() == 0, "scePadInit");
    handle = pad_open(1, 0, 0, NULL);
    check(handle == 1, "scePadOpen gives pad handle 1");
    if (argc > 1) map_test(argv[1]);
    else for (size_t pad = 0; pad < PAD_COUNT; ++pad) back_test(pad);
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
#elif defined(PAD_KIND_TEST)
/* The controller kind the pad layer publishes for the prompts: classified when a gamepad connects or goes away, never per sample,
 * and without any change to what the pad delivers. */
static void kind_test(void) {
    static const struct { SDL_GamepadType type; int kind; const char *name; } kinds[] = {
        {SDL_GAMEPAD_TYPE_XBOX360, SOS_PAD_XBOX, "Xbox 360"},
        {SDL_GAMEPAD_TYPE_XBOXONE, SOS_PAD_XBOX, "Xbox One and Series"},
        {SDL_GAMEPAD_TYPE_PS4, SOS_PAD_PLAYSTATION4, "DualShock 4"},
        {SDL_GAMEPAD_TYPE_PS5, SOS_PAD_PLAYSTATION5, "DualSense"},
        {SDL_GAMEPAD_TYPE_PS3, SOS_PAD_OTHER, "DualShock 3"},
        {SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, SOS_PAD_OTHER, "Switch Pro"},
        {SDL_GAMEPAD_TYPE_STANDARD, SOS_PAD_OTHER, "a standard gamepad"},
        {SDL_GAMEPAD_TYPE_UNKNOWN, SOS_PAD_OTHER, "an unknown gamepad"},
    };
    fake.present = false;
    release_all();
    PadData d;
    pad_read_state(handle, &d);
    check(publishes == 0 && published_kind == -1, "nothing is published while no gamepad was ever connected");
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        char what[96];
        fake_type = kinds[i].type;
        fake.present = true;
        struct timespec pause = {0, 300000000};
        nanosleep(&pause, NULL);
        pad_read_state(handle, &d);
        snprintf(what, sizeof(what), "%s is published as kind %d", kinds[i].name, kinds[i].kind);
        check(published_kind == kinds[i].kind, what);
        const unsigned before = publishes;
        for (int k = 0; k < 1000; ++k) pad_read_state(handle, &d);
        snprintf(what, sizeof(what), "%s: a thousand samples publish nothing more", kinds[i].name);
        check(publishes == before, what);
        fake.present = false;
        pad_read_state(handle, &d);
        snprintf(what, sizeof(what), "%s: unplugging publishes None", kinds[i].name);
        check(published_kind == SOS_PAD_NONE, what);
    }
    fake_type = SDL_GAMEPAD_TYPE_XBOXONE;
    fake.present = true;
    struct timespec pause = {0, 300000000};
    nanosleep(&pause, NULL);
    release_all();
    fake.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    pad_read_state(handle, &d);
    check((d.buttons & BTN_CROSS) && d.connected == 1, "after a reconnect the Xbox A button still reaches the game as Cross");
    release_all();
}

int main(void) {
    fake.present = false;
    check(pad_init() == 0, "scePadInit");
    handle = pad_open(1, 0, 0, NULL);
    check(handle == 1, "scePadOpen gives pad handle 1");
    kind_test();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
#else
int main(void) {
    fake.present = true;
    check(pad_init() == 0, "scePadInit");
    handle = pad_open(1, 0, 0, NULL);
    check(handle == 1, "scePadOpen gives pad handle 1");
    buttons_test();
    sticks_test();
    edges_test();
    frame_rate_test();
    connection_test();
    capture_test();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
