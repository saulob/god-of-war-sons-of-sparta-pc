/* libSceMove without a Move controller: initialization and opening succeed because the VR/input
 * layers need a Move handle that is distinct from other devices; every query reports no controller.
 * The VR peripheral libraries (camera, headset, tracker) live here too. */
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define MOVE_NO_CONTROLLER ((int32_t)1)
#define MOVE_NOT_INIT ((int32_t)0x80EE0001)
#define MOVE_ALREADY_INIT ((int32_t)0x80EE0002)
#define MOVE_INVALID_ARG ((int32_t)0x80EE0003)
#define MOVE_FIRST_HANDLE ((int32_t)0x30b0100)
#define NOTE_MOVE(api) runtime_history_note(api,"libSceMove",(uintptr_t)__builtin_return_address(0))

typedef struct { float sphere_radius; float accelerometer_offset[3]; } MoveDeviceInfo;
typedef struct { uint16_t button_data, trigger_data; } MoveButtonData;
typedef struct {
    uint16_t status, digital0, digital1;
    uint16_t analog_right_x, analog_right_y, analog_left_x, analog_left_y;
    uint8_t custom[5];
} MoveExtensionPortData;
typedef struct {
    float accelerometer[3], gyro[3];
    MoveButtonData button_data;
    MoveExtensionPortData extension_data;
    int64_t timestamp;
    int32_t count;
    float temperature;
} MoveData;
_Static_assert(sizeof(MoveDeviceInfo)==16,"move device info layout");
_Static_assert(sizeof(MoveExtensionPortData)==20 && sizeof(MoveData)==64,"move data layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static int32_t next_handle=MOVE_FIRST_HANDLE;
static unsigned handles_opened, reads;

static ABI int32_t move_init(void) {
    NOTE_MOVE("sceMoveInit");
    pthread_mutex_lock(&lock);
    const int already=initialized;
    if (!already) initialized=1;
    pthread_mutex_unlock(&lock);
    if (already) return MOVE_ALREADY_INIT;
    printf("Runtime: Move initialized (no controller present)\n");
    return 0;
}
static ABI int32_t move_open(int32_t user, int32_t type, int32_t index) {
    (void)user; (void)type; (void)index;
    NOTE_MOVE("sceMoveOpen");
    pthread_mutex_lock(&lock);
    if (!initialized) { pthread_mutex_unlock(&lock); return MOVE_NOT_INIT; }
    const int32_t handle=next_handle;
    next_handle+=0x100;
    if (!handles_opened) printf("Runtime: Move synthetic handle 0x%x opened (no controller present)\n",(unsigned)handle);
    ++handles_opened;
    pthread_mutex_unlock(&lock);
    return handle;
}
static ABI int32_t move_get_device_info(int32_t handle, MoveDeviceInfo *info) {
    (void)handle;
    if (!initialized) return MOVE_NOT_INIT;
    if (!info) return MOVE_INVALID_ARG;
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_read_latest(int32_t handle, MoveData *data) {
    (void)handle;
    if (!initialized) return MOVE_NOT_INIT;
    if (!data) return MOVE_INVALID_ARG;
    pthread_mutex_lock(&lock); ++reads; pthread_mutex_unlock(&lock);
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_read_recent(int32_t handle, int64_t timestamp, MoveData *data, int32_t *count) {
    (void)handle;
    if (!initialized) return MOVE_NOT_INIT;
    if (timestamp<0 || !data || !count) return MOVE_INVALID_ARG;
    pthread_mutex_lock(&lock); ++reads; pthread_mutex_unlock(&lock);
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_get_extension(int32_t handle, void *data) {
    (void)handle;
    if (!initialized) return MOVE_NOT_INIT;
    if (!data) return MOVE_INVALID_ARG;
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_set_vibration(int32_t handle, uint8_t intensity) {
    (void)handle; (void)intensity;
    return initialized ? MOVE_NO_CONTROLLER : MOVE_NOT_INIT;
}
static ABI int32_t move_set_light(int32_t handle, uint8_t red, uint8_t green, uint8_t blue) {
    (void)handle; (void)red; (void)green; (void)blue;
    return initialized ? MOVE_NO_CONTROLLER : MOVE_NOT_INIT;
}
static ABI int32_t move_reset_light(int32_t handle) {
    (void)handle;
    return initialized ? 0 : MOVE_NOT_INIT;
}
static ABI int32_t move_close(int32_t handle) {
    (void)handle;
    return initialized ? 0 : MOVE_NOT_INIT;
}
static ABI int32_t move_term(void) {
    NOTE_MOVE("sceMoveTerm");
    pthread_mutex_lock(&lock);
    if (!initialized) { pthread_mutex_unlock(&lock); return MOVE_NOT_INIT; }
    initialized=0;
    pthread_mutex_unlock(&lock);
    return 0;
}

/* libSceCamera, libSceHmd and libSceVrTracker without a PlayStation Camera or headset: the libraries
 * initialize, every device query reports that nothing is connected. */
#define CAMERA_ERROR_NOT_OPEN ((int32_t)0x802E0004)
#define CAMERA_ERROR_NOT_CONNECTED ((int32_t)0x802E0010)
#define CAMERA_ERROR_PARAM ((int32_t)0x802E0000)
#define HMD_ERROR_ALREADY_INITIALIZED ((int32_t)0x81110001)
#define HMD_ERROR_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_ERROR_HANDLE_INVALID ((int32_t)0x81110003)
#define HMD_ERROR_DEVICE_DISCONNECTED ((int32_t)0x81110004)
#define HMD_ERROR_PARAMETER_NULL ((int32_t)0x81110008)
#define HMD_ERROR_PARAMETER_INVALID ((int32_t)0x81110009)
#define HMD_STATUS_NOT_DETECTED 2
#define HMD_FIRST_HANDLE ((int32_t)0xf000000)
#define VR_ERROR_NOT_INIT ((int32_t)0x81260801)
#define VR_ERROR_ALREADY_INITIALIZED ((int32_t)0x81260802)
#define VR_ERROR_DEVICE_NOT_REGISTERED ((int32_t)0x81260803)
#define VR_ERROR_DEVICE_ALREADY_REGISTERED ((int32_t)0x81260804)
#define VR_ERROR_ARGUMENT_INVALID ((int32_t)0x81260806)
#define VR_ERROR_NOT_EXECUTE_GPU_SUBMIT ((int32_t)0x81260815)
#define VR_ERROR_NOT_EXECUTE_GPU_WAIT ((int32_t)0x81260816)
#define VR_MEMORY_ALIGNMENT 0x10000u
#define VR_ONION_SIZE 0x400000u
#define VR_GARLIC_SIZE 0x3000000u
#define VR_WORK_SIZE 0x1000000u
#define VR_DEVICE_HMD 0
#define VR_DEVICE_GUN 3
#define VR_DEVICE_SLOTS 4
#define VR_PROFILE_000 0
#define VR_PROFILE_100 100
#define VR_CALIBRATION_MANUAL 0
#define VR_CALIBRATION_AUTO 1

typedef struct { uint32_t hmd_position, pad_position, move_position, gun_position, reserved[4]; } VrCalibration;
typedef struct { uint32_t size, profile, reserved[6]; VrCalibration calibration; } VrQueryParam;
typedef struct {
    uint32_t size, onion_size, onion_alignment, garlic_size, garlic_alignment, work_size, work_alignment, reserved[9];
} VrQueryResult;
typedef struct {
    uint32_t size, profile, execution_mode;
    int32_t thread_priority[4], reserved;
    uint64_t cpu_mask;
    VrCalibration calibration;
    void *onion; uint32_t onion_size, onion_alignment;
    void *garlic; uint32_t garlic_size, garlic_alignment;
    void *work; uint32_t work_size, work_alignment;
    int32_t pipe_id, queue_id;
} VrInitParam;
typedef struct { uint32_t size; uint32_t device_type; } VrRecalibrateParam;
_Static_assert(sizeof(VrQueryParam)==64 && sizeof(VrQueryResult)==64 && sizeof(VrInitParam)==128,"vr tracker layout");

static pthread_mutex_t vr_lock=PTHREAD_MUTEX_INITIALIZER;
static int hmd_initialized, hmd_open_user=-1, hmd_handle;
static int vr_initialized;
static int32_t vr_handles[VR_DEVICE_SLOTS]={-1,-1,-1,-1};

static ABI int32_t camera_is_attached(int32_t index) { return index ? CAMERA_ERROR_PARAM : 0; }
static ABI int32_t camera_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)user; (void)param;
    return type || index ? CAMERA_ERROR_PARAM : CAMERA_ERROR_NOT_CONNECTED;
}
static ABI int32_t camera_not_open(int32_t handle) { return handle<1 ? CAMERA_ERROR_PARAM : CAMERA_ERROR_NOT_OPEN; }
static ABI int32_t camera_with_param(int32_t handle, const void *param) {
    return handle<1 || !param ? CAMERA_ERROR_PARAM : CAMERA_ERROR_NOT_OPEN;
}

static ABI int32_t hmd_initialize(const void *param) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=0;
    if (hmd_initialized) result=HMD_ERROR_ALREADY_INITIALIZED;
    else if (!param) result=HMD_ERROR_PARAMETER_NULL;
    else hmd_initialized=1;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t hmd_terminate(void) {
    pthread_mutex_lock(&vr_lock);
    const int32_t result=hmd_initialized ? 0 : HMD_ERROR_NOT_INITIALIZED;
    hmd_initialized=0; hmd_open_user=-1; hmd_handle=0;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t hmd_open(int32_t user, int32_t type, int32_t index, const void *param) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=HMD_ERROR_NOT_INITIALIZED;
    if (hmd_initialized) {
        if (type || index || param || user==-1 || user==0xff) result=HMD_ERROR_PARAMETER_INVALID;
        else if (hmd_handle) result=(int32_t)0x81110006;
        else { hmd_open_user=user; hmd_handle=HMD_FIRST_HANDLE; result=hmd_handle; }
    }
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t hmd_close(int32_t handle) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=HMD_ERROR_NOT_INITIALIZED;
    if (hmd_initialized) {
        result=handle==hmd_handle && hmd_handle ? 0 : HMD_ERROR_HANDLE_INVALID;
        if (!result) { hmd_handle=0; hmd_open_user=-1; }
    }
    pthread_mutex_unlock(&vr_lock);
    return result;
}
/* status, user id, reserved, panel size, latencies, hmu mount: 40 bytes. */
static ABI int32_t hmd_device_information(unsigned char *info) {
    if (!info) return HMD_ERROR_PARAMETER_NULL;
    if (!hmd_initialized) return HMD_ERROR_NOT_INITIALIZED;
    memset(info,0,40);
    const uint32_t status=HMD_STATUS_NOT_DETECTED;
    memcpy(info,&status,4);
    memcpy(info+4,&hmd_open_user,4);
    return 0;
}
static ABI int32_t hmd_handle_query(int32_t handle, void *out) {
    (void)handle; (void)out;
    return hmd_initialized ? HMD_ERROR_HANDLE_INVALID : HMD_ERROR_NOT_INITIALIZED;
}
static ABI int32_t hmd_reprojection_ok(void) { return 0; }
static ABI int32_t hmd_garlic_alignment(void) { return 0x100; }
static ABI int32_t hmd_garlic_size(void) { return 0x100000; }
static ABI int32_t hmd_onion_alignment(void) { return 0x100; }
static ABI int32_t hmd_onion_size(void) { return 0x810; }

static int vr_calibration_valid(const VrCalibration *c) {
    return c->pad_position<=VR_CALIBRATION_AUTO && c->hmd_position<=VR_CALIBRATION_MANUAL &&
           c->move_position<=VR_CALIBRATION_AUTO && c->gun_position<=VR_CALIBRATION_AUTO;
}
static uint32_t vr_onion_size(const VrCalibration *c) {
    return c->move_position==VR_CALIBRATION_AUTO ? VR_ONION_SIZE*2 : VR_ONION_SIZE;
}
static ABI int32_t vr_query_memory(const VrQueryParam *param, VrQueryResult *result) {
    if (!param || !result || param->size!=sizeof(*param) ||
        (param->profile!=VR_PROFILE_000 && param->profile!=VR_PROFILE_100) || !vr_calibration_valid(&param->calibration))
        return VR_ERROR_ARGUMENT_INVALID;
    result->onion_size=vr_onion_size(&param->calibration);
    result->onion_alignment=VR_MEMORY_ALIGNMENT;
    result->garlic_size=VR_GARLIC_SIZE;
    result->garlic_alignment=VR_MEMORY_ALIGNMENT;
    result->work_size=VR_WORK_SIZE;
    result->work_alignment=VR_MEMORY_ALIGNMENT;
    return 0;
}
static ABI int32_t vr_init(const VrInitParam *param) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=0;
    if (vr_initialized) result=VR_ERROR_ALREADY_INITIALIZED;
    else if (!param || param->size!=sizeof(*param) || !param->onion || !param->garlic || !param->work ||
             param->onion_alignment!=VR_MEMORY_ALIGNMENT || param->garlic_alignment!=VR_MEMORY_ALIGNMENT ||
             param->work_alignment!=VR_MEMORY_ALIGNMENT || param->garlic_size!=VR_GARLIC_SIZE ||
             param->work_size!=VR_WORK_SIZE || param->onion_size!=vr_onion_size(&param->calibration) ||
             !vr_calibration_valid(&param->calibration))
        result=VR_ERROR_ARGUMENT_INVALID;
    else vr_initialized=1;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t vr_term(void) {
    pthread_mutex_lock(&vr_lock);
    const int32_t result=vr_initialized ? 0 : VR_ERROR_NOT_INIT;
    vr_initialized=0;
    for (int i=0;i<VR_DEVICE_SLOTS;++i) vr_handles[i]=-1;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t vr_register_device(uint32_t type, int32_t handle) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=0;
    if (!vr_initialized) result=VR_ERROR_NOT_INIT;
    else if (type>VR_DEVICE_GUN) result=VR_ERROR_ARGUMENT_INVALID;
    else if (vr_handles[type]!=-1) result=VR_ERROR_DEVICE_ALREADY_REGISTERED;
    else vr_handles[type]=handle;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t vr_unregister_device(int32_t handle) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=vr_initialized ? VR_ERROR_ARGUMENT_INVALID : VR_ERROR_NOT_INIT;
    if (vr_initialized && handle>=0)
        for (int i=0;i<VR_DEVICE_SLOTS;++i) if (vr_handles[i]==handle) { vr_handles[i]=-1; result=0; break; }
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static ABI int32_t vr_recalibrate(const VrRecalibrateParam *param) {
    pthread_mutex_lock(&vr_lock);
    int32_t result=0;
    if (!vr_initialized) result=VR_ERROR_NOT_INIT;
    else if (!param || param->device_type>VR_DEVICE_GUN) result=VR_ERROR_ARGUMENT_INVALID;
    else if (param->device_type==VR_DEVICE_HMD || vr_handles[param->device_type]==-1) result=VR_ERROR_DEVICE_NOT_REGISTERED;
    pthread_mutex_unlock(&vr_lock);
    return result;
}
static uint64_t vr_time_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static ABI int32_t vr_get_time(uint64_t *time) {
    if (!vr_initialized) return VR_ERROR_NOT_INIT;
    if (!time) return VR_ERROR_ARGUMENT_INVALID;
    *time=vr_time_us();
    return 0;
}
static ABI int32_t vr_gpu_submit(const void *param) {
    (void)param;
    return vr_initialized ? VR_ERROR_ARGUMENT_INVALID : VR_ERROR_NOT_INIT;
}
static ABI int32_t vr_gpu_wait(const uint32_t *param) {
    if (!vr_initialized) return VR_ERROR_NOT_INIT;
    return !param || param[0]!=32 ? VR_ERROR_ARGUMENT_INVALID : VR_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}
static ABI int32_t vr_ok(void) { return 0; }

static const RuntimeExport exports[]={
    {"sceMoveInit",move_init}, {"sceMoveOpen",move_open},
    {"sceMoveGetDeviceInfo",move_get_device_info}, {"sceMoveReadStateLatest",move_read_latest},
    {"sceMoveReadStateRecent",move_read_recent}, {"sceMoveGetExtensionPortInfo",move_get_extension},
    {"sceMoveSetVibration",move_set_vibration}, {"sceMoveSetLightSphere",move_set_light},
    {"sceMoveResetLightSphere",move_reset_light}, {"sceMoveClose",move_close},
    {"sceMoveTerm",move_term},
    {"sceCameraIsAttached",camera_is_attached}, {"sceCameraOpen",camera_open}, {"sceCameraClose",camera_not_open},
    {"sceCameraSetConfig",camera_with_param}, {"sceCameraStart",camera_with_param},
    {"sceCameraSetVideoSync",camera_with_param}, {"sceCameraGetFrameData",camera_with_param},
    {"sceCameraIsValidFrameData",camera_with_param},
    {"sceHmdInitialize315",hmd_initialize}, {"sceHmdTerminate",hmd_terminate}, {"sceHmdOpen",hmd_open},
    {"sceHmdClose",hmd_close}, {"sceHmdGetDeviceInformation",hmd_device_information},
    {"sceHmdGetDeviceInformationByHandle",hmd_handle_query}, {"sceHmdGetFieldOfView",hmd_handle_query},
    {"sceHmdReprojectionQueryGarlicBuffAlign",hmd_garlic_alignment},
    {"sceHmdReprojectionQueryGarlicBuffSize",hmd_garlic_size},
    {"sceHmdReprojectionQueryOnionBuffAlign",hmd_onion_alignment},
    {"sceHmdReprojectionQueryOnionBuffSize",hmd_onion_size},
    {"sceHmdReprojectionInitialize",hmd_reprojection_ok}, {"sceHmdReprojectionFinalize",hmd_reprojection_ok},
    {"sceHmdReprojectionSetDisplayBuffers",hmd_reprojection_ok},
    {"sceHmdReprojectionUnsetDisplayBuffers",hmd_reprojection_ok},
    {"sceHmdReprojectionSetOutputMinColor",hmd_reprojection_ok},
    {"sceHmdReprojectionSetUserEventStart",hmd_reprojection_ok},
    {"sceHmdReprojectionSetUserEventEnd",hmd_reprojection_ok},
    {"sceHmdReprojectionClearUserEventStart",hmd_reprojection_ok},
    {"sceHmdReprojectionStart2dVr",hmd_reprojection_ok}, {"sceHmdReprojectionStartMultilayer",hmd_reprojection_ok},
    {"sceHmdReprojectionStop",hmd_reprojection_ok},
    {"sceVrTrackerQueryMemory",vr_query_memory}, {"sceVrTrackerInit",vr_init}, {"sceVrTrackerTerm",vr_term},
    {"sceVrTrackerRegisterDevice",vr_register_device}, {"sceVrTrackerUnregisterDevice",vr_unregister_device},
    {"sceVrTrackerRecalibrate",vr_recalibrate}, {"sceVrTrackerGetTime",vr_get_time},
    {"sceVrTrackerGpuSubmit",vr_gpu_submit}, {"sceVrTrackerGpuWait",vr_gpu_wait},
    {"sceVrTrackerCpuProcess",vr_ok}, {"sceVrTrackerGetPlayAreaWarningInfo",vr_ok},
    {"sceVrTrackerGetResult",vr_ok}, {"sceVrTrackerNotifyEndOfCpuProcess",vr_ok},
    {"sceVrTrackerResetOrientationRelative",vr_ok}, {"sceVrTrackerUpdateMotionSensorData",vr_ok},
};
uintptr_t runtime_move_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_move_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: Move initialized=%s, handles opened=%u, controllers=0, reads=%u\n",
           initialized ? "yes" : "no",handles_opened,reads);
    pthread_mutex_unlock(&lock);
}
