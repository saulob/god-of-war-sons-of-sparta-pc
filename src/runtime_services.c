/* System services for an offline console:
 *   - local users (runtime_users.c) are logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "startup_phase.h"
#include "dcpgpu.h"
#include "runtime_log.h"
#include "trophies/trophy_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define USER_ID 1
#define ORBIS_OK 0
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* ---- SystemService ---- */
#define NOTE_SYSTEM(api) runtime_history_note(api,"libSceSystemService",(uintptr_t)__builtin_return_address(0))
static int launcher_language=1;
void runtime_set_language(int ps4_language) { launcher_language=ps4_language; }
static int language(void) { const char *v=getenv("SOS_LANGUAGE"); return v ? atoi(v) : launcher_language; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    NOTE_SYSTEM("sceSystemServiceParamGetInt");
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
    case 4: case 5: {                           /* standard-time offset in minutes; summer time flag */
        time_t now=time(NULL); struct tm t; int dst;
#ifdef _WIN32
        dst=!localtime_s(&t,&now) && t.tm_isdst>0;
        *value=id==5 ? dst : (int32_t)((compat_utc_offset(now)-(dst ? 3600 : 0))/60);
#else
        dst=localtime_r(&now,&t) && t.tm_isdst>0;
        *value=id==5 ? dst : (int32_t)((t.tm_gmtoff-(dst ? 3600 : 0))/60);
#endif
        break;
    }
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"Runtime: unsupported system parameter %d reads 0\n",id); *value=0; break;
    }
    return 0;
}
static ABI int32_t system_param_string(int32_t id,char *buffer,uint64_t size) {
    NOTE_SYSTEM("sceSystemServiceParamGetString");
    if (!buffer || !size) return SYSTEM_PARAMETER;
    if (id==6) { snprintf(buffer,(size_t)size,"Sons of Sparta PC"); return 0; }
    buffer[0]=0;
    return SYSTEM_PARAMETER;
}
static ABI int32_t hdr_luminance(void *info) {
    NOTE_SYSTEM("sceSystemServiceGetHdrToneMapLuminance");
    if (!info) return SYSTEM_PARAMETER;
    memset(info,0,3*sizeof(float)); /* SDR host: no HDR luminance to advertise */
    return 0;
}
static ABI int32_t power_tick(void) { NOTE_SYSTEM("sceSystemServicePowerTick"); return 0; }
static ABI int32_t dialog_ok(void) { return 0; }
/* Events the online layer queues (an accepted squad invitation, a Join): eventType @0, data @4. They wait until the
 * game's user manager runs, which drops a session invitation addressed to a user it does not know yet. */
