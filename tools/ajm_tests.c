/* libSceAjm through its exports, with batches built by the same builder exports the game uses: contexts, modules,
 * instances, batch slots, malformed batches and error codes; with AJM_TEST_AT9 naming a fixture cut by
 * scripts/at9_fixture.py, also ATRAC9 decoding (initialisation, formats, split buffers, multiple frames, gapless,
 * reset, RIFF). Unsupported codecs and bad input must come back as results, never end the process. */
#include <stdarg.h>
#include "runtime_ajm.c"

int runtime_log_printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

static size_t pending_batches(void) {
    size_t n = 0;
    for (int i = 0; i < MAX_BATCHES; ++i) n += batches[i].used != 0;
    return n;
}
static uint32_t reset_batch(unsigned char *buffer, uint32_t instance, SidebandResult *result) {
    unsigned char *end = job_control(buffer, instance, UINT64_C(1) << 13, NULL, 0, result, sizeof(*result), NULL);
    return (uint32_t)(end - buffer);
}

static void test_context_and_instances(uint32_t *ctx, uint32_t *instance) {
    uint32_t id = 0;
    CHECK(ajm_initialize(1, &id) == ERR_INVALID_PARAMETER);
    CHECK(ajm_initialize(0, NULL) == ERR_INVALID_PARAMETER);
    CHECK(ajm_initialize(0, &id) == 0 && id >= 1);
    CHECK(ajm_instance_create(id, CODEC_AT9, 1, instance) == ERR_NOT_REGISTERED);
    CHECK(ajm_module_register(id, CODEC_AT9, 0) == 0);
    CHECK(ajm_module_register(id, CODEC_AT9, 0) == ERR_ALREADY_REGISTERED);
    CHECK(ajm_module_register(id, 24, 0) == ERR_INVALID_PARAMETER);
    CHECK(ajm_instance_create(id, CODEC_AT9, 0, instance) == ERR_WRONG_REVISION);
    CHECK(ajm_instance_create(id, CODEC_AT9, 1 | (7 << 7), instance) == ERR_INVALID_PARAMETER);
    CHECK(ajm_instance_create(id, CODEC_AT9, 1 | (FORMAT_FLOAT << 7) | (2 << 3), instance) == 0);
    CHECK(ajm_instance_codec(*instance) == CODEC_AT9);
    CHECK(ajm_instance_create(id + 1, CODEC_AT9, 1, instance + 1) == ERR_INVALID_CONTEXT);
    *ctx = id;
}

static void test_batch_lifecycle(uint32_t ctx, uint32_t instance) {
    unsigned char buffer[256];
    SidebandResult result = {-1, -1};
    uint32_t size = reset_batch(buffer, instance, &result);
    BatchError error;
    uint32_t batch = 0;
    size_t jobs_before = jobs_run, pending_before = pending_batches();
    CHECK(ajm_batch_start(ctx, buffer, size, 0, &error, &batch) == 0);
    CHECK(batch >= 1 && batch <= MAX_BATCHES);
    CHECK(jobs_run == jobs_before + 1);
    CHECK(result.result == 0);
    CHECK(pending_batches() == pending_before + 1);
    CHECK(ajm_batch_cancel(ctx, batch) == 0);
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == 0);
    CHECK(pending_batches() == pending_before);
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == ERR_INVALID_BATCH);
    CHECK(ajm_batch_cancel(ctx, batch) == ERR_INVALID_BATCH);
    CHECK(ajm_batch_wait(ctx, 0, 0, &error) == ERR_INVALID_BATCH);
    CHECK(ajm_batch_wait(ctx, MAX_BATCHES + 1, 0, &error) == ERR_INVALID_BATCH);
    CHECK(ajm_batch_wait(ctx + 1, 1, 0, &error) == ERR_INVALID_CONTEXT);
    const uint64_t note = 0x1122334455667788;
    unsigned char *end = job_inline(buffer, &note, sizeof(note), NULL);
    end = job_control(end, instance, UINT64_C(1) << 13, NULL, 0, &result, sizeof(result), NULL);
    CHECK(ajm_batch_start(ctx, buffer, (uint32_t)(end - buffer), 0, &error, &batch) == 0);
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == 0);
}

