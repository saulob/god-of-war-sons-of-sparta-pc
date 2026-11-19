#pragma once
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef SOS_SDL_COMPAT_POLICY_ONLY
#include <SDL3/SDL.h>
#endif

typedef enum { SOS_HIDAPI_LEAVE, SOS_HIDAPI_DISABLE, SOS_HIDAPI_PRESERVE } SosHidapiAction;

static inline SosHidapiAction sos_hidapi_action(int under_wine, const char *configured) {
    if (configured && configured[0]) return SOS_HIDAPI_PRESERVE;
    return under_wine ? SOS_HIDAPI_DISABLE : SOS_HIDAPI_LEAVE;
}

static inline int sos_running_under_wine(void) {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
#else
    return 0;
#endif
}

#ifndef SOS_SDL_COMPAT_POLICY_ONLY
/* Before the first SDL_INIT_GAMEPAD, which reads the hint; later calls see "0" and do nothing. */
static inline void sos_prepare_sdl_gamepad(void) {
    if (sos_hidapi_action(sos_running_under_wine(), SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)) != SOS_HIDAPI_DISABLE) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    printf("Runtime: Proton/Wine detected; SDL HIDAPI gamepads disabled for compatibility\n");
}
#endif