#define SYSTEM_EVENT_BYTES 0xc0
#define MAX_SYSTEM_EVENTS 4
static unsigned char system_events[MAX_SYSTEM_EVENTS][SYSTEM_EVENT_BYTES];
static int system_events_pending;
int runtime_system_event_push(const void *event, size_t size) {
    if (!event || size>SYSTEM_EVENT_BYTES) return 0;
    pthread_mutex_lock(&lock);
    const int queued=system_events_pending<MAX_SYSTEM_EVENTS;
    if (queued) {
        memset(system_events[system_events_pending],0,SYSTEM_EVENT_BYTES);
        memcpy(system_events[system_events_pending++],event,size);
    }
    pthread_mutex_unlock(&lock);
    return queued;
}
static ABI int32_t system_status(unsigned char *status) {
    NOTE_SYSTEM("sceSystemServiceGetStatus");
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* no overlay, foreground */
    status[6]=1;         /* normal CPU mode */
    const int polled=runtime_user_events_polled();
    pthread_mutex_lock(&lock);
    const int32_t events=polled ? system_events_pending : 0;
    pthread_mutex_unlock(&lock);
    memcpy(status,&events,4);
    return 0;
}
static ABI int32_t system_event(unsigned char *event) {
    NOTE_SYSTEM("sceSystemServiceReceiveEvent");
    if (!event) return SYSTEM_PARAMETER;
    const int polled=runtime_user_events_polled();
    pthread_mutex_lock(&lock);
    const int ready=polled && system_events_pending;
    if (ready) {
        memcpy(event,system_events[0],SYSTEM_EVENT_BYTES);
        memmove(system_events[0],system_events[1],(size_t)--system_events_pending*SYSTEM_EVENT_BYTES);
    }
    pthread_mutex_unlock(&lock);
    if (ready) printf("Runtime: system event 0x%08x delivered\n",*(const uint32_t *)event);
    return ready ? 0 : SYSTEM_NO_EVENT;
}
static ABI int32_t hide_splash(void) { NOTE_SYSTEM("sceSystemServiceHideSplashScreen"); note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t report_abnormal_termination(int32_t reason) {
    NOTE_SYSTEM("sceSystemServiceReportAbnormalTermination");
    printf("Runtime: SystemService: abnormal termination reported (%d)\n",reason);
    return 0;
}
static ABI int32_t safe_area(void *info) {
    NOTE_SYSTEM("sceSystemServiceGetDisplaySafeAreaInfo");
    if (!info) return SYSTEM_PARAMETER;
    memset(info,0,132);
    *(float *)info=1.0f;
    return 0;
}
static ABI int32_t disable_music_player(void) { return 0; }
static ABI int32_t reenable_music_player(void) { return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }
#define LOAD_EXEC_EXIT 30
/* The PS4 replaces the process with another executable of the package. Sons of Sparta has only
 * the one, so the request ends the process with LOAD_EXEC_EXIT. */
static ABI __attribute__((noreturn)) void load_exec(const char *path, char *const *argv) {
    (void)argv;
    char host[4096]="";
    if (path) runtime_file_translate(path,host,sizeof(host));
    sos_startup_phase("guest exit requested");
    printf("LoadExec: %s -> %s\n",path ? path : "(null)",host[0] ? host : "(no host file)");
    fflush(NULL);
    runtime_log_mark_clean_exit();
    runtime_log_finalize();
    _exit(LOAD_EXEC_EXIT); /* no destructors: GPU and guest threads are running */
}

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_init(void) { return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
#define SSL_INVALID_ID ((int32_t)0x80435003)
static ABI int32_t ssl_no_certificate(void) { return SSL_INVALID_ID; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x804311fe; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; uint32_t button; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0},{"MsgDialog",0,0,0},{"SaveDataDialog",0,0,0},
                         {"NpProfileDialog",0,0,0},{"NpCommerceDialog",0,0,0},{"ImeDialog",0,0,0},{"ErrorDialog",0,0,0},{"InvitationDialog",0,0,0},
                         {"WebBrowserDialog",0,0,0},{"SocialScreenDialog",0,0,0},{"HmdSetupDialog",0,0,0}};
#define DIALOG_RUNNING 2
#define DIALOG_FINISHED 3
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_NOT_SUPPORTED ((int32_t)0x80B8000F)
static int common_initialized;
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
static ABI int32_t common_is_used(void) {
    for (size_t i=0;i<sizeof(dialogs)/sizeof(*dialogs);++i) if (dialogs[i].status==DIALOG_RUNNING) return 1;
    return 0;
}
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_FINISHED; dialogs[i].button=1; return 0; /* OK / YES */
}
/* A progress bar shows the game's own work: it runs until the game closes it. */
static int32_t dialog_open_progress(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s progress bar opened (runs until closed)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_RUNNING; dialogs[i].button=0; return 0;
}
static int32_t dialog_close(int i) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    dialogs[i].status=DIALOG_FINISHED; return 0;
}
static int32_t dialog_progress(int i, uint32_t target) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    return target ? DIALOG_PARAM_INVALID : 0;
}
/* Result: result (OK), button, reserved; SaveDataDialog's begins with the mode. */
static int32_t dialog_result(int i, unsigned char *out, size_t offset) {
    if (dialogs[i].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    uint32_t values[2]={0,dialogs[i].button};
    memcpy(out+offset,values,8);
    return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4)
/* Invitations need PSN: the dialog initializes but cannot open, so it never finishes and has no result. */
#define INVITATION_DIALOG 7
static ABI int32_t invitation_init(void) { return dialog_init(INVITATION_DIALOG); }
static ABI int32_t invitation_open(const void *param) { (void)param; return dialogs[INVITATION_DIALOG].initialized ? NP_SIGNED_OUT : DIALOG_NOT_INITIALIZED; }
static ABI int32_t invitation_status(void) { return dialog_status(INVITATION_DIALOG); }
static ABI int32_t invitation_close(void) { return dialog_close(INVITATION_DIALOG); }
static ABI int32_t invitation_term(void) { return dialog_term(INVITATION_DIALOG); }
static ABI int32_t invitation_result(void *out) { (void)out; return dialogs[INVITATION_DIALOG].status==DIALOG_FINISHED ? 0 : DIALOG_NOT_FINISHED; }
static ABI int32_t error_init(void) { return dialog_init(6); }
static ABI int32_t error_status(void) { return dialog_status(6); }
static ABI int32_t error_term(void) { return dialog_term(6); }
static ABI int32_t error_close(void) { return dialog_close(6); }
static ABI int32_t commerce_close(void) { return dialog_close(4); }
/* Nothing can be bought offline: the store dialog reports that the player cancelled. */
static ABI int32_t commerce_result(unsigned char *out) {
    if (dialogs[4].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    const int32_t canceled=1;
    memcpy(out,&canceled,4); out[4]=0; /* result, authorized */
    return 0;
}
static ABI int32_t error_open(const int32_t *param) {
    if (param) printf("Runtime: error dialog, code 0x%08x\n",(unsigned)param[1]);
    return dialog_open(6);
}
/* OrbisMsgDialogParam: BaseParam (48 bytes), size, mode (2 = progress bar).
 * OrbisSaveDataDialogParam: BaseParam, int32 size, mode (5 = progress bar). */
static int32_t dialog_open_mode(int i, const unsigned char *param, size_t mode_offset, uint32_t progress_mode) {
    if (!param) return DIALOG_ARG_NULL;
    uint32_t mode; memcpy(&mode,param+mode_offset,4);
    return mode==progress_mode ? dialog_open_progress(i) : dialog_open(i);
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_open(const unsigned char *p) { return dialog_open_mode(1,p,56,2); }
static ABI int32_t msg_status(void) { return dialog_status(1); }
static ABI int32_t msg_term(void) { return dialog_term(1); }
static ABI int32_t msg_close(void) { return dialog_close(1); }
static ABI int32_t msg_progress(uint32_t target, uint32_t value) { (void)value; return dialog_progress(1,target); }
static ABI int32_t msg_result(unsigned char *out) { return dialog_result(1,out,0); }
/* SaveDataDialog. OrbisSaveDataDialogParam: BaseParam (48), size, mode @52, dispType @56,
 * animParam @64, items @72, ..., userData @112. SaveDialogItems: userId @0, titleId @8,
 * dirName @16 (32-byte names), dirNameNum @24, newItem @32 (title @0), focusPos @40.
 * A list (mode 1) is shown for the player to choose; every other mode answers for the
 * first listed save. The result names the chosen save ("" = the new item). */
#define SAVE_MODE_LIST 1
#define SAVE_RESULT_CANCELED 1
static struct {
    uint32_t mode, result, progress;
    void *user_data;
    char dirs[32][32];
    int dir_count, has_new, choosing;
} save_dialog;
static char save_labels[33][200], save_icons[33][720];
static void save_label(char *out, size_t size, const char *dir) {
    unsigned char param[RUNTIME_SAVE_PARAM_SIZE];
    if (runtime_savedata_param(USER_ID,dir,param)) { snprintf(out,size,"%s",dir); return; }
    int64_t mtime; memcpy(&mtime,param+1288,8);
    time_t t=(time_t)mtime; struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char when[32]; strftime(when,sizeof(when),"%Y-%m-%d %H:%M",&tm);
    const char *title=(const char *)param, *subtitle=(const char *)param+128;
    /* Title, subtitle and date on their own lines, beside the save's picture. */
    snprintf(out,size,"%.60s\n%.60s%s%s",title[0] ? title : dir,subtitle,subtitle[0] ? "\n" : "",when);
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_open(const unsigned char *p) {
    if (!p) return DIALOG_ARG_NULL;
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    memset(&save_dialog,0,sizeof(save_dialog));
    memcpy(&save_dialog.mode,p+52,4);
    memcpy(&save_dialog.user_data,p+112,8);
    const unsigned char *items; memcpy(&items,p+72,8);
    if (items) {
        const char *dirs; uint32_t count; const void *new_item;
        memcpy(&dirs,items+16,8); memcpy(&count,items+24,4); memcpy(&new_item,items+32,8);
        for (uint32_t i=0;dirs && i<count && save_dialog.dir_count<32;++i)
            if (dirs[i*32]) memcpy(save_dialog.dirs[save_dialog.dir_count++],dirs+i*32,31);
        save_dialog.has_new=new_item!=NULL;
    }
    if (save_dialog.mode!=SAVE_MODE_LIST) return dialog_open_mode(2,p,52,5);
    const char *labels[33], *icons[33]; int n=0;
    if (save_dialog.has_new) { snprintf(save_labels[n],sizeof(save_labels[n]),"New save"); labels[n]=save_labels[n]; icons[n]=NULL; ++n; }
    for (int i=0;i<save_dialog.dir_count;++i) {
        save_label(save_labels[n],sizeof(save_labels[n]),save_dialog.dirs[i]); labels[n]=save_labels[n];
        icons[n]=runtime_savedata_icon(USER_ID,save_dialog.dirs[i],save_icons[n],sizeof(save_icons[n])) ? NULL : save_icons[n];
        ++n;
    }
    uint32_t type; memcpy(&type,p+56,4);
    const char *title=type==1 ? "Save game" : type==2 ? "Load game" : "Delete save";
    if (!n || !dcpgpu_choice_begin(title,labels,icons,n,0)) {
        printf("Runtime: SaveDataDialog list (%s): %s\n",title,n ? "no window, cancelled" : "no saves, cancelled");
        save_dialog.result=SAVE_RESULT_CANCELED;
        dialogs[2].status=DIALOG_FINISHED; dialogs[2].button=0;
        return 0;
    }
    printf("Runtime: SaveDataDialog list (%s): %d entries shown\n",title,n);
    save_dialog.choosing=1;
    dialogs[2].status=DIALOG_RUNNING;
    return 0;
}
static ABI int32_t save_status(void) {
    if (save_dialog.choosing) {
        int choice=dcpgpu_choice_poll();
        if (choice!=-1) {
            save_dialog.choosing=0;
            dialogs[2].status=DIALOG_FINISHED;
            if (choice<0) { save_dialog.result=SAVE_RESULT_CANCELED; dialogs[2].button=0; }
            else {
                int dir=choice-save_dialog.has_new;
                /* The chosen save goes first; the new item has no name. */
                if (dir>=0) memcpy(save_dialog.dirs[0],save_dialog.dirs[dir],32);
                else save_dialog.dirs[0][0]=0;
                save_dialog.dir_count=dir>=0 || save_dialog.dir_count ? 1 : 0;
                dialogs[2].button=1;
            }
            printf("Runtime: SaveDataDialog list: %s%s\n",choice<0 ? "cancelled" : "chose ",
                   choice<0 ? "" : save_dialog.dirs[0][0] ? save_dialog.dirs[0] : "new save");
        }
    }
    return dialog_status(2);
}
static ABI int32_t save_term(void) { return dialog_term(2); }
static ABI int32_t save_close(void) { return dialog_close(2); }
static ABI int32_t save_progress(uint32_t target, uint32_t rate) {
    if (target) return DIALOG_PARAM_INVALID;
    if (dialogs[2].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    save_dialog.progress=rate;
    return 0;
}
static ABI int32_t save_progress_inc(uint32_t target, uint32_t delta) {
    if (target) return DIALOG_PARAM_INVALID;
    if (dialogs[2].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    save_dialog.progress=delta>UINT32_MAX-save_dialog.progress ? UINT32_MAX : save_dialog.progress+delta;
    return 0;
}
/* OrbisSaveDataDialogResult: mode, result, buttonId, dirName* @16, param* @24, userData @32. */
static ABI int32_t save_result(unsigned char *out) {
    int32_t r=dialog_result(2,out,4);
    if (r) return r;
    memcpy(out,&save_dialog.mode,4);
    memcpy(out+4,&save_dialog.result,4);
    char *dir; unsigned char *param;
    memcpy(&dir,out+16,8); memcpy(&param,out+24,8);
    const char *name=save_dialog.dir_count && !save_dialog.result ? save_dialog.dirs[0] : "";
    if (dir) { memset(dir,0,32); memcpy(dir,name,strnlen(name,31)); }
    if (param && name[0]) runtime_savedata_param(USER_ID,name,param);
    memcpy(out+32,&save_dialog.user_data,8);
    return 0;
}
static ABI int32_t save_ready(void) { return 1; }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
#define IME_DIALOG_BUSY ((int32_t)0x80BC0001)
#define IME_DIALOG_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_DIALOG_INVALID_TYPE ((int32_t)0x80BC0011)
#define IME_DIALOG_INVALID_MAX_LENGTH ((int32_t)0x80BC0016)
#define IME_DIALOG_INVALID_BUFFER ((int32_t)0x80BC0017)
#define IME_DIALOG_INVALID_POSX ((int32_t)0x80BC0018)
#define IME_DIALOG_INVALID_POSY ((int32_t)0x80BC0019)
#define IME_DIALOG_INVALID_HALIGN ((int32_t)0x80BC001A)
#define IME_DIALOG_INVALID_VALIGN ((int32_t)0x80BC001B)
#define IME_DIALOG_INVALID_PARAM ((int32_t)0x80BC0030)
#define IME_DIALOG_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_DIALOG_INVALID_RESERVED ((int32_t)0x80BC0032)
#define IME_DIALOG_NOT_RUNNING ((int32_t)0x80BC0105)
#define IME_DIALOG_NOT_FINISHED ((int32_t)0x80BC0106)
#define IME_DIALOG_NOT_IN_USE ((int32_t)0x80BC0107)
static struct { int open, running, finished, end_status, overlay; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static int32_t ime_validate(const ImeParam *param) {
    if (param->type>4) return IME_DIALOG_INVALID_TYPE;
    const float limit_x=param->option&0x4000u ? 3840.0f : 1920.0f, limit_y=param->option&0x4000u ? 2160.0f : 1080.0f;
    if (!(param->x>=0.0f && param->x<limit_x)) return IME_DIALOG_INVALID_POSX;
    if (!(param->y>=0.0f && param->y<limit_y)) return IME_DIALOG_INVALID_POSY;
    if (param->halign>=3) return IME_DIALOG_INVALID_HALIGN;
    if (param->valign>=3) return IME_DIALOG_INVALID_VALIGN;
    if ((param->option&5u)==5u) return IME_DIALOG_INVALID_PARAM;
    if (param->user==-1 || param->user==0xFF || param->user<0) return IME_DIALOG_INVALID_USER_ID;
    for (size_t i=0;i<sizeof(param->reserved);++i) if (param->reserved[i]) return IME_DIALOG_INVALID_RESERVED;
    if (!param->buffer) return IME_DIALOG_INVALID_BUFFER;
    if (!param->max_length) return IME_DIALOG_INVALID_MAX_LENGTH;
    return 0;
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (ime.open) return IME_DIALOG_BUSY;
    if (!param) return IME_DIALOG_INVALID_ADDRESS;
    const int32_t invalid=ime_validate(param);
    if (invalid) return invalid;
    memset(&ime,0,sizeof(ime));
    ime.open=1; ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("SOS_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the player name (SOS_USER_NAME) or "Player": a controller alone
     * can accept it (the port has no on-screen keyboard). */
    const char *name=getenv("SOS_USER_NAME");
    const char *start=initial[0] ? initial : name && *name ? name : "Player";
    if (!dcpgpu_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else { ime.overlay=1; printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n"); }
    return 0;
}
static ABI int32_t ime_status(void) {
    if (!ime.open) return 0;
    if (ime.overlay) {
        char text[512];
        const int state=dcpgpu_text_input_poll(text,sizeof(text));
        if (state) {
            ime.overlay=0;
            if (ime.running) ime_complete(state==1 ? 0 : 1,text);
        }
    }
    return ime.running ? 1 : 2; /* Running / Finished */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.open) return IME_DIALOG_NOT_IN_USE;
    if (!result) return IME_DIALOG_INVALID_ADDRESS;
    for (size_t i=4;i<16;++i) if (((const unsigned char *)result)[i]) return IME_DIALOG_INVALID_RESERVED;
    if (ime.running) return IME_DIALOG_NOT_FINISHED;
    *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_abort(void) {
    if (!ime.open) return IME_DIALOG_NOT_IN_USE;
    if (!ime.running) return IME_DIALOG_NOT_RUNNING;
    ime.end_status=2; ime.finished=1; ime.running=0; /* Aborted */
    return 0;
}
static ABI int32_t ime_term(void) {
    if (!ime.open) return IME_DIALOG_NOT_IN_USE;
    if (ime.running) return IME_DIALOG_NOT_FINISHED;
    memset(&ime,0,sizeof(ime));
    return 0;
}
/* The system panel's size, which the game centers the dialog with. Sizes of SDK 2.00 and later
 * (the eboot is SDK 4.50); the port draws its own box, so `extended` does not change them. */
#define IME_TYPE_NUMBER 4
#define IME_OPTION_MULTILINE 1u
#define IME_OPTION_OVER_2K 0x4000u
static ABI int32_t ime_panel_size_extended(const ImeParam *param, const void *extended, uint32_t *width, uint32_t *height) {
    (void)extended;
    if (!param || !width || !height) return (int32_t)0x80BC0031; /* IME INVALID_ADDRESS */
    if (param->type>IME_TYPE_NUMBER) return (int32_t)0x80BC0011; /* IME INVALID_TYPE */
    uint32_t w=0x319, h=param->option&IME_OPTION_MULTILINE ? 0x274 : 0x210;
    if (param->type==IME_TYPE_NUMBER) { w=0x172; h=0x20a; }
    if (param->option&IME_OPTION_OVER_2K) { w<<=1; h<<=1; }
    *width=w; *height=h;
    return 0;
}

#define MAX_USERS 4
/* ---- Ime keyboard: a registration per user. Key events are not delivered, so the handler is never called. ---- */
#define IME_ERR_BUSY ((int32_t)0x80BC0001)
#define IME_ERR_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_ERR_CONNECTION_FAILED ((int32_t)0x80BC0004)
#define IME_ERR_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_ERR_INVALID_OPTION ((int32_t)0x80BC0015)
#define IME_ERR_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_ERR_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_ERR_INVALID_RESERVED ((int32_t)0x80BC0032)
#define IME_KEYBOARD_OPTION_MASK 0x3Fu
typedef struct { uint32_t option; uint8_t reserved1[4]; void *arg; void *handler; uint8_t reserved2[8]; } ImeKeyboardParam;
_Static_assert(sizeof(ImeKeyboardParam)==32,"OrbisImeKeyboardParam is 32 bytes");
static int ime_keyboard_open_for[MAX_USERS];
static pthread_mutex_t ime_keyboard_lock=PTHREAD_MUTEX_INITIALIZER;
static int ime_user_slot(int32_t user) { const int player=runtime_user_player(user); return player ? player-1 : -1; }
static int all_zero(const uint8_t *bytes,size_t size) { for (size_t i=0;i<size;++i) if (bytes[i]) return 0; return 1; }
static ABI int32_t ime_keyboard_open(int32_t user,const ImeKeyboardParam *param) {
    if (!param) return IME_ERR_INVALID_ADDRESS;
    if (!param->handler) return IME_ERR_INVALID_HANDLER;
    if (param->option&~IME_KEYBOARD_OPTION_MASK) return IME_ERR_INVALID_OPTION;
    const int slot=ime_user_slot(user);
    if (slot<0) return IME_ERR_INVALID_USER_ID;
    if (!all_zero(param->reserved1,sizeof(param->reserved1)) || !all_zero(param->reserved2,sizeof(param->reserved2))) return IME_ERR_INVALID_RESERVED;
    pthread_mutex_lock(&ime_keyboard_lock);
    const int busy=ime_keyboard_open_for[slot];
    ime_keyboard_open_for[slot]=1;
    pthread_mutex_unlock(&ime_keyboard_lock);
    if (busy) return IME_ERR_BUSY;
    printf("Runtime: ImeKeyboard opened for user %d: key events are not delivered to the game\n",user);
    return 0;
}
static ABI int32_t ime_keyboard_close(int32_t user) {
    const int slot=ime_user_slot(user);
    if (slot<0) return IME_ERR_INVALID_USER_ID;
    pthread_mutex_lock(&ime_keyboard_lock);
    const int was_open=ime_keyboard_open_for[slot];
    ime_keyboard_open_for[slot]=0;
    pthread_mutex_unlock(&ime_keyboard_lock);
    return was_open ? 0 : IME_ERR_NOT_OPENED;
}
static ABI int32_t ime_update(const void *handler) {
    (void)handler;
    pthread_mutex_lock(&ime_keyboard_lock);
    int any_open=0;
    for (int i=0;i<MAX_USERS;++i) any_open|=ime_keyboard_open_for[i];
    pthread_mutex_unlock(&ime_keyboard_lock);
    return any_open ? 0 : IME_ERR_NOT_OPENED;
}
typedef struct { uint32_t user_id; uint32_t resource_id[5]; } ImeKeyboardResourceIds;
typedef struct {
    uint32_t user_id, device, type, repeat_delay, repeat_rate, status;
    int8_t reserved[12];
} ImeKeyboardInfo;
_Static_assert(sizeof(ImeKeyboardResourceIds)==24 && sizeof(ImeKeyboardInfo)==36,"IME keyboard layouts");
/* No USB keyboard: the buffer is initialized and CONNECTION_FAILED stops Unity's probe, as shadPS4 does. */
static ABI int32_t ime_keyboard_resource_ids(int32_t user, ImeKeyboardResourceIds *out) {
    if (!out) return IME_ERR_INVALID_ADDRESS;
    if (ime_user_slot(user)<0) return IME_ERR_INVALID_USER_ID;
    memset(out,0,sizeof(*out));
    out->user_id=(uint32_t)user;
    return IME_ERR_CONNECTION_FAILED;
}
static ABI int32_t ime_keyboard_info(uint32_t resource, ImeKeyboardInfo *info) {
    (void)resource;
    if (info) memset(info,0,sizeof(*info));
    return 0;
}

/* ---- Trophies: kept on this PC (src/trophies) ---- */
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) { return trophy_runtime_create_context(ctx,user,label,options); }
static ABI int32_t trophy_handle(int32_t *handle) { return trophy_runtime_create_handle(handle); }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { return trophy_runtime_register_context(ctx,handle,options); }
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) { return trophy_runtime_unlock(ctx,handle,id,platinum); }
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) { return trophy_runtime_game_info(ctx,handle,details,data); }
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) { return trophy_runtime_trophy_info(ctx,handle,id,details,data); }
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) { return trophy_runtime_unlock_state(ctx,handle,flags,count); }
static ABI int32_t trophy_abort_handle(int32_t handle) { (void)handle; return 0; }
static ABI int32_t trophy_destroy_handle(int32_t handle) { return trophy_runtime_destroy_handle(handle); }
static ABI int32_t trophy_destroy_context(int32_t ctx) { return trophy_runtime_destroy_context(ctx); }

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=2; return 0; /* Full: nothing left to install */
}
/* Progress of installed chunks: done (1 of 1, never a zero total to divide by). */
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    out[0]=out[1]=1; return 0;
}
static ABI int32_t playgo_todo(int32_t handle,const void *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    return count ? 0 : PLAYGO_BAD_SIZE; /* everything is installed: nothing to schedule */
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_terminate(void) { playgo_handle=0; return 0; }
static uint64_t playgo_language_mask=~UINT64_C(0); /* every language chunk is installed */
static ABI int32_t playgo_get_language_mask(int32_t handle,uint64_t *mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!mask) return PLAYGO_BAD_POINTER;
    *mask=playgo_language_mask; return 0;
}
static ABI int32_t playgo_set_language_mask(int32_t handle,uint64_t mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_language_mask=mask; return 0;
}
static ABI int32_t playgo_get_todo(int32_t handle,void *list,uint32_t count,uint32_t *entries) {
    (void)count;
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list || !entries) return PLAYGO_BAD_POINTER;
    *entries=0; return 0; /* nothing left to install */
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0; return 0;
}
static ABI int32_t playgo_prefetch(int32_t handle,const uint16_t *ids,uint32_t count,int32_t locus) {
    (void)locus;
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids) return PLAYGO_BAD_POINTER;
    return count ? 0 : PLAYGO_BAD_SIZE;
}

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* sceDiscMap 0x7C980FFB0AA27E7A (libSceFios2, same arguments): no disc bitmap, nothing pending. */
static ABI int32_t discmap_7c98(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size;
    if (flags) *flags=0;
    if (r1) *r1=0;
    if (r2) *r2=0;
    return ORBIS_OK;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
#define MOUSE_INVALID_ARG ((int32_t)0x80DF0001)
#define MOUSE_INVALID_HANDLE ((int32_t)0x80DF0003)
#define MOUSE_ALREADY_OPENED ((int32_t)0x80DF0004)
static int mouse_handles[2]={-1,-1};
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const unsigned char *param) {
    if (!runtime_user_player(user) || type!=0 || index<0 || index>1) return MOUSE_INVALID_ARG;
    const int merged=param && (param[0]&1);
    if (merged && index!=0) return MOUSE_ALREADY_OPENED;
    pthread_mutex_lock(&lock);
    const int opened=mouse_handles[index]!=-1;
    if (!opened) mouse_handles[index]=index;
    pthread_mutex_unlock(&lock);
    return opened ? MOUSE_ALREADY_OPENED : index;
}
static int mouse_slot(int32_t handle) { return handle>=0 && handle<2 && mouse_handles[handle]==handle ? handle : -1; }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    if (!data || count<1 || count>64) return MOUSE_INVALID_ARG;
    pthread_mutex_lock(&lock);
    const int slot=mouse_slot(handle);
    pthread_mutex_unlock(&lock);
    if (slot<0) return MOUSE_INVALID_HANDLE;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) {
    pthread_mutex_lock(&lock);
    const int slot=mouse_slot(handle);
    if (slot>=0) mouse_handles[slot]=-1;
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : MOUSE_INVALID_HANDLE;
}
/* No capture device exists: no port ever opens, so every handle is either malformed or not open. */
#define AUDIO_IN_INVALID_HANDLE ((int32_t)0x80260101)
#define AUDIO_IN_INVALID_SIZE ((int32_t)0x80260102)
#define AUDIO_IN_INVALID_FREQ ((int32_t)0x80260103)
#define AUDIO_IN_INVALID_POINTER ((int32_t)0x80260105)
#define AUDIO_IN_INVALID_PARAM ((int32_t)0x80260106)
static ABI int32_t audio_in_open(int32_t user,uint32_t type,uint32_t index,uint32_t length,uint32_t freq,uint32_t param) {
    (void)user; (void)type; (void)index;
    if (!length || length>2048) return AUDIO_IN_INVALID_SIZE;
    if (param!=0 && param!=2) return AUDIO_IN_INVALID_PARAM;
    if (freq!=16000 && freq!=48000) return AUDIO_IN_INVALID_FREQ;
    return AUDIO_IN_NOT_OPENED;
}
static int32_t audio_in_handle(int32_t handle) {
    return (handle&0x7f000000)!=0x30000000 || (handle&0xff)>=7 ? AUDIO_IN_INVALID_HANDLE : AUDIO_IN_NOT_OPENED;
}
static ABI int32_t audio_in_close(int32_t handle) { return audio_in_handle(handle); }
static ABI int32_t audio_in_input(int32_t handle,void *dest) {
    const int32_t r=audio_in_handle(handle);
    return r!=AUDIO_IN_NOT_OPENED ? r : dest ? r : AUDIO_IN_INVALID_POINTER;
}
static ABI int32_t audio_in_silent_state(int32_t handle) { return audio_in_handle(handle); }