static void test_slot_exhaustion(uint32_t ctx, uint32_t instance) {
    unsigned char buffer[256];
    SidebandResult result;
    uint32_t size = reset_batch(buffer, instance, &result);
    BatchError error;
    uint32_t ids[MAX_BATCHES];
    int started = 1;
    for (int i = 0; i < MAX_BATCHES; ++i) if (ajm_batch_start(ctx, buffer, size, 0, &error, &ids[i])) started = 0;
    CHECK(started);
    CHECK(pending_batches() == MAX_BATCHES);
    uint32_t extra = 0;
    CHECK(ajm_batch_start(ctx, buffer, size, 0, &error, &extra) == ERR_OUT_OF_RESOURCES);
    int waited = 1;
    for (int i = 0; i < MAX_BATCHES; ++i) if (ajm_batch_wait(ctx, ids[i], 0, &error)) waited = 0;
    CHECK(waited);
    CHECK(pending_batches() == 0);
    CHECK(ajm_batch_start(ctx, buffer, size, 0, &error, &extra) == 0);
    CHECK(ajm_batch_wait(ctx, extra, 0, &error) == 0);
}

static void test_malformed_batches(uint32_t ctx, uint32_t instance) {
    unsigned char buffer[256];
    SidebandResult result;
    uint32_t size = reset_batch(buffer, instance, &result);
    BatchError error;
    uint32_t batch = 0;
    size_t pending_before = pending_batches();
    CHECK(ajm_batch_start(ctx, buffer, size - 4, 0, &error, &batch) == ERR_MALFORMED_BATCH);
    Chunk bogus = {header(IDENT_JOB, instance), 8};
    uint64_t junk = UINT64_C(0x3f);
    memcpy(buffer, &bogus, sizeof(bogus));
    memcpy(buffer + sizeof(bogus), &junk, sizeof(junk));
    memset(&error, 0, sizeof(error));
    CHECK(ajm_batch_start(ctx, buffer, 16, 0, &error, &batch) == ERR_MALFORMED_BATCH);
    CHECK(error.error_code == ERR_MALFORMED_BATCH && error.command_offset == 0);
    Chunk overrun = {header(IDENT_JOB, instance), 4096};
    memcpy(buffer, &overrun, sizeof(overrun));
    CHECK(ajm_batch_start(ctx, buffer, 16, 0, &error, &batch) == ERR_MALFORMED_BATCH);
    CHECK(ajm_batch_start(ctx, buffer, 8, 0, &error, &batch) == ERR_MALFORMED_BATCH);
    CHECK(pending_batches() == pending_before);
    CHECK(ajm_batch_start(ctx + 1, buffer, 16, 0, &error, &batch) == ERR_INVALID_CONTEXT);
}

static void test_destroyed_instance(uint32_t ctx, uint32_t instance) {
    CHECK(ajm_instance_destroy(ctx, instance) == 0);
    CHECK(ajm_instance_destroy(ctx, instance) == ERR_INVALID_INSTANCE);
    unsigned char buffer[256];
    SidebandResult result = {0, 0};
    uint32_t size = reset_batch(buffer, instance, &result);
    BatchError error;
    uint32_t batch = 0;
    CHECK(ajm_batch_start(ctx, buffer, size, 0, &error, &batch) == 0);
    CHECK((uint32_t)result.result == (RESULT_FATAL | RESULT_INVALID_PARAMETER));
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == 0);
}

static void test_str_error(void) {
    CHECK(!strcmp(ajm_str_error(ERR_INVALID_CONTEXT), "SCE_AJM_ERROR_INVALID_CONTEXT"));
    CHECK(!strcmp(ajm_str_error(ERR_ALREADY_REGISTERED), "SCE_AJM_ERROR_CODEC_ALREADY_REGISTERED"));
    CHECK(!strcmp(ajm_str_error(ERR_CANCELLED), "SCE_AJM_ERROR_CANCELLED"));
    CHECK(!strcmp(ajm_str_error(0x12345678), "SCE_AJM_ERROR_UNKNOWN"));
}

