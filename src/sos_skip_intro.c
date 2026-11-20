#include "sos_skip_intro.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)

/* Logical points of the coroutine, as offsets from its first byte. */
enum {
    kFirstStateCase = 0x19d,  /* case 0: if (this == null) return false, after <>1__state = -1 */
    kFirstStateJump = 0x1a9,
    kResumeCase = 0x555,      /* case 6, the video loop's yield: resumes at the video loop */
    kResumeJump = 0x561,
    kVideoLoop = 0x80e,
    kSaveLookup = 0x1046,     /* after the ending's two log lines: finds the save system, then the save check */
    kEndingFlag = 0xf87,      /* the ending sets isSplashScreenVisualSequenceFinished (+0xa2) */
    kLoopExitJump = 0x82f,    /* the video loop: videoPlayer.isPlaying false goes to the ending */
    kSkipCheck = 0x898,       /* if (skipFinished) { user skip } else yield */
    kSkipBranch = 0x8a6,      /* the user skip: unreachable without the video, and nothing else jumps into it */
    kEnding = 0xa0e,          /* after the video loop */
    kNextFrame = 0x9b2,       /* yield return null as state 6, which resumes at the video loop */
    kSaveCheck = 0x10aa,      /* if (saveSystem == null || !saveSystem.isInitialized) skip the booted mark */
    kSaveReady = 0x10b5,
    kSkipBranchSpace = 0x10c,
};

static const short kHead[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x48, 0x4c, 0x8b, 0x3d, ANY, ANY, ANY, ANY, 0x49, 0x89, 0xfc, 0x49, 0x8b, 0x07, 0x48, 0x89,
    0x45, 0xd0, 0x80, 0x3d, ANY, ANY, ANY, ANY, 0x00, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0x48,
};
/* State at +0x10 and the splash screen at +0x20 of the coroutine object, then the jump table of its 11 states. */
static const short kDispatch[] = {
    0x48, 0xc7, 0x45, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x45, 0xb0, 0x00, 0x00, 0x00, 0x00,
    0xc7, 0x45, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x41, 0x8b, 0x44, 0x24, 0x10, 0x48, 0x83, 0xf8, 0x0a,
    0x0f, 0x87, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY, 0x4d, 0x8b, 0x6c,
    0x24, 0x20, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0,
};
enum { kDispatchAt = 0x162 };
/* test r13,r13 ; mov dword [r12+0x10],-1 ; je ... ; mov rdi,[r13+0x20] */
static const short kFirstState[] = {
    0x4d, 0x85, 0xed, 0x41, 0xc7, 0x44, 0x24, 0x10, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x84, ANY, ANY,
    ANY, ANY, 0x49, 0x8b, 0x7d, 0x20,
};
/* call rax ; test al,al ; je ending */
static const short kLoopExit[] = {0xff, 0xd0, 0x84, 0xc0, 0x0f, 0x84, ANY, ANY, ANY, ANY};
static const short kEndingHead[] = {
    0x48, 0x8b, 0x3d, ANY, ANY, ANY, ANY, 0x83, 0xbf, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, ANY, ANY,
    ANY, ANY, 0x80, 0x3d, ANY, ANY, ANY, ANY, 0x00,
};
/* cmp byte [r13+0x90],0 ; je yield ; mov rdi,[r13+0x30] ; test rdi,rdi ; je ... */
static const short kSkipCheckCode[] = {
    0x41, 0x80, 0xbd, 0x90, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0x7d, 0x30,
    0x48, 0x85, 0xff, 0x0f, 0x84,
};
/* lea rdi,[r12+0x18] ; mov qword [r12+0x18],0 ; call write barrier ; mov dword [r12+0x10],6 */
static const short kNextFrameCode[] = {
    0x49, 0x8d, 0x7c, 0x24, 0x18, 0x49, 0xc7, 0x44, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00, 0xe8, ANY,
    ANY, ANY, ANY, 0x41, 0xc7, 0x44, 0x24, 0x10, 0x06, 0x00, 0x00, 0x00,
};
/* mov dword [r12+0x10],-1 ; jne video loop ; jmp (null splash screen) */
static const short kResumeCode[] = {
    0x4d, 0x85, 0xed, 0x41, 0xc7, 0x44, 0x24, 0x10, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0xe9,
};
/* mov r14,[class statics] ; cmp qword [r14+0x38],0 ; jne ... */
static const short kSaveLookupCode[] = {0x4c, 0x8b, 0x35, ANY, ANY, ANY, ANY, 0x49, 0x83, 0x7e, 0x38, 0x00, 0x75, ANY};
/* mov word [r13+0xa1],0x100 */
static const short kEndingFlagCode[] = {0x66, 0x41, 0xc7, 0x85, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x01};
/* call get save system ; test rax,rax ; je skip ; cmp byte [rax+0x11],0 ; je skip */
static const short kSaveCheckCode[] = {
    0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0, 0x74, ANY, 0x80, 0x78, 0x11, 0x00, 0x74, ANY,
};