/* ---- NpWebApi offline: contexts and requests exist, any transfer reports that nobody is signed in ---- */
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define WEBAPI_LIB_CONTEXT_NOT_FOUND ((int32_t)0x80552904)
#define WEBAPI_USER_CONTEXT_NOT_FOUND ((int32_t)0x80552905)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552906)
#define WEBAPI_NOT_SIGNED_IN ((int32_t)0x80552907)
#define WEBAPI_INVALID_CONTENT_PARAMETER ((int32_t)0x80552908)
#define WEBAPI_USER_CONTEXT_ALREADY_EXIST ((int32_t)0x8055290A)
#define WEBAPI_LIB_CONTEXT_MAX ((int32_t)0x8055291A)
#define WEBAPI_USER_CONTEXT_MAX ((int32_t)0x8055291B)
#define HTTP_INVALID_ID ((int32_t)0x80431100)
#define WEBAPI_LIBS 8
#define WEBAPI_USERS 16
#define WEBAPI_REQUESTS 64
static struct { int32_t id; } webapi_libs[WEBAPI_LIBS];
static struct { int32_t id, user; } webapi_users[WEBAPI_USERS];
static struct { int64_t id; } webapi_requests[WEBAPI_REQUESTS];
static int32_t webapi_next_lib, webapi_next_user;
static uint32_t webapi_next_request;
static int webapi_lib_slot(int32_t id) { for (int i=0;i<WEBAPI_LIBS;++i) if (id && webapi_libs[i].id==id) return i; return -1; }
static int webapi_user_slot(int32_t id) { for (int i=0;i<WEBAPI_USERS;++i) if (id && webapi_users[i].id==id) return i; return -1; }
static int webapi_request_slot(int64_t id) { for (int i=0;i<WEBAPI_REQUESTS;++i) if (id && webapi_requests[i].id==id) return i; return -1; }
static ABI int32_t webapi_initialize(int32_t http_context, uint64_t pool_size) {
    (void)http_context; (void)pool_size;
    pthread_mutex_lock(&lock);
    int32_t r=WEBAPI_LIB_CONTEXT_MAX;
    for (int i=0;i<WEBAPI_LIBS;++i) if (!webapi_libs[i].id) {
        do { if (++webapi_next_lib>=0x8000) webapi_next_lib=1; } while (webapi_lib_slot(webapi_next_lib)>=0);
        webapi_libs[i].id=r=webapi_next_lib;
        break;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static void webapi_drop_user(int slot) {
    for (int i=0;i<WEBAPI_REQUESTS;++i)
        if (webapi_requests[i].id && (int32_t)(webapi_requests[i].id>>32)==webapi_users[slot].id) webapi_requests[i].id=0;
    webapi_users[slot].id=0;
}
static ABI int32_t webapi_terminate(int32_t lib) {
    pthread_mutex_lock(&lock);
    const int slot=webapi_lib_slot(lib);
    if (slot>=0) {
        webapi_libs[slot].id=0;
        for (int i=0;i<WEBAPI_USERS;++i) if (webapi_users[i].id && (webapi_users[i].id>>16)==lib) webapi_drop_user(i);
    }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : WEBAPI_LIB_CONTEXT_NOT_FOUND;
}
static int32_t webapi_create_context(int32_t lib, int32_t user) {
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    if (user==-1) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int32_t r=WEBAPI_LIB_CONTEXT_NOT_FOUND;
    if (webapi_lib_slot(lib)>=0) {
        r=WEBAPI_USER_CONTEXT_MAX;
        for (int i=0;i<WEBAPI_USERS;++i) if (webapi_users[i].id && (webapi_users[i].id>>16)==lib && webapi_users[i].user==user) r=WEBAPI_USER_CONTEXT_ALREADY_EXIST;
        for (int i=0;i<WEBAPI_USERS && r==WEBAPI_USER_CONTEXT_MAX;++i) if (!webapi_users[i].id) {
            do { if (++webapi_next_user>=0x10000) webapi_next_user=1; } while (webapi_user_slot((lib<<16)|webapi_next_user)>=0);
            webapi_users[i].id=(lib<<16)|webapi_next_user; webapi_users[i].user=user;
            r=webapi_users[i].id;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t webapi_create_context_a(int32_t lib, int32_t user) { return webapi_create_context(lib,user); }
static ABI int32_t webapi_create_context_online(int32_t lib, const void *online_id) { (void)online_id; return webapi_create_context(lib,USER_ID); }
static int32_t webapi_user_found(int32_t user_context) {
    if (webapi_lib_slot(user_context>>16)<0) return WEBAPI_LIB_CONTEXT_NOT_FOUND;
    return webapi_user_slot(user_context)<0 ? WEBAPI_USER_CONTEXT_NOT_FOUND : 0;
}
static ABI int32_t webapi_delete_context(int32_t user_context) {
    pthread_mutex_lock(&lock);
    int32_t r=webapi_user_found(user_context);
    if (!r) webapi_drop_user(webapi_user_slot(user_context));
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t webapi_create_request(int32_t user_context, const char *group, const char *path, int32_t method,
                                         const struct { uint64_t length; const char *type; } *content, int64_t *out) {
    if (!group || !path) return WEBAPI_INVALID_ARGUMENT;
    if (content && content->length && !content->type) return WEBAPI_INVALID_CONTENT_PARAMETER;
    if ((uint32_t)method>3) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int32_t r=webapi_user_found(user_context);
    if (!r) {
        r=WEBAPI_INVALID_ARGUMENT;
        for (int i=0;i<WEBAPI_REQUESTS;++i) if (!webapi_requests[i].id) {
            int64_t id;
            do { if (!++webapi_next_request) webapi_next_request=1; id=((int64_t)user_context<<32)|webapi_next_request; } while (webapi_request_slot(id)>=0);
            webapi_requests[i].id=id;
            if (out) *out=id;
            r=0;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static int32_t webapi_request_found(int64_t id) {
    if (webapi_lib_slot((int32_t)(id>>48))<0) return WEBAPI_LIB_CONTEXT_NOT_FOUND;
    if (webapi_user_slot((int32_t)(id>>32))<0) return WEBAPI_USER_CONTEXT_NOT_FOUND;
    return webapi_request_slot(id)<0 ? WEBAPI_REQUEST_NOT_FOUND : 0;
}
static int32_t webapi_with_request(int64_t id, int32_t found) {
    pthread_mutex_lock(&lock);
    const int32_t r=webapi_request_found(id);
    pthread_mutex_unlock(&lock);
    return r ? r : found;
}
static ABI int32_t webapi_delete_request(int64_t id) {
    pthread_mutex_lock(&lock);
    const int32_t r=webapi_request_found(id);
    if (!r) webapi_requests[webapi_request_slot(id)].id=0;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t webapi_send_request(int64_t id, const void *data, uint64_t size) {
    (void)data; (void)size;
    return webapi_with_request(id,WEBAPI_NOT_SIGNED_IN);
}
static ABI int32_t webapi_send_request2(int64_t id, const void *data, uint64_t size, void *response_option) {
    (void)response_option;
    return webapi_send_request(id,data,size);
}
static ABI int32_t webapi_read_data(int64_t id, void *data, uint64_t size) {
    if (!data || !size) return WEBAPI_INVALID_ARGUMENT;
    return webapi_with_request(id,0);
}
static ABI int32_t webapi_add_header(int64_t id, const char *name, const char *value) {
    if (!name || !value) return WEBAPI_INVALID_ARGUMENT;
    return webapi_with_request(id,0);
}
static ABI int32_t webapi_header_value(int64_t id, const char *name, char *value, uint64_t size) {
    if (!name || !value || !size) return WEBAPI_INVALID_ARGUMENT;
    return webapi_with_request(id,HTTP_INVALID_ID);
}
static ABI int32_t webapi_header_length(int64_t id, const char *name, uint64_t *length) {
    if (!name || !length) return WEBAPI_INVALID_ARGUMENT;
    return webapi_with_request(id,HTTP_INVALID_ID);
}
static ABI int32_t webapi_status_code(int64_t id, int32_t *status) {
    (void)status;
    return webapi_with_request(id,HTTP_INVALID_ID);
}
static ABI int32_t np_user_signed_out(int32_t user, void *out) {
    if (user==-1 || !out) return NP_INVALID_ARGUMENT;
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_id_a(int32_t user, uint64_t *account) {
    if (!account || user==-1) return NP_INVALID_ARGUMENT;
    *account=0;
    return NP_SIGNED_OUT;
}

/* ---- Peripherals and companion services the host does not provide ---- */
#define WEB_BROWSER_DIALOG 8
#define SOCIAL_SCREEN_DIALOG 9
#define HMD_SETUP_DIALOG 10
#define DIALOG_RESULT_USER_CANCELED 1
DIALOG(web_browser,WEB_BROWSER_DIALOG) DIALOG(social_dialog,SOCIAL_SCREEN_DIALOG) DIALOG(hmd_setup,HMD_SETUP_DIALOG)
static ABI int32_t web_browser_close(void) { return dialog_close(WEB_BROWSER_DIALOG); }
static ABI int32_t social_dialog_close(void) { return dialog_close(SOCIAL_SCREEN_DIALOG); }
static ABI int32_t hmd_setup_close(void) { return dialog_close(HMD_SETUP_DIALOG); }
/* There is no browser, second screen or headset: the dialog finishes as cancelled. */
static int32_t dialog_canceled_result(int i, int32_t *out) {
    if (dialogs[i].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    out[0]=DIALOG_RESULT_USER_CANCELED;
    return 0;
}
static ABI int32_t web_browser_result(int32_t *out) { return dialog_canceled_result(WEB_BROWSER_DIALOG,out); }
static ABI int32_t social_dialog_result(int32_t *out) { return dialog_canceled_result(SOCIAL_SCREEN_DIALOG,out); }
static ABI int32_t hmd_setup_result(int32_t *out) { return dialog_canceled_result(HMD_SETUP_DIALOG,out); }
#define REMOTEPLAY_STATUS_DISCONNECTED 0
static ABI int32_t remoteplay_status(int32_t user, int32_t *status) {
    (void)user;
    if (status) *status=REMOTEPLAY_STATUS_DISCONNECTED;
    return 0;
}

/* ---- libSceSsl certificate queries: no certificates exist offline ---- */
typedef struct { void *certs; uint64_t count; void *pool; } SslCaCerts;
static ABI int32_t ssl_ca_certs(int32_t context, SslCaCerts *certs) {
    (void)context;
    if (!certs) return SSL_INVALID_ID;
    memset(certs,0,sizeof(*certs));
    return 0;
}
static ABI int32_t ssl_pool_stats(int32_t context, uint64_t *stats) {
    (void)context;
    if (!stats) return SSL_INVALID_ID;
    memset(stats,0,3*sizeof(*stats));
    return 0;
}

/* ---- libSceHttp URI parsing: pure string work, independent of the network ---- */
#define HTTP_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_INVALID_URL ((int32_t)0x80433060)
#define HTTP_URI_COMPONENT_MAX 0x3fff
typedef struct {
    uint8_t opaque; char *scheme, *username, *password, *hostname, *path, *query, *fragment;
    uint16_t port; uint8_t reserved[10];
} HttpUriElement;
static int http_userinfo_punct(unsigned char c) {
    return c==0x21 || (c>=0x24 && c<=0x2e) || c==0x3a || c==0x3b || c==0x3d || c==0x5f || c==0x7e;
}
static int http_host_char(unsigned char c) { return c<0x80 && (isalnum(c) || c=='-' || c=='.' || c=='_'); }
static void http_sweep_path(char *dst, const char *src, uint64_t size) {
    if (!size) return;
    if (src[0]!='/') { memcpy(dst,src,size-1); dst[size-1]=0; return; }
    dst[0]='/'; dst[1]=0;
    if (size-1<=1) return;
    uint64_t pos=1;
    char *end=dst;
    while (pos<size-1) {
        if (src[pos]=='.') {
            if (src[pos+1]=='/') { pos+=2; continue; }
            if (src[pos+1]=='.' && src[pos+2]=='/') {
                char *next=dst;
                if (end!=dst) {
                    *end=0;
                    char *slash=strrchr(dst,'/');
                    if (!slash) return;
                    slash[1]=0;
                    next=slash;
                }
                pos+=3;
                end=next;
                continue;
            }
        }
        const char *slash=strchr(src+pos,'/');
        const uint64_t remaining=size-pos-1;
        uint64_t length=remaining;
        if (slash) { const uint64_t segment=(uint64_t)(slash+1-(src+pos)); length=segment<=remaining ? segment : remaining; }
        memcpy(end+1,src+pos,length);
        end[length+1]=0;
        end+=length;
        pos+=length;
    }
}
static ABI int32_t http_uri_parse(HttpUriElement *out, const char *uri, void *pool, uint64_t *require, uint64_t prepare) {
    if (!uri) return HTTP_INVALID_URL;
    const int write=out && pool;
    if (!write && !require) return HTTP_INVALID_VALUE;
    if (write) memset(out,0,sizeof(*out));
    char *bytes=pool;
    uint64_t scheme_length=0, used, offset;
    while (scheme_length<0x20 && uri[scheme_length] && uri[scheme_length]!=':') {
        const unsigned char c=(unsigned char)uri[scheme_length];
        if (!isalnum(c) && c!='+' && c!='-' && c!='.') break;
        ++scheme_length;
    }
    if (scheme_length && uri[scheme_length]==':' && isalpha((unsigned char)uri[0])) {
        if (write) {
            if (prepare<scheme_length+1) return HTTP_OUT_OF_MEMORY;
            memcpy(bytes,uri,scheme_length); bytes[scheme_length]=0; out->scheme=bytes;
        }
        used=scheme_length+1; offset=scheme_length+1;
    } else {
        if (write) {
            if (prepare<2) return HTTP_OUT_OF_MEMORY;
            bytes[0]=0; out->scheme=bytes;
        }
        used=1; offset=0;
    }
    int slashes=0;
    for (const char *p=uri+offset;*p=='/';++p) ++slashes;
    if (slashes>=2) offset+=2; else if (write) out->opaque=1;

    const char *authority=uri+offset;
    uint64_t scan=0, colon=0, at=0;
    int seen_colon=0, seen_at=0;
    for (;;) {
        const unsigned char c=(unsigned char)authority[scan];
        if (!c) break;
        if (c=='@') { seen_at=1; at=scan; break; }
        if (!seen_colon && c==':') { seen_colon=1; colon=scan; }
        else if (c>=0x80 || (!isalnum(c) && !http_userinfo_punct(c))) break;
        ++scan;
    }
    char *user=bytes+used;
    if (seen_at) {
        const uint64_t user_length=seen_colon ? colon : at;
        const uint64_t pass_offset=seen_colon ? colon+1 : at+1;
        const uint64_t pass_length=seen_colon ? at-pass_offset : 0;
        if (write) {
            if (prepare-used<pass_offset+pass_length+1) return HTTP_OUT_OF_MEMORY;
            memcpy(user,authority,user_length); user[user_length]=0;
            memcpy(user+pass_offset,authority+pass_offset,pass_length); user[pass_offset+pass_length]=0;
            out->username=user; out->password=user+pass_offset;
        }
        used+=pass_offset+pass_length+1;
        offset+=at+1;
    } else {
        if (write) {
            if (prepare-used<2) return HTTP_OUT_OF_MEMORY;
            user[0]=0; user[1]=0; out->username=user; out->password=user+1;
        }
        used+=2;
    }

    char *host=bytes+used;
    const char *host_start=uri+offset;
    uint64_t host_scan=0, host_stored=0;
    if (host_start[0]=='[') {
        host_scan=1;
        for (;;) {
            if (host_scan==0xff) return HTTP_INVALID_URL;
            const unsigned char c=(unsigned char)host_start[host_scan];
            if (!c || c>=0x80 || c==']') break;
            if (!isalnum(c) && c!='-' && c!='.' && c!='_' && c!=':') break;
            ++host_scan;
        }
        if (host_start[host_scan]!=']') return HTTP_INVALID_URL;
        host_stored=host_scan-1;
        ++host_scan;
    } else if (host_start[0]!='.') {
        while (http_host_char((unsigned char)host_start[host_scan])) {
            if (++host_scan==0xff) return HTTP_INVALID_URL;
        }
        host_stored=host_scan;
    }
    if (write) {
        if (prepare-used<host_stored+1) return HTTP_OUT_OF_MEMORY;
        memcpy(host,host_start[0]=='[' ? host_start+1 : host_start,host_stored);
        host[host_stored]=0; out->hostname=host;
    }
    used+=host_stored+1;
    offset+=host_scan;

    int explicit_port=0;
    uint32_t port=0;
    if (uri[offset]==':') {
        ++offset;
        uint64_t digits=0;
        while (digits<5 && isdigit((unsigned char)uri[offset+digits])) port=port*10+(uint32_t)(uri[offset+digits++]-'0');
        if (port>0x10000) return HTTP_INVALID_URL;
        const char after=uri[offset+digits];
        if (after && after!='/') return HTTP_INVALID_URL;
        explicit_port=digits>0;
        offset+=digits;
    }
    if (write) {
        if (explicit_port) out->port=(uint16_t)port;
        else if (strlen(out->scheme)<=0x20) {
            if (!strncasecmp(out->scheme,"https",5)) out->port=443;
            else if (!strncasecmp(out->scheme,"http",4) || !strncasecmp(out->scheme,"ttp",3)) out->port=80;
        }
    }

    char *path=bytes+used;
    uint64_t path_length=0;
    while (uri[offset+path_length] && uri[offset+path_length]!='?' && uri[offset+path_length]!='#') {
        if (path_length>=HTTP_URI_COMPONENT_MAX) return HTTP_INVALID_URL;
        ++path_length;
    }
    if (write) {
        if (prepare-used<path_length+1) return HTTP_OUT_OF_MEMORY;
        char *copy=malloc(path_length+1);
        if (!copy) return HTTP_OUT_OF_MEMORY;
        memcpy(copy,uri+offset,path_length); copy[path_length]=0;
        memcpy(path,copy,path_length+1);
        http_sweep_path(path,copy,path_length+1);
        free(copy);
        out->path=path;
    }
    used+=path_length+1;
    offset+=path_length;

    char *query=bytes+used;
    uint64_t query_length=0;
    if (uri[offset]=='?') {
        query_length=1;
        while (uri[offset+query_length] && uri[offset+query_length]!='#') {
            if (query_length>=HTTP_URI_COMPONENT_MAX) return HTTP_INVALID_URL;
            ++query_length;
        }
    }
    if (write) {
        if (prepare-used<query_length+1) return HTTP_OUT_OF_MEMORY;
        memcpy(query,uri+offset,query_length); query[query_length]=0; out->query=query;
    }
    used+=query_length+1;
    offset+=query_length;

    char *fragment=bytes+used;
    uint64_t fragment_length=0;
    if (uri[offset]=='#') {
        fragment_length=1;
        while (uri[offset+fragment_length]) {
            if (fragment_length>=HTTP_URI_COMPONENT_MAX) return HTTP_INVALID_URL;
            ++fragment_length;
        }
    }
    if (write) {
        if (prepare-used<fragment_length+1) return HTTP_OUT_OF_MEMORY;
        memcpy(fragment,uri+offset,fragment_length); fragment[fragment_length]=0; out->fragment=fragment;
    }
    used+=fragment_length+1;
    if (require) *require=used;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceSystemServiceReenableMusicPlayer",reenable_music_player},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceParamGetString",system_param_string},
    {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceReportAbnormalTermination",report_abnormal_termination},
    {"sceSystemServiceGetHdrToneMapLuminance",hdr_luminance},
    {"sceSystemServicePowerTick",power_tick},
    {"sceSystemServiceEnableSuspendConfirmationDialog",dialog_ok},
    {"sceSystemServiceDisableSuspendConfirmationDialog",dialog_ok},
    {"sceSystemServiceShowControllerSettings",dialog_ok},
    {"sceSystemServiceLaunchWebBrowser",launch_browser}, {"sceSystemServiceLoadExec",load_exec},
    {"sceRemoteplayInitialize",ok_void}, {"sceRemoteplayApprove",ok_void}, {"sceRemoteplayProhibit",ok_void},
    {"sceRemoteplayProhibitStreaming",ok_void}, {"sceRemoteplayGetConnectionStatus",remoteplay_status},
    {"sceSystemServiceEnablePersonalEyeToEyeDistanceSetting",dialog_ok},
    {"sceSystemServiceDisablePersonalEyeToEyeDistanceSetting",dialog_ok},
    {"sceSocialScreenInitialize",ok_void}, {"sceSocialScreenTerminate",ok_void}, {"sceSocialScreenSetMode",ok_void},
    {"sceSocialScreenInitializeSeparateModeParameter",ok_void}, {"sceSocialScreenConfigureSeparateMode",ok_void},
    {"sceSocialScreenOpenSeparateMode",ok_void}, {"sceSocialScreenCloseSeparateMode",ok_void},
    {"sceSocialScreenDialogInitialize",social_dialog_init}, {"sceSocialScreenDialogOpen",social_dialog_open},
    {"sceSocialScreenDialogUpdateStatus",social_dialog_status}, {"sceSocialScreenDialogTerminate",social_dialog_term},
    {"sceSocialScreenDialogClose",social_dialog_close}, {"sceSocialScreenDialogGetResult",social_dialog_result},
    {"sceWebBrowserDialogInitialize",web_browser_init}, {"sceWebBrowserDialogOpen",web_browser_open},
    {"sceWebBrowserDialogUpdateStatus",web_browser_status}, {"sceWebBrowserDialogGetStatus",web_browser_status},
    {"sceWebBrowserDialogTerminate",web_browser_term}, {"sceWebBrowserDialogClose",web_browser_close},
    {"sceWebBrowserDialogGetResult",web_browser_result},
    {"sceHmdSetupDialogInitialize",hmd_setup_init}, {"sceHmdSetupDialogOpen",hmd_setup_open},
    {"sceHmdSetupDialogUpdateStatus",hmd_setup_status}, {"sceHmdSetupDialogGetStatus",hmd_setup_status},
    {"sceHmdSetupDialogTerminate",hmd_setup_term}, {"sceHmdSetupDialogClose",hmd_setup_close},
    {"sceHmdSetupDialogGetResult",hmd_setup_result},
    {"sceSslGetCaCerts",ssl_ca_certs}, {"sceSslFreeCaCerts",ok_void}, {"sceSslGetPem",ssl_no_certificate},
    {"sceSslGetSubjectName",ssl_no_certificate}, {"sceSslGetIssuerName",ssl_no_certificate},
    {"sceSslGetNameEntryCount",ssl_no_certificate}, {"sceSslGetNameEntryInfo",ssl_no_certificate},
    {"sceSslFreeSslCertName",ok_void}, {"sceSslGetMemoryPoolStats",ssl_pool_stats},
    {"sceHttpUriParse",http_uri_parse}, {"sceHttpCreateConnection",http_object}, {"sceHttpCreateRequest2",http_object},
    {"sceHttpAbortRequest",ok_void}, {"sceHttpCookieFlush",ok_void}, {"sceHttpSetAuthEnabled",ok_void},
    {"sceHttpSetChunkedTransferEnabled",ok_void}, {"sceHttpSetRecvTimeOut",ok_void},
    {"sceHttpSetSendTimeOut",ok_void}, {"sceHttpSetRedirectCallback",ok_void},
    {"sceSystemServiceGetDisplaySafeAreaInfo",safe_area}, {"sceSystemServiceDisableMusicPlayer",disable_music_player},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlInit",netctl_init}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void}, {"sceSslGetSerialNumber",ssl_no_certificate},
    {"sceHttpsSetSslCallback",ok_void}, {"sceHttpGetAllResponseHeaders",http_fail}, {"sceNetGetSockInfo",net_unreachable},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_user_signed_out}, {"sceNpGetNpId",np_user_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    /* NpToolkit2 (linked from the game's sce_module) and the A/AccountId variants: signed out. */
    {"sceNetCtlRegisterCallbackForNpToolkit",netctl_register}, {"sceNetCtlCheckCallbackForNpToolkit",netctl_check},
    {"sceNetCtlUnregisterCallbackForNpToolkit",netctl_unregister},
    {"sceNpRegisterStateCallbackForToolkit",np_register}, {"sceNpUnregisterStateCallbackForToolkit",ok_void},
    {"sceNpCheckCallbackForLib",ok_void},
    {"sceNpCreateRequest",np_request}, {"sceNpCheckNpAvailabilityA",np_signed_out},
    {"sceNpGetAccountIdA",np_account_id_a}, {"sceNpGetAccountId",np_signed_out}, {"sceNpGetParentalControlInfoA",np_signed_out},
    {"sceNpGetUserIdByAccountId",np_signed_out},
    {"sceNpInGameMessageInitialize",lib_init_id}, {"sceNpInGameMessageTerminate",ok_void},
    {"sceNpInGameMessageCreateHandle",np_request_ctx}, {"sceNpInGameMessageDeleteHandle",ok_void},
    {"sceNpInGameMessagePrepareA",np_signed_out}, {"sceNpInGameMessageSendDataA",np_signed_out},
    {"sceNpAuthCreateRequest",np_request}, {"sceNpAuthAbortRequest",ok_void},
    {"sceNpAuthGetAuthorizationCodeA",np_signed_out}, {"sceNpAuthGetIdTokenA",np_signed_out},
    {"sceNpTusCreateNpTitleCtxA",np_request_ctx}, {"sceNpTssCreateNpTitleCtxA",np_request_ctx},
    {"sceNpTssCreateNpTitleCtx",np_request_ctx},
    {"sceNpTusDeleteNpTitleCtx",ok_void}, {"sceNpTusCreateRequest",np_request}, {"sceNpTusDeleteRequest",ok_void},
    {"sceNpTusAbortRequest",ok_void}, {"sceNpTusPollAsync",np_poll},
    {"sceNpTusGetDataAAsync",np_signed_out}, {"sceNpTusSetDataAAsync",np_signed_out},
    {"sceNpTusSetDataA",np_signed_out}, {"sceNpTusGetDataForCrossSave",np_signed_out},
    {"sceNpTusGetMultiSlotDataStatusAAsync",np_signed_out}, {"sceNpTusDeleteMultiSlotDataAAsync",np_signed_out},
    {"sceNpTssGetDataAsync",np_signed_out}, {"sceNpTssGetData",np_signed_out},
    {"sceNpScoreCreateNpTitleCtxA",np_request_ctx}, {"sceNpScorePollAsync",np_poll},
    {"sceNpScoreGetRankingByNpIdAsync",np_signed_out}, {"sceNpScoreGetRankingByRangeAsync",np_signed_out},
    {"sceNpScoreGetFriendsRankingAsync",np_signed_out}, {"sceNpScoreRecordGameDataAsync",np_signed_out},
    {"sceNpScoreGetGameDataAsync",np_signed_out}, {"sceNpScoreGetBoardInfoAsync",np_signed_out},
    {"sceNpScoreRecordScoreAsync",np_signed_out}, {"sceNpScoreGetRankingByAccountIdAsync",np_signed_out},
    {"sceNpScoreGetRankingByAccountId",np_signed_out}, {"sceNpScoreGetFriendsRankingA",np_signed_out},
    {"sceNpWordFilterCreateTitleCtxA",np_request_ctx}, {"sceNpWordFilterDeleteTitleCtx",ok_void},
    {"sceNpWordFilterCreateRequest",np_request}, {"sceNpWordFilterDeleteRequest",ok_void},
    {"sceNpWordFilterAbortRequest",ok_void}, {"sceNpWordFilterSanitizeComment",np_signed_out},
    {"sceNpBandwidthTestInitStart",np_signed_out}, {"sceNpBandwidthTestShutdown",ok_void},
    {"sceNpBandwidthTestAbort",ok_void},
    {"sceNpMatching2CreateContextA",np_signed_out},
    {"sceNpWebApiCreateContextA",webapi_create_context_a}, {"sceNpWebApiCreateHandle",np_request},
    {"sceNpWebApiDeleteHandle",ok_void},
    {"sceNpWebApiCheckTimeout",ok_void}, {"sceNpWebApiSetRequestTimeout",ok_void},
    {"sceNpWebApiAddHttpRequestHeader",webapi_add_header}, {"sceNpWebApiSendRequest2",webapi_send_request2},
    {"sceNpWebApiCreateExtdPushEventFilter",np_signed_out}, {"sceNpWebApiDeleteExtdPushEventFilter",ok_void},
    {"sceNpWebApiCreateServicePushEventFilter",np_signed_out}, {"sceNpWebApiDeleteServicePushEventFilter",ok_void},
    {"sceNpWebApiRegisterServicePushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterServicePushEventCallback",ok_void},
    {"sceNpSnsFacebookCreateRequest",np_request}, {"sceNpSnsFacebookDeleteRequest",ok_void},
    {"sceNpSnsFacebookGetAccessToken",np_signed_out},
    {"sceSigninDialogInitialize",ok_void}, {"sceSigninDialogTerminate",ok_void},
    {"sceNpSnsFacebookDialogInitialize",ok_void}, {"sceNpSnsFacebookDialogTerminate",ok_void},
    {"sceNpWebApiInitialize",webapi_initialize}, {"sceNpWebApiTerminate",webapi_terminate},
    {"sceNpWebApiCreateContext",webapi_create_context_online},
    {"sceNpWebApiCreateRequest",webapi_create_request}, {"sceNpWebApiSendRequest",webapi_send_request},
    {"sceNpWebApiDeleteRequest",webapi_delete_request}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",webapi_delete_context}, {"sceNpWebApiReadData",webapi_read_data},
    {"sceNpWebApiGetHttpStatusCode",webapi_status_code}, {"sceNpWebApiGetHttpResponseHeaderValue",webapi_header_value},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",webapi_header_length},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterRoomMessageCallback",ok_void}, {"sceNpMatching2GetRoomDataExternalList",np_signed_out},
    {"sceNpMatching2GetRoomMemberDataExternalList",np_signed_out},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_input}, {"sceAudioInClose",audio_in_close},
    {"sceAudioInGetSilentState",audio_in_silent_state},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init}, {"sceCommonDialogIsUsed",common_is_used},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogProgressBarSetValue",msg_progress},
    {"sceMsgDialogGetResult",msg_result}, {"sceMsgDialogGetStatus",msg_status},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress}, {"sceSaveDataDialogProgressBarInc",save_progress_inc},
    {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogGetStatus",error_status}, {"sceErrorDialogUpdateStatus",error_status},
    {"sceErrorDialogTerminate",error_term}, {"sceErrorDialogClose",error_close},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceNpCommerceDialogClose",commerce_close}, {"sceNpCommerceDialogGetResult",commerce_result},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_abort},
    {"sceImeDialogGetPanelSizeExtended",ime_panel_size_extended},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpen",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogClose",invitation_close},
    {"sceInvitationDialogTerminate",invitation_term}, {"sceInvitationDialogGetResult",invitation_result},
    {"sceImeKeyboardOpen",ime_keyboard_open}, {"sceImeKeyboardClose",ime_keyboard_close}, {"sceImeUpdate",ime_update},
    {"sceImeKeyboardGetResourceId",ime_keyboard_resource_ids}, {"sceImeKeyboardGetInfo",ime_keyboard_info},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state}, {"sceNpTrophyAbortHandle",trophy_abort_handle},
    {"sceNpTrophyDestroyHandle",trophy_destroy_handle}, {"sceNpTrophyDestroyContext",trophy_destroy_context},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetProgress",playgo_progress},
    {"scePlayGoSetToDoList",playgo_todo}, {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"scePlayGoGetLanguageMask",playgo_get_language_mask}, {"scePlayGoSetLanguageMask",playgo_set_language_mask},
    {"scePlayGoGetToDoList",playgo_get_todo}, {"scePlayGoGetEta",playgo_eta}, {"scePlayGoPrefetch",playgo_prefetch},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceDiscMap_7C980FFB0AA27E7A",discmap_7c98},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
uintptr_t runtime_services_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