static void test_finalize(uint32_t ctx) {
    uint32_t instance = 0;
    CHECK(ajm_instance_create(ctx, CODEC_AT9, 1, &instance) == 0);
    CHECK(ajm_module_unregister(ctx, CODEC_AT9) == 0);
    CHECK(ajm_module_unregister(ctx, CODEC_AT9) == ERR_NOT_REGISTERED);
    CHECK(ajm_finalize(ctx) == 0);
    CHECK(ajm_finalize(ctx) == ERR_INVALID_CONTEXT);
    uint32_t again = 0;
    CHECK(ajm_initialize(0, &again) == 0 && again == ctx);
    CHECK(ajm_finalize(again) == 0);
}

/* ---- other codecs and bad input ---- */
enum { SIDE_STREAM = 1, SIDE_FORMAT = 2, SIDE_GAPLESS = 4, RUN_INFO = 8, RUN_MULTI = 16 };
static uint64_t run_flags(int bits) {
    return 1 | ((bits & RUN_INFO) ? UINT64_C(1) << 11 : 0) | ((bits & RUN_MULTI) ? UINT64_C(1) << 12 : 0) |
           ((bits & SIDE_GAPLESS) ? UINT64_C(1) << 45 : 0) | ((bits & SIDE_FORMAT) ? UINT64_C(1) << 46 : 0) |
           ((bits & SIDE_STREAM) ? UINT64_C(1) << 47 : 0);
}
typedef struct { SidebandResult result; SidebandStream stream; SidebandFormat format; SidebandGapless gapless; uint32_t frames[2]; At9Info info; } Side;
static uint32_t run_job_batch(uint32_t ctx, uint32_t instance, int bits, const AjmBuffer *in, int in_count,
                              const AjmBuffer *out, int out_count, Side *side) {
    static unsigned char buffer[1024];
    unsigned char raw[128];
    memset(raw, 0, sizeof(raw));
    unsigned char *end = job_run_split(buffer, instance, run_flags(bits), in, (uint64_t)in_count, out, (uint64_t)out_count, raw, sizeof(raw), NULL);
    BatchError error;
    uint32_t batch = 0;
    int32_t r = ajm_batch_start(ctx, buffer, (uint32_t)(end - buffer), 0, &error, &batch);
    CHECK(r == 0);
    if (r) return 0xffffffffu;
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == 0);
    size_t at = 0;
    memset(side, 0, sizeof(*side));
    memcpy(&side->result, raw, 8); at = 8;
    if (bits & SIDE_STREAM) { memcpy(&side->stream, raw + at, 16); at += 16; }
    if (bits & SIDE_FORMAT) { memcpy(&side->format, raw + at, 24); at += 24; }
    if (bits & SIDE_GAPLESS) { memcpy(&side->gapless, raw + at, 8); at += 8; }
    if (bits & RUN_MULTI) { memcpy(side->frames, raw + at, 8); at += 8; }
    if (bits & RUN_INFO) memcpy(&side->info, raw + at, 16);
    return (uint32_t)side->result.result;
}
static uint32_t control_batch(uint32_t ctx, uint32_t instance, uint64_t flags, const void *control, uint64_t control_size) {
    unsigned char buffer[256];
    SidebandResult result = {-1, -1};
    unsigned char *end = job_control(buffer, instance, flags, (void *)control, control_size, &result, sizeof(result), NULL);
    BatchError error;
    uint32_t batch = 0;
    CHECK(ajm_batch_start(ctx, buffer, (uint32_t)(end - buffer), 0, &error, &batch) == 0);
    CHECK(ajm_batch_wait(ctx, batch, 0, &error) == 0);
    return (uint32_t)result.result;
}

static void test_other_codecs_do_not_stop(void) {
    uint32_t ctx = 0, instance = 0;
    CHECK(ajm_initialize(0, &ctx) == 0);
    for (uint32_t codec = 0; codec < 3; codec += 2) {
        CHECK(ajm_module_register(ctx, codec, 0) == 0);
        CHECK(ajm_instance_create(ctx, codec, 1, &instance) == 0 && ajm_instance_codec(instance) == codec);
        unsigned char data[64] = {1, 2, 3}, pcm[256];
        AjmBuffer in = {data, sizeof(data)}, out = {pcm, sizeof(pcm)};
        Side side;
        uint32_t result = run_job_batch(ctx, instance, SIDE_STREAM, &in, 1, &out, 1, &side);
        CHECK(result == (RESULT_CODEC_ERROR | RESULT_FATAL));
        CHECK(side.stream.input_consumed == 0 && side.stream.output_written == 0);
        CHECK(ajm_instance_destroy(ctx, instance) == 0);
    }
    CHECK(ajm_finalize(ctx) == 0);
}