/* rax = save system: not ready (no save system, not initialized, no global data, defaults not set) goes to the next frame.
 * Ready goes to the ending while isSplashScreenVisualSequenceFinished (+0xa2, r13 = splash screen) is still false, and on
 * the ending's own pass on to the booted mark. The three jumps follow: ending, save ready, next frame. */
static const uint8_t kWaitForSaveSystem[] = {
    0x48, 0x85, 0xc0, 0x74, 0x29, 0x80, 0x78, 0x11, 0x01, 0x75, 0x23, 0x48, 0x8b, 0x48, 0x20, 0x48, 0x85, 0xc9,
    0x74, 0x1a, 0x80, 0x79, 0x1c, 0x00, 0x74, 0x14, 0x41, 0x80, 0xbd, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x75, 0x05,
};
/* <InitSaveSystem>d__44.MoveNext: the prologue, then the await of Task.Delay(10000) in front of LoadAll. */
static const short kInitSaveSystemHead[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0x25,
};
/* cmp dword [rdi+0xe0],0 ; jne +5 ; call class init ; mov edi,10000 ; xor esi,esi ; call Task.Delay ; test rax,rax */
static const short kStartupDelayCode[] = {
    0x83, 0xbf, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x75, 0x05, 0xe8, ANY, ANY, ANY, ANY, 0xbf, 0x10, 0x27, 0x00, 0x00,
    0x31, 0xf6, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0,
};
enum { kDelayCode = 0x1e9, kDelayImmediate = 0x1f8, kDelayCall = kDelayCode + 21 };
static const uint8_t kShortDelay[] = {0x01, 0x00, 0x00, 0x00};
#define JUMP_LENGTH 5
#define WAIT_BLOCK_LENGTH (sizeof(kWaitForSaveSystem) + 3 * JUMP_LENGTH)
_Static_assert(WAIT_BLOCK_LENGTH <= kSkipBranchSpace, "the wait block fits the user skip");

/* The save loading code polls the flag its native save events clear with await Task.Delay(100): LoadSaveTask and
 * CheckFileExist. Each 100 becomes 1, which keeps the await asynchronous. CheckFileExist's two 500 ms retries stay. */
static const short kLoadSaveTaskHead[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x25,
};
static const short kCheckFileExistHead[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68, 0x48, 0x8b, 0x05,
};
/* mov edi,100 ; xor esi,esi ; call Task.Delay, and the same for 500 */
static const short kPollCode[] = {0xbf, 0x64, 0x00, 0x00, 0x00, 0x31, 0xf6, 0xe8};
static const short kRetryCode[] = {0xbf, 0xf4, 0x01, 0x00, 0x00, 0x31, 0xf6, 0xe8};
enum {
    kLoadSaveTaskLength = 0x3430,
    kLoadSaveTaskPolls = 25,
    kCheckFileExistLength = 0x3610,
    kCheckFileExistPolls = 12,
    kCheckFileExistRetries = 2,
    kMaxPolls = 40,
};