/* ---- ATRAC9 ---- */
static unsigned char *fixture;
static size_t fixture_size;
static Atrac9CodecInfo fixture_info;

static int load_fixture(void) {
    const char *path = getenv("AJM_TEST_AT9");
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    fixture = malloc((size_t)size);
    fixture_size = fixture && fread(fixture, 1, (size_t)size, file) == (size_t)size ? (size_t)size : 0;
    fclose(file);
    if (fixture_size < 4096) return 0;
    void *handle = Atrac9GetHandle();
    int ok = handle && Atrac9InitDecoder(handle, fixture) == 0 && Atrac9GetCodecInfo(handle, &fixture_info) == 0;
    if (handle) Atrac9ReleaseHandle(handle);
    return ok;
}

typedef struct { uint32_t ctx, instance; } At9;
static At9 at9_create(uint32_t ctx, int format, uint32_t codec_flags) {
    At9 a = {ctx, 0};
    CHECK(ajm_instance_create(ctx, CODEC_AT9, 1 | ((uint64_t)format << 7) | ((uint64_t)codec_flags << 32), &a.instance) == 0);
    CHECK(control_batch(ctx, a.instance, UINT64_C(1) << 14, fixture, 4) == 0);
    return a;
}
static size_t frame_bytes(int format) {
    return (size_t)fixture_info.frameSamples * (size_t)fixture_info.channels * (size_t)pcm_size(format);
}
static size_t superframe_pcm_bytes(int format) { return frame_bytes(format) * (size_t)fixture_info.framesInSuperframe; }
static double energy16(const int16_t *pcm, size_t count) {
    double sum = 0;
    for (size_t i = 0; i < count; ++i) sum += (double)pcm[i] * pcm[i];
    return sum;
}

static void test_at9_decode(uint32_t ctx) {
    const size_t sf = (size_t)fixture_info.superframeSize;
    const int frames_per_sf = fixture_info.framesInSuperframe;
    const size_t pcm16_sf = superframe_pcm_bytes(FORMAT_S16);
    CHECK(sf > 0 && frames_per_sf > 0 && fixture_size >= sf * 4);
    unsigned char *pcm = calloc(1, pcm16_sf * 8), *other = calloc(1, pcm16_sf * 8);
    Side side;

    At9 a = at9_create(ctx, FORMAT_S16, 0);
    AjmBuffer in = {fixture + 4, sf}, out = {pcm, pcm16_sf};
    uint32_t result = run_job_batch(ctx, a.instance, SIDE_STREAM | SIDE_FORMAT | SIDE_GAPLESS | RUN_INFO | RUN_MULTI, &in, 1, &out, 1, &side);
    CHECK(result == 0);
    CHECK(side.frames[0] == (uint32_t)frames_per_sf);
    CHECK(side.stream.input_consumed == (int32_t)sf && side.stream.output_written == (int32_t)pcm16_sf);
    CHECK(side.stream.total_decoded_samples == (uint64_t)fixture_info.frameSamples * (uint64_t)frames_per_sf);
    CHECK(side.format.channels == (uint32_t)fixture_info.channels && side.format.sample_rate == (uint32_t)fixture_info.samplingRate);
    CHECK(side.format.encoding == FORMAT_S16 && side.format.channel_mask == channel_mask(fixture_info.channels) && side.format.bitrate > 0);
    CHECK(side.info.super_frame_size == sf && side.info.frames_in_super_frame == (uint32_t)frames_per_sf);
    CHECK(side.info.frame_samples == (uint32_t)fixture_info.frameSamples && side.info.next_frame_size == sf);
    CHECK(energy16((int16_t *)pcm, pcm16_sf / 2) > 0);

    /* The same superframe split over three input chunks and two output chunks decodes identically. */
    At9 b = at9_create(ctx, FORMAT_S16, 0);
    AjmBuffer split_in[3] = {{fixture + 4, 7}, {fixture + 4 + 7, sf / 2}, {fixture + 4 + 7 + sf / 2, sf - 7 - sf / 2}};
    AjmBuffer split_out[2] = {{other, 10}, {other + 10, pcm16_sf - 10}};
    result = run_job_batch(ctx, b.instance, SIDE_STREAM | RUN_MULTI, split_in, 3, split_out, 2, &side);
    CHECK(result == 0 && side.stream.input_consumed == (int32_t)sf && !memcmp(pcm, other, pcm16_sf));

    /* Single frame mode decodes one frame per job, and the superframe continues across jobs. */
    At9 c = at9_create(ctx, FORMAT_S16, 0);
    size_t consumed = 0;
    int ok = 1;
    for (int i = 0; i < frames_per_sf; ++i) {
        AjmBuffer one_in = {fixture + 4 + consumed, sf - consumed}, one_out = {other + (size_t)i * frame_bytes(FORMAT_S16), frame_bytes(FORMAT_S16)};
        result = run_job_batch(ctx, c.instance, SIDE_STREAM | RUN_INFO, &one_in, 1, &one_out, 1, &side);
        ok &= result == 0 && side.stream.output_written == (int32_t)frame_bytes(FORMAT_S16);
        consumed += (size_t)side.stream.input_consumed;
    }
    CHECK(ok && consumed == sf && !memcmp(pcm, other, pcm16_sf) && side.info.next_frame_size == sf);

    /* Partial input and too little room are reported, nothing is consumed. */
    AjmBuffer half = {fixture + 4, sf / 2};
    result = run_job_batch(ctx, c.instance, SIDE_STREAM, &half, 1, &out, 1, &side);
    CHECK((result & RESULT_PARTIAL_INPUT) && side.stream.input_consumed == 0);
    AjmBuffer tiny = {pcm, 8};
    result = run_job_batch(ctx, c.instance, SIDE_STREAM, &in, 1, &tiny, 1, &side);
    CHECK((result & RESULT_NOT_ENOUGH_ROOM) && side.stream.input_consumed == 0 && side.stream.output_written == 0);

    /* Reset restarts the decoder: the same superframe decodes to the same PCM. */
    CHECK(control_batch(ctx, b.instance, UINT64_C(1) << 13, NULL, 0) == 0);
    memset(other, 0, pcm16_sf);
    AjmBuffer again_out = {other, pcm16_sf};
    result = run_job_batch(ctx, b.instance, RUN_MULTI, &in, 1, &again_out, 1, &side);
    CHECK(result == 0 && !memcmp(pcm, other, pcm16_sf));

    /* Output encodings and layouts. */
    At9 s32 = at9_create(ctx, FORMAT_S32, 0), f32 = at9_create(ctx, FORMAT_FLOAT, 0), planar = at9_create(ctx, FORMAT_S16, 1u << 8);
    unsigned char *wide = calloc(1, pcm16_sf * 2), *planes = calloc(1, pcm16_sf);
    AjmBuffer wide_out = {wide, pcm16_sf * 2}, plane_out = {planes, pcm16_sf};
    result = run_job_batch(ctx, s32.instance, RUN_MULTI, &in, 1, &wide_out, 1, &side);
    const size_t count = pcm16_sf / 2;
    int s32_ok = result == 0;
    for (size_t i = 0; i < count && s32_ok; ++i) { int32_t w; memcpy(&w, wide + i * 4, 4); s32_ok = w == (int32_t)((int16_t *)pcm)[i] * 65536; }
    CHECK(s32_ok);
    memset(wide, 0, pcm16_sf * 2);
    result = run_job_batch(ctx, f32.instance, RUN_MULTI, &in, 1, &wide_out, 1, &side);
    int f32_ok = result == 0;
    for (size_t i = 0; i < count && f32_ok; ++i) { float f; memcpy(&f, wide + i * 4, 4); f32_ok = f == (float)((int16_t *)pcm)[i] / 32768.0f; }
    CHECK(f32_ok);
    result = run_job_batch(ctx, planar.instance, RUN_MULTI, &in, 1, &plane_out, 1, &side);
    int planar_ok = result == 0;
    const size_t samples = (size_t)fixture_info.frameSamples;
    for (int f = 0; f < frames_per_sf && planar_ok; ++f) for (int c2 = 0; c2 < fixture_info.channels && planar_ok; ++c2) for (size_t s = 0; s < samples && planar_ok; ++s) {
        int16_t expect = ((int16_t *)pcm)[(size_t)f * samples * (size_t)fixture_info.channels + s * (size_t)fixture_info.channels + (size_t)c2], got;
        memcpy(&got, planes + (size_t)f * frame_bytes(FORMAT_S16) + ((size_t)c2 * samples + s) * 2, 2);
        planar_ok = got == expect;
    }
    CHECK(planar_ok);

    /* Gapless: encoder delay is skipped and the total is honoured, with the rest of the superframe drained. */
    At9 g = at9_create(ctx, FORMAT_S16, 0);
    const uint32_t total = (uint32_t)samples * 2 + 10, skip = 5;
    SidebandGapless gapless = {total, skip, 0};
    CHECK(control_batch(ctx, g.instance, UINT64_C(1) << 45, &gapless, sizeof(gapless)) == 0);
    memset(other, 0, pcm16_sf * 2);
    AjmBuffer gapless_in = {fixture + 4, sf * 2}, gapless_out = {other, pcm16_sf * 2};
    result = run_job_batch(ctx, g.instance, SIDE_STREAM | SIDE_GAPLESS | RUN_MULTI, &gapless_in, 1, &gapless_out, 1, &side);
    CHECK(result == 0 && side.stream.total_decoded_samples == total);
    CHECK(side.stream.output_written == (int32_t)(total * (uint32_t)fixture_info.channels * 2));
    CHECK(side.gapless.total_samples == 0 && side.gapless.skip_samples == 0);
    CHECK(!memcmp(other, pcm + (size_t)skip * (size_t)fixture_info.channels * 2, (size_t)(samples - skip) * (size_t)fixture_info.channels * 2));
    CHECK(side.stream.input_consumed > 0 && side.stream.input_consumed % (int32_t)sf == 0);

    /* The 640-byte shape of the game's first job: two superframes' worth of input in one run job. */
    At9 fmod = at9_create(ctx, FORMAT_S16, 0);
    unsigned char *fmod_out = calloc(1, pcm16_sf * 4);
    AjmBuffer fmod_in = {fixture + 4, 640 < fixture_size - 4 ? 640 : fixture_size - 4}, fmod_buffer = {fmod_out, pcm16_sf * 4};
    result = run_job_batch(ctx, fmod.instance, SIDE_STREAM | RUN_MULTI, &fmod_in, 1, &fmod_buffer, 1, &side);
    CHECK(side.stream.input_consumed > 0 && side.stream.output_written > 0 && !(result & RESULT_FATAL));
    CHECK(energy16((int16_t *)fmod_out, (size_t)side.stream.output_written / 2) > 0);

    free(wide); free(planes); free(fmod_out); free(pcm); free(other);
    CHECK(ajm_instance_destroy(ctx, a.instance) == 0 && ajm_instance_destroy(ctx, b.instance) == 0);
}