static const SosSkipIntroSite kSites[] = {
    {"CUSA57767-1.00", 168044704u, 0x4fe1d40, 0x49b3aa0, 0x8092f30, 0x8092ee0, 0x528d1a0, 0x52876f0, 0x4bea87c, 0x4be6790},
    {"CUSA57767-1.01", 168046032u, 0x4fe9ea0, 0x49ba2d0, 0x80aa530, 0x80aa4e0, 0x5297860, 0x5291bc0, 0x4bf10bc, 0x4becf70},
};

#define PATTERN_LENGTH(p) (sizeof(p) / sizeof(*(p)))

static int matches(const uint8_t *image, uint64_t size, uint64_t at, const short *pattern, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (pattern[i] != ANY && image[at + i] != (uint8_t)pattern[i]) return 0;
    return 1;
}

static int64_t rel32_target(const uint8_t *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at, sizeof(rel));
    return (int64_t)at + 4 + rel;
}

static int64_t rel8_target(const uint8_t *image, uint64_t at) {
    return (int64_t)at + 1 + (int8_t)image[at];
}

const SosSkipIntroSite *sos_skip_intro_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

/* Offsets of the `mov edi,<ms>` immediates in front of a Task.Delay call inside [start, start + length). */
static size_t delay_immediates(const uint8_t *image, uint64_t size, uint64_t start, uint64_t length, const short *pattern,
                               size_t pattern_length, const SosSkipIntroSite *site, uint64_t *found, size_t capacity) {
    size_t count = 0;
    if (start > size || size - start < length) return 0;
    for (uint64_t at = start; at + pattern_length + 4 <= start + length; ++at) {
        if (!matches(image, size, at, pattern, pattern_length)) continue;
        const int64_t target = rel32_target(image, at + pattern_length);
        if (target != (int64_t)site->task_delay && target != (int64_t)site->task_delay_other) continue;
        if (count < capacity) found[count] = at + 1;
        ++count;
    }
    return count;
}