static unsigned char *build_riff(size_t *size, int valid_guid) {
    const size_t data = (size_t)fixture_info.superframeSize;
    unsigned char *r = calloc(1, 12 + 8 + 52 + 8 + 12 + 8 + data);
    unsigned char *p = r;
    memcpy(p, "RIFF", 4); uint32_t total = (uint32_t)(4 + 8 + 52 + 8 + 12 + 8 + data); memcpy(p + 4, &total, 4); memcpy(p + 8, "WAVE", 4); p += 12;
    memcpy(p, "fmt ", 4); uint32_t length = 52; memcpy(p + 4, &length, 4); p += 8;
    uint16_t tag = 0xFFFE; memcpy(p, &tag, 2);
    memcpy(p + 24, at9_guid, 16); memcpy(p + 44, fixture, 4);
    if (!valid_guid) p[24] ^= 0xff;
    p += 52;
    memcpy(p, "fact", 4); length = 12; memcpy(p + 4, &length, 4);
    uint32_t samples = (uint32_t)fixture_info.frameSamples * (uint32_t)fixture_info.framesInSuperframe - 100, delay = 32; memcpy(p + 8, &samples, 4); memcpy(p + 12, &delay, 4);
    p += 20;
    memcpy(p, "data", 4); length = (uint32_t)data; memcpy(p + 4, &length, 4); memcpy(p + 8, fixture + 4, data);
    *size = (size_t)(p + 8 + data - r);
    return r;
}

static void test_at9_riff_and_errors(uint32_t ctx) {
    Side side;
    const size_t pcm16_sf = superframe_pcm_bytes(FORMAT_S16);
    unsigned char *pcm = calloc(1, pcm16_sf * 2);
    AjmBuffer out = {pcm, pcm16_sf * 2};
    size_t size = 0;
    unsigned char *riff = build_riff(&size, 1);
    uint32_t instance = 0;
    CHECK(ajm_instance_create(ctx, CODEC_AT9, 1 | 1ull << 32, &instance) == 0);
    AjmBuffer in = {riff, size};
    uint32_t result = run_job_batch(ctx, instance, SIDE_STREAM | SIDE_GAPLESS | RUN_MULTI, &in, 1, &out, 1, &side);
    CHECK(result == 0 && side.stream.input_consumed == (int32_t)size);
    CHECK(side.stream.total_decoded_samples == (uint64_t)fixture_info.frameSamples * (uint64_t)fixture_info.framesInSuperframe - 100);

    for (size_t cut = 1; cut < 80; cut += 7) {
        in.size = cut;
        run_job_batch(ctx, instance, RUN_MULTI, &in, 1, &out, 1, &side);
    }
    free(riff);
    riff = build_riff(&size, 0);
    in = (AjmBuffer){riff, size};
    result = run_job_batch(ctx, instance, RUN_MULTI, &in, 1, &out, 1, &side);
    CHECK(result == (RESULT_CODEC_ERROR | RESULT_FATAL));
    free(riff);

    /* Raw decoding without initialisation, a bad configuration and corrupt data are results, not stops. */
    CHECK(ajm_instance_create(ctx, CODEC_AT9, 1, &instance) == 0);
    in = (AjmBuffer){fixture + 4, (uint64_t)fixture_info.superframeSize};
    result = run_job_batch(ctx, instance, RUN_MULTI, &in, 1, &out, 1, &side);
    CHECK(result & RESULT_NOT_INITIALIZED);
    const unsigned char zero_config[4] = {0, 0, 0, 0};
    CHECK(control_batch(ctx, instance, UINT64_C(1) << 14, zero_config, 4) & RESULT_FATAL);
    CHECK(control_batch(ctx, instance, UINT64_C(1) << 14, fixture, 4) == 0);
    unsigned char *garbage = malloc((size_t)fixture_info.superframeSize);
    memset(garbage, 0xA5, (size_t)fixture_info.superframeSize);
    in = (AjmBuffer){garbage, (uint64_t)fixture_info.superframeSize};
    run_job_batch(ctx, instance, RUN_MULTI, &in, 1, &out, 1, &side);
    CHECK(side.stream.output_written >= 0);
    free(garbage);
    free(pcm);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(runtime_ajm_resolve("sceAjmBatchStartBuffer") == (uintptr_t)ajm_batch_start);
    CHECK(runtime_ajm_resolve("sceAjmBatchWait") == (uintptr_t)ajm_batch_wait);
    CHECK(runtime_ajm_resolve("sceAjmBatchJobRunSplitBufferRa") == (uintptr_t)job_run_split);
    uint32_t ctx = 0, instance[2] = {0, 0};
    test_context_and_instances(&ctx, instance);
    test_batch_lifecycle(ctx, instance[0]);
    test_slot_exhaustion(ctx, instance[0]);
    test_malformed_batches(ctx, instance[0]);
    test_destroyed_instance(ctx, instance[0]);
    test_str_error();
    test_finalize(ctx);
    test_other_codecs_do_not_stop();
    if (load_fixture()) {
        uint32_t at9_ctx = 0;
        CHECK(ajm_initialize(0, &at9_ctx) == 0 && ajm_module_register(at9_ctx, CODEC_AT9, 0) == 0);
        test_at9_decode(at9_ctx);
        test_at9_riff_and_errors(at9_ctx);
        CHECK(ajm_finalize(at9_ctx) == 0);
        CHECK(frames_decoded > 0);
    } else {
        printf("not covered: ATRAC9 decoding (set AJM_TEST_AT9, see scripts/at9_fixture.py)\n");
    }
    runtime_ajm_report();
    printf("ajm: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