/* mov rdi,r14 ; call MainMenuState.TickState ; cmp byte [r14+0x6c],0 ; je end, whose target pops the frame and returns */
static const short kTitleCheckCode[] = {
    0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x80, 0x7e, 0x6c, 0x00, 0x0f, 0x84, ANY, ANY, ANY, ANY,
};
static const short kTitleEndCode[] = {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
enum { kTitleCheckAt = 8, kTitleCallAt = 3, kTitleJumpAt = 13, kTitleCheckLength = 5 };
/* MainMenuState.mainMenu, Menu.CurrentState (State.Closed) and MainMenu.IsProcessingLoadButtonVisibility. */
enum { kStateMainMenu = 0x48, kMenuState = 0x88, kMenuClosed = 2, kMenuProcessingLoad = 0x130 };

static int title_check_matches(const uint8_t *image, uint64_t size, const SosSkipIntroSite *site) {
    const uint64_t at = site->title_check;
    if (at < kTitleCheckAt) return 0;
    const uint64_t start = at - kTitleCheckAt;
    return matches(image, size, start, kTitleCheckCode, PATTERN_LENGTH(kTitleCheckCode)) &&
           rel32_target(image, start + kTitleCallAt + 1) == (int64_t)site->base_tick &&
           matches(image, size, (uint64_t)rel32_target(image, start + kTitleJumpAt + 2), kTitleEndCode,
                   PATTERN_LENGTH(kTitleEndCode));
}

static int save_polls_match(const uint8_t *image, uint64_t size, const SosSkipIntroSite *site) {
    uint64_t found[kMaxPolls];
    return matches(image, size, site->load_save_task, kLoadSaveTaskHead, PATTERN_LENGTH(kLoadSaveTaskHead)) &&
           matches(image, size, site->check_file_exist, kCheckFileExistHead, PATTERN_LENGTH(kCheckFileExistHead)) &&
           delay_immediates(image, size, site->load_save_task, kLoadSaveTaskLength, kPollCode, PATTERN_LENGTH(kPollCode), site,
                            found, kMaxPolls) == kLoadSaveTaskPolls &&
           delay_immediates(image, size, site->check_file_exist, kCheckFileExistLength, kPollCode, PATTERN_LENGTH(kPollCode), site,
                            found, kMaxPolls) == kCheckFileExistPolls &&
           delay_immediates(image, size, site->check_file_exist, kCheckFileExistLength, kRetryCode, PATTERN_LENGTH(kRetryCode),
                            site, found, kMaxPolls) == kCheckFileExistRetries;
}

int sos_skip_intro_verify(const uint8_t *image, uint64_t size, const SosSkipIntroSite *site, const char **why) {
    const char *reason = NULL;
    const uint64_t c = site ? site->coroutine : 0;
    if (!site)
        reason = "not a supported build";
    else if (size != site->image_size)
        reason = "the image is not the verified size";
    else if (!matches(image, size, c, kHead, PATTERN_LENGTH(kHead)) ||
             !matches(image, size, c + kDispatchAt, kDispatch, PATTERN_LENGTH(kDispatch)))
        reason = "the splash screen's start coroutine differs";
    else if (!matches(image, size, c + kFirstStateCase, kFirstState, PATTERN_LENGTH(kFirstState)))
        reason = "the coroutine's first state differs";
    else if (!matches(image, size, c + kResumeCase, kResumeCode, PATTERN_LENGTH(kResumeCode)) ||
             rel32_target(image, c + kResumeJump + 2) != (int64_t)(c + kVideoLoop))
        reason = "the coroutine's resume after a yield differs";
    else if (!matches(image, size, c + kSaveLookup, kSaveLookupCode, PATTERN_LENGTH(kSaveLookupCode)) ||
             !matches(image, size, c + kEndingFlag, kEndingFlagCode, PATTERN_LENGTH(kEndingFlagCode)))
        reason = "the ending's save system lookup differs";
    else if (!matches(image, size, c + kLoopExitJump - 4, kLoopExit, PATTERN_LENGTH(kLoopExit)) ||
             rel32_target(image, c + kLoopExitJump + 2) != (int64_t)(c + kEnding))
        reason = "the video loop does not end in the ending";
    else if (!matches(image, size, c + kEnding, kEndingHead, PATTERN_LENGTH(kEndingHead)))
        reason = "the coroutine's ending differs";
    else if (!matches(image, size, c + kSkipCheck, kSkipCheckCode, PATTERN_LENGTH(kSkipCheckCode)) ||
             rel32_target(image, c + kSkipCheck + 10) != (int64_t)(c + kNextFrame) ||
             c + kSkipBranch != c + kSkipCheck + 14 || kNextFrame - kSkipBranch != kSkipBranchSpace)
        reason = "the video loop's user skip differs";
    else if (!matches(image, size, c + kNextFrame, kNextFrameCode, PATTERN_LENGTH(kNextFrameCode)))
        reason = "the coroutine's next frame yield differs";
    else if (!matches(image, size, site->init_save_system, kInitSaveSystemHead, PATTERN_LENGTH(kInitSaveSystemHead)) ||
             !matches(image, size, site->init_save_system + kDelayCode, kStartupDelayCode, PATTERN_LENGTH(kStartupDelayCode)) ||
             rel32_target(image, site->init_save_system + kDelayCall + 1) != (int64_t)site->task_delay)
        reason = "the save system's startup delay differs";
    else if (!save_polls_match(image, size, site))
        reason = "the save loading code's polling waits differ";
    else if (!title_check_matches(image, size, site))
        reason = "the title intro's switch check differs";
    else if (!matches(image, size, c + kSaveCheck - 5, kSaveCheckCode, PATTERN_LENGTH(kSaveCheckCode)) ||
             rel8_target(image, c + kSaveCheck + 4) != rel8_target(image, c + kSaveCheck + 10) ||
             kSaveReady != kSaveCheck + 11)
        reason = "the coroutine's save system check differs";
    if (reason && why) *why = reason;
    return reason == NULL;
}

static void write_jump(uint8_t *image, uint64_t from, uint64_t to) {
    const int32_t rel = (int32_t)((int64_t)to - (int64_t)(from + JUMP_LENGTH));
    image[from] = 0xe9;
    memcpy(image + from + 1, &rel, sizeof(rel));
}

/* push rdi,rsi,rax,rcx,rdx ; mov rdi,r14 ; mov rax,claim ; call rax ; test eax,eax ; pop them ; je original ;
 * mov byte [r14+0x6c],1 ; original: cmp byte [r14+0x6c],0 ; ret. Stands in for the check, in a frame whose rsp is 16-byte
 * aligned at the call, so the five pushes keep it aligned for the claim. */
static const uint8_t kTitleStubHead[] = {0x57, 0x56, 0x50, 0x51, 0x52, 0x4c, 0x89, 0xf7, 0x48, 0xb8};
static const uint8_t kTitleStubTail[] = {0xff, 0xd0, 0x85, 0xc0, 0x5a, 0x59, 0x58, 0x5e, 0x5f, 0x74, 0x05,
                                         0x41, 0xc6, 0x46, 0x6c, 0x01, 0x41, 0x80, 0x7e, 0x6c, 0x00, 0xc3};
_Static_assert(sizeof(kTitleStubHead) + 8 + sizeof(kTitleStubTail) <= SOS_SKIP_INTRO_STUB_BYTES, "the stub fits");

int sos_skip_intro_hook_title(uint8_t *check, uint8_t *stubs) {
    static const uint8_t original[kTitleCheckLength] = {0x41, 0x80, 0x7e, 0x6c, 0x00};
    if (!check || !stubs || memcmp(check, original, sizeof(original))) return 0;
    const int64_t reach = (int64_t)(intptr_t)stubs - (int64_t)(intptr_t)(check + 5);
    if (reach < INT32_MIN || reach > INT32_MAX) return 0;
    const uint64_t claim = (uint64_t)(uintptr_t)sos_skip_intro_claim;
    uint8_t *at = stubs;
    memcpy(at, kTitleStubHead, sizeof(kTitleStubHead));
    at += sizeof(kTitleStubHead);
    memcpy(at, &claim, sizeof(claim));
    at += sizeof(claim);
    memcpy(at, kTitleStubTail, sizeof(kTitleStubTail));
    const int32_t rel = (int32_t)reach;
    check[0] = 0xe8;
    memcpy(check + 1, &rel, sizeof(rel));
    return 1;
}

static atomic_int armed;
static unsigned ticks;

void sos_skip_intro_arm(void) {
    ticks = 0;
    atomic_store(&armed, 1);
}

/* The natural switch comes seconds after the intro starts, with its start coroutine run and the main menu open. The claim
 * waits for the same: a few ticks, then the conditions MainMenuState_WaitForInputPress.TickState needs to leave its state. */
static int title_ready(const uint8_t *state) {
    if (++ticks < SOS_SKIP_INTRO_MIN_TICKS) return 0;
    const uint8_t *menu;
    memcpy(&menu, state + kStateMainMenu, sizeof(menu));
    if (!menu) return 0;
    int32_t menu_state;
    memcpy(&menu_state, menu + kMenuState, sizeof(menu_state));
    return menu_state != kMenuClosed && !menu[kMenuProcessingLoad];
}

__attribute__((sysv_abi)) int sos_skip_intro_claim(const uint8_t *state) {
    if (!atomic_load(&armed) || !state || !title_ready(state) || !atomic_exchange(&armed, 0)) return 0;
    puts("Skip Intro: title intro skipped");
    return 1;
}

int sos_skip_intro_apply(uint8_t *image, uint64_t size, const char *build, uint8_t *stubs, size_t stub_capacity, char *message,
                         size_t message_size) {
    const SosSkipIntroSite *site = sos_skip_intro_site(build);
    const char *why = "";
    if (!site) {
        snprintf(message, message_size, "not applied: not a verified game build");
        return 0;
    }
    if (!sos_skip_intro_verify(image, size, site, &why)) {
        snprintf(message, message_size, "not applied: %s in %s; the game's own behavior is kept", why, build);
        return 0;
    }
    if (!stubs || stub_capacity < SOS_SKIP_INTRO_STUB_BYTES) {
        snprintf(message, message_size, "not applied: no room for the title stub");
        return 0;
    }
    const uint64_t c = site->coroutine;
    if (!sos_skip_intro_hook_title(image + site->title_check, stubs)) {
        snprintf(message, message_size, "not applied: the title stub is out of reach of the image");
        return 0;
    }
    sos_skip_intro_arm();
    memcpy(image + site->init_save_system + kDelayImmediate, kShortDelay, sizeof(kShortDelay));
    uint64_t polls[kMaxPolls];
    const uint64_t starts[] = {site->load_save_task, site->check_file_exist};
    const uint64_t lengths[] = {kLoadSaveTaskLength, kCheckFileExistLength};
    for (size_t i = 0; i < 2; ++i) {
        const size_t count = delay_immediates(image, size, starts[i], lengths[i], kPollCode, PATTERN_LENGTH(kPollCode), site, polls,
                                              kMaxPolls);
        for (size_t k = 0; k < count; ++k) memcpy(image + polls[k], kShortDelay, sizeof(kShortDelay));
    }
    write_jump(image, c + kFirstStateJump, c + kSaveLookup);
    image[c + kFirstStateJump + JUMP_LENGTH] = 0x90;
    write_jump(image, c + kResumeJump, c + kSaveLookup);
    image[c + kResumeJump + JUMP_LENGTH] = 0x90;
    memcpy(image + c + kSkipBranch, kWaitForSaveSystem, sizeof(kWaitForSaveSystem));
    write_jump(image, c + kSkipBranch + sizeof(kWaitForSaveSystem), c + kEnding);
    write_jump(image, c + kSkipBranch + sizeof(kWaitForSaveSystem) + JUMP_LENGTH, c + kSaveReady);
    write_jump(image, c + kSkipBranch + sizeof(kWaitForSaveSystem) + 2 * JUMP_LENGTH, c + kNextFrame);
    write_jump(image, c + kSaveCheck, c + kSkipBranch);
    memset(image + c + kSaveCheck + JUMP_LENGTH, 0x90, kSaveReady - kSaveCheck - JUMP_LENGTH);
    snprintf(message, message_size, "startup presentation bypassed, save waits shortened, title intro armed");
    return 1;
}

int sos_skip_intro_from_file(const char *settings_file) {
    int on = 0;
    FILE *file = settings_file && *settings_file ? fopen(settings_file, "rb") : NULL;
    if (!file) return 0;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        while (isspace((unsigned char)*text)) ++text;
        char *equals = strchr(text, '=');
        if (*text == '#' || !equals) continue;
        char *end = equals;
        while (end > text && isspace((unsigned char)end[-1])) --end;
        *end = 0;
        if (strcmp(text, "skip_intro")) continue;
        char *value = equals + 1;
        while (isspace((unsigned char)*value)) ++value;
        char *stop = strstr(value, " #");
        if (!stop) stop = value + strlen(value);
        while (stop > value && isspace((unsigned char)stop[-1])) --stop;
        *stop = 0;
        on = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "yes") || !strcmp(value, "true");
    }
    fclose(file);
    return on;
}
