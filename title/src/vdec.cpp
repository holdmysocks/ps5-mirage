/*
 * Mirage - title: hardware video decode through libSceVideodec2.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The structure layouts, constants and call order are those of
 * blackbearreloaded/ProsperoLight (src/moonlight_stream.cpp), as written up
 * and verified on hardware by sainsaji/EVO-PLAYER-PS5
 * (projects/evoplayer/media/include/sce/sce_videodec2.h and
 * docs/evo-pro/videodec2-abi.md), both GPL-3.0-or-later. The decoder only
 * works from a title: from a loader payload the decode call fails.
 *
 * One thread owns the decoder. It brings it up once (H.264 High, up to
 * 1920x1088), then decodes the access units vdec_submit queues. Each picture
 * is copied out of the decoder's frame slot at once (the slot is reused
 * three units later) into ordinary memory, where the screen code reads it.
 */
#include "vdec.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>

extern "C"
{
    int sceKernelUsleep(std::uint32_t microseconds);
    int sceSysmoduleLoadModule(std::uint32_t id);
    std::int64_t sceKernelGetDirectMemorySize(void);
    std::int32_t sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                               std::size_t length, std::size_t alignment, int memory_type,
                                               std::int64_t *direct_memory_start);
    std::int32_t sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                          std::int64_t direct_memory_start, std::size_t alignment);
    std::int32_t sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int protection, int flags,
                                                 const char *name);

    struct SceVideodec2DecoderConfigInfo
    {
        std::uint64_t size;
        std::uint32_t resource_type;
        std::uint32_t codec_type;
        std::uint32_t profile;
        std::uint32_t max_level;
        std::int32_t max_width;
        std::int32_t max_height;
        std::int32_t max_dpb_frames;
        std::uint32_t pipeline_depth;
        std::uint64_t compute_queue;
        std::uint64_t cpu_affinity;
        std::int32_t cpu_priority;
        std::uint32_t optimize_progressive;
        std::uint32_t check_memory_type;
        std::uint32_t reserved;
    };
    struct SceVideodec2DecoderMemoryInfo
    {
        std::uint64_t size;
        std::uint64_t cpu_size;
        void *cpu;
        std::uint64_t gpu_size;
        void *gpu;
        std::uint64_t cpu_gpu_size;
        void *cpu_gpu;
        std::uint64_t max_frame_size;
        std::uint32_t frame_alignment;
        std::uint32_t reserved;
    };
    struct SceVideodec2ComputeConfigInfo
    {
        std::uint64_t size;
        std::uint16_t pipe_id;
        std::uint16_t queue_id;
        std::uint8_t check_memory_type;
        std::uint8_t reserved0;
        std::uint16_t reserved1;
    };
    struct SceVideodec2ComputeMemoryInfo
    {
        std::uint64_t size;
        std::uint64_t cpu_gpu_size;
        void *cpu_gpu;
    };
    struct SceVideodec2InputData
    {
        std::uint64_t size;
        void *au;
        std::uint64_t au_size;
        std::uint64_t pts;
        std::uint64_t dts;
        std::uint64_t attached;
    };
    struct SceVideodec2FrameBuffer
    {
        std::uint64_t size;
        void *buffer;
        std::uint64_t buffer_size;
        std::uint32_t accepted;
        std::uint32_t reserved;
    };
    struct SceVideodec2OutputInfo
    {
        std::uint64_t size;
        std::uint8_t valid;
        std::uint8_t error;
        std::uint8_t picture_count;
        std::uint8_t padding;
        std::uint32_t codec;
        std::uint32_t width;
        std::uint32_t pitch;
        std::uint32_t height;
        std::uint32_t reserved;
        void *buffer;
        std::uint64_t buffer_size;
        std::uint32_t frame_format;
        std::uint32_t pitch_bytes;
    };

    std::int32_t sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo *memory);
    std::int32_t sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeConfigInfo *config,
                                                  const SceVideodec2ComputeMemoryInfo *memory, void **queue);
    std::int32_t sceVideodec2QueryDecoderMemoryInfo(const SceVideodec2DecoderConfigInfo *config,
                                                    SceVideodec2DecoderMemoryInfo *memory);
    std::int32_t sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo *config,
                                           const SceVideodec2DecoderMemoryInfo *memory, void **decoder);
    std::int32_t sceVideodec2DeleteDecoder(void *decoder);
    std::int32_t sceVideodec2Reset(void *decoder);
    std::int32_t sceVideodec2Decode(void *decoder, SceVideodec2InputData *input, SceVideodec2FrameBuffer *frame,
                                    SceVideodec2OutputInfo *output);
    std::int32_t sceVideodec2Flush(void *decoder, SceVideodec2FrameBuffer *frame, SceVideodec2OutputInfo *output);
}

static_assert(sizeof(SceVideodec2DecoderConfigInfo) == 72, "decoder config must keep the verified layout");
static_assert(sizeof(SceVideodec2DecoderMemoryInfo) == 72, "decoder memory info must keep the verified layout");
static_assert(sizeof(SceVideodec2ComputeConfigInfo) == 16, "compute config must keep the verified layout");
static_assert(sizeof(SceVideodec2ComputeMemoryInfo) == 24, "compute memory info must keep the verified layout");
static_assert(sizeof(SceVideodec2InputData) == 48, "input data must keep the verified layout");
static_assert(sizeof(SceVideodec2FrameBuffer) == 32, "frame buffer must keep the verified layout");
static_assert(sizeof(SceVideodec2OutputInfo) == 56, "output info must keep the verified layout");

namespace mirage
{
namespace
{
constexpr std::uint32_t kSysmoduleVideodec2 = 207;
constexpr std::uint32_t kCodecAvc = 1;
constexpr std::uint32_t kProfileHigh = 100;
constexpr std::uint32_t kLevel51 = 51;
constexpr std::int32_t kMaxWidth = 1920;
constexpr std::int32_t kMaxHeight = 1088;
constexpr int kProtCpuGpu = 0x33;  // CPU read/write, GPU all
constexpr int kProtGpu = 0x32;     // CPU write, GPU all
constexpr int kProtCpu = 0x03;     // flexible memory, CPU read/write
constexpr int kMemoryType = 12;
constexpr std::size_t kAlignment = 0x4000;
constexpr std::size_t kInputSlotBytes = 0x800000;
constexpr unsigned kSlots = 3;
constexpr unsigned kQueueUnits = 256; // a film is sent a second ahead, 60 units of it at 60 pictures a second
// A title opened by the payload connects seconds into a stream and is handed
// everything since the keyframe at once. The link thread waits this long for
// room in the queue before giving the stream up until its next keyframe.
constexpr unsigned kQueueWaitMs = 5000;

constexpr std::size_t align_16k(std::size_t value) noexcept
{
    return (value + 0x3fff) & ~static_cast<std::size_t>(0x3fff);
}

struct Unit
{
    std::uint8_t *data;
    std::size_t length;
    std::int64_t time_ns;
    bool keyframe;
    bool ordered; // a film (PCM_LINK_FLAG_ORDERED): decoding order, B-frames, sent ahead of time
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// Guarded by lock.
Unit queue[kQueueUnits];
unsigned queue_read = 0, queue_count = 0;
bool need_keyframe = true;
VdecStatus status{};

std::atomic<bool> ready{false};

// The last few pictures, in ordinary memory, oldest first, so that the
// screen can show each one when it is due and not merely the newest.
// Guarded by picture_lock.
// 24 is 400 ms at 60 pictures a second: room for the delay the sound needs.
constexpr unsigned kPictures = 24;
struct StoredPicture
{
    std::uint8_t luma[kMaxWidth * kMaxHeight];
    std::uint8_t chroma[kMaxWidth * kMaxHeight / 2];
    unsigned width, height;
    std::int64_t time_ns;
    unsigned long serial;
};
pthread_mutex_t picture_lock = PTHREAD_MUTEX_INITIALIZER;
StoredPicture pictures[kPictures];
// Which slots hold pictures, in order of the pictures' times, oldest first.
// (A film's pictures do not come out of the decoder in that order.)
unsigned picture_order[kPictures];
unsigned picture_count = 0;
bool picture_slot_used[kPictures];
unsigned long picture_serial = 0; // counts every picture ever stored
std::atomic<unsigned> pictures_waiting{0}; // picture_count, readable without the lock
// After a stream reset the picture that was on screen stays for a moment, so
// that a seek shows the old picture until the new place has one, and not the
// status screen in between. It goes when a new picture comes or its time is up.
constexpr std::int64_t kLeftoverSeconds = 3;
bool leftover = false;
std::int64_t leftover_until = 0; // CLOCK_MONOTONIC seconds

std::int64_t picture_clock_seconds() noexcept
{
    timespec now{};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

void drop_oldest_picture_locked() noexcept
{
    picture_slot_used[picture_order[0]] = false;
    for (unsigned i = 1; i < picture_count; ++i)
        picture_order[i - 1] = picture_order[i];
    --picture_count;
    pictures_waiting.store(picture_count);
}

void clear_pictures_locked() noexcept
{
    for (bool &used : picture_slot_used)
        used = false;
    picture_count = 0;
    pictures_waiting.store(0);
    leftover = false;
}

// The times of the film units the decoder has taken and not yet answered with
// a picture, smallest first. A decoder that reorders gives pictures back in
// order of time, some units late; one that does not answers each unit at
// once. Either way the next picture out belongs to the smallest time waiting.
// Owned by the decoder thread.
constexpr unsigned kPendingTimes = 32;
std::int64_t pending_times[kPendingTimes];
unsigned pending_count = 0;

void pending_add(std::int64_t time) noexcept
{
    if (pending_count == kPendingTimes)
    { // the decoder is holding more than it ever should: forget the oldest
        for (unsigned i = 1; i < pending_count; ++i)
            pending_times[i - 1] = pending_times[i];
        --pending_count;
    }
    unsigned at = pending_count;
    while (at > 0 && pending_times[at - 1] > time)
    {
        pending_times[at] = pending_times[at - 1];
        --at;
    }
    pending_times[at] = time;
    ++pending_count;
}

std::int64_t pending_take(std::int64_t fallback) noexcept
{
    if (pending_count == 0)
        return fallback;
    const std::int64_t time = pending_times[0];
    for (unsigned i = 1; i < pending_count; ++i)
        pending_times[i - 1] = pending_times[i];
    --pending_count;
    return time;
}

// Owned by the decoder thread.
void *decoder = nullptr;
std::uint8_t *input_pool = nullptr;
std::uint8_t *frame_pool = nullptr;
std::size_t frame_slot_bytes = 0;
unsigned long slot_counter = 0;

void set_stage(VdecStage stage, int result, bool failed) noexcept
{
    pthread_mutex_lock(&lock);
    status.stage = stage;
    status.result = result;
    status.failed = failed;
    pthread_mutex_unlock(&lock);
}

// Runs one bring-up step: records it before the call, and its result after.
#define VDEC_STEP(stage_value, call)                                                                           \
    do                                                                                                         \
    {                                                                                                          \
        set_stage(stage_value, 0, false);                                                                      \
        const int step_result = static_cast<int>(call);                                                        \
        if (step_result != 0)                                                                                  \
        {                                                                                                      \
            set_stage(stage_value, step_result, true);                                                         \
            return false;                                                                                      \
        }                                                                                                      \
    } while (0)

int allocate_direct(std::size_t size, int protection, std::int64_t limit, void **address) noexcept
{
    std::int64_t start = 0;
    int result = sceKernelAllocateDirectMemory(0, limit, size, kAlignment, kMemoryType, &start);
    if (result == 0)
        result = sceKernelMapDirectMemory(address, size, protection, 0, start, kAlignment);
    return result;
}

// Kept for re-creating the decoder on the same memory.
SceVideodec2DecoderConfigInfo decoder_config{};
SceVideodec2DecoderMemoryInfo decoder_memory{};
// The stream's current sequence parameter set, to notice when it changes.
std::uint8_t current_sps[256];
std::size_t current_sps_length = 0;
unsigned errors_in_a_row = 0;
// Set by vdec_reset_stream (another thread): the next unit starts a new stream.
std::atomic<bool> stream_restarted{false};

// The bytes of the first SPS (NAL type 7) in an Annex-B unit, or 0 if none.
std::size_t find_sps(const std::uint8_t *data, std::size_t length, const std::uint8_t **sps) noexcept
{
    for (std::size_t i = 0; i + 4 < length; ++i)
    {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1 || (data[i + 3] & 0x1f) != 7)
            continue;
        const std::size_t start = i + 3;
        std::size_t end = start + 1;
        while (end + 2 < length && !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 || data[end + 2] == 0)))
            ++end;
        if (end + 2 >= length)
            end = length;
        *sps = data + start;
        return end - start;
    }
    return 0;
}

bool bring_up() noexcept
{
    SceVideodec2ComputeMemoryInfo compute_memory{};
    SceVideodec2ComputeConfigInfo compute_config{};
    SceVideodec2DecoderConfigInfo config{};
    SceVideodec2DecoderMemoryInfo memory{};
    void *compute_queue = nullptr;
    void *pool = nullptr;

    VDEC_STEP(VdecStage::load_module, sceSysmoduleLoadModule(kSysmoduleVideodec2));
    const std::int64_t limit = sceKernelGetDirectMemorySize();

    compute_memory.size = sizeof(compute_memory);
    VDEC_STEP(VdecStage::query_compute, sceVideodec2QueryComputeMemoryInfo(&compute_memory));
    const std::size_t compute_bytes = align_16k(static_cast<std::size_t>(compute_memory.cpu_gpu_size));
    VDEC_STEP(VdecStage::allocate_compute,
              allocate_direct(compute_bytes, kProtCpuGpu, limit, &compute_memory.cpu_gpu));
    compute_memory.cpu_gpu_size = compute_bytes;
    compute_config.size = sizeof(compute_config);
    VDEC_STEP(VdecStage::compute_queue,
              sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &compute_queue));

    config.size = sizeof(config);
    config.resource_type = 1; // the compute path
    config.codec_type = kCodecAvc;
    config.profile = kProfileHigh;
    config.max_level = kLevel51;
    config.max_width = kMaxWidth;
    config.max_height = kMaxHeight;
    config.max_dpb_frames = 4;
    config.pipeline_depth = 1;
    config.compute_queue = reinterpret_cast<std::uint64_t>(compute_queue);
    config.cpu_affinity = 0x3f;
    config.cpu_priority = 700;
    config.optimize_progressive = 1;

    memory.size = sizeof(memory);
    VDEC_STEP(VdecStage::query_decoder, sceVideodec2QueryDecoderMemoryInfo(&config, &memory));
    VDEC_STEP(VdecStage::map_cpu,
              sceKernelMapNamedFlexibleMemory(&memory.cpu, align_16k(static_cast<std::size_t>(memory.cpu_size)),
                                              kProtCpu, 0, "MirageVdecCpu"));
    const std::size_t gpu_bytes = align_16k(static_cast<std::size_t>(memory.gpu_size));
    VDEC_STEP(VdecStage::allocate_gpu, allocate_direct(gpu_bytes, kProtGpu, limit, &memory.gpu));
    memory.gpu_size = gpu_bytes;
    if (memory.cpu_gpu_size != 0)
    {
        const std::size_t shared_bytes = align_16k(static_cast<std::size_t>(memory.cpu_gpu_size));
        VDEC_STEP(VdecStage::allocate_shared, allocate_direct(shared_bytes, kProtCpuGpu, limit, &memory.cpu_gpu));
        memory.cpu_gpu_size = shared_bytes;
    }
    VDEC_STEP(VdecStage::create, sceVideodec2CreateDecoder(&config, &memory, &decoder));
    decoder_config = config;
    decoder_memory = memory;

    frame_slot_bytes = align_16k(static_cast<std::size_t>(memory.max_frame_size));
    VDEC_STEP(VdecStage::allocate_pools, allocate_direct(kInputSlotBytes * kSlots, kProtGpu, limit, &pool));
    input_pool = static_cast<std::uint8_t *>(pool);
    VDEC_STEP(VdecStage::allocate_pools, allocate_direct(frame_slot_bytes * kSlots, kProtGpu, limit, &pool));
    frame_pool = static_cast<std::uint8_t *>(pool);

    VDEC_STEP(VdecStage::reset, sceVideodec2Reset(decoder));
    set_stage(VdecStage::ready, 0, false);
    return true;
}

unsigned elapsed_ms(const timespec &from) noexcept
{
    timespec now{};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned>((now.tv_sec - from.tv_sec) * 1000 + (now.tv_nsec - from.tv_nsec) / 1000000);
}

// Deletes the decoder and creates it again with the same configuration on
// the same memory, as ProsperoLight does when its decoder stops working.
int recreate_decoder() noexcept
{
    (void)sceVideodec2DeleteDecoder(decoder);
    decoder = nullptr;
    int result = sceVideodec2CreateDecoder(&decoder_config, &decoder_memory, &decoder);
    if (result == 0)
        result = sceVideodec2Reset(decoder);
    return result;
}

void decode(const Unit &unit) noexcept
{
    // A new sequence parameter set means a new picture size or layout (the
    // phone was turned). Fed straight in, the decoder refused it and every
    // keyframe after it (0x811D0300, seen on the console 2026-10-05): it has
    // to be reset first.
    if (stream_restarted.exchange(false) && decoder)
    {
        pending_count = 0;
        const int reset = sceVideodec2Reset(decoder);
        current_sps_length = 0; // whatever SPS comes next is this stream's first
        pthread_mutex_lock(&lock);
        status.reset_result = reset;
        pthread_mutex_unlock(&lock);
    }
    const std::uint8_t *sps = nullptr;
    const std::size_t sps_length = find_sps(unit.data, unit.length, &sps);
    if (sps_length > 0 && sps_length <= sizeof(current_sps) &&
        (sps_length != current_sps_length || std::memcmp(sps, current_sps, sps_length) != 0))
    {
        const bool first = current_sps_length == 0;
        std::memcpy(current_sps, sps, sps_length);
        current_sps_length = sps_length;
        if (!first && decoder)
        {
            const int reset = sceVideodec2Reset(decoder);
            pthread_mutex_lock(&lock);
            ++status.sequence_changes;
            status.reset_result = reset;
            pthread_mutex_unlock(&lock);
        }
    }
    if (!decoder)
    { // a re-creation failed: try again on each keyframe
        const int result = unit.keyframe ? recreate_decoder() : -1;
        pthread_mutex_lock(&lock);
        if (unit.keyframe)
        {
            ++status.recreations;
            status.reset_result = result;
        }
        if (result != 0)
        {
            ++status.dropped;
            need_keyframe = true;
        }
        pthread_mutex_unlock(&lock);
        if (result != 0)
        {
            decoder = nullptr;
            return;
        }
    }

    const unsigned slot = static_cast<unsigned>(slot_counter++ % kSlots);
    SceVideodec2InputData input{};
    SceVideodec2FrameBuffer frame{};
    SceVideodec2OutputInfo output{};
    timespec started{};

    std::memcpy(input_pool + slot * kInputSlotBytes, unit.data, unit.length);
    input.size = sizeof(input);
    input.au = input_pool + slot * kInputSlotBytes;
    input.au_size = unit.length;
    input.pts = static_cast<std::uint64_t>(unit.time_ns / 1000);
    input.dts = UINT64_MAX;
    frame.size = sizeof(frame);
    frame.buffer = frame_pool + slot * frame_slot_bytes;
    frame.buffer_size = frame_slot_bytes;
    output.size = sizeof(output);

    if (unit.ordered)
        pending_add(unit.time_ns);
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    int result = sceVideodec2Decode(decoder, &input, &frame, &output);
    if (result == 0 && !output.error && !output.valid && !unit.ordered)
    { // a mirrored screen: taken but held back; with a pipeline depth of one, a flush brings it out.
      // (Not for a film: its decoder holds pictures back to put them in order, and a flush would
      // end the sequence it is in the middle of.)
        std::memset(&output, 0, sizeof(output));
        output.size = sizeof(output);
        result = sceVideodec2Flush(decoder, &frame, &output);
    }
    const unsigned took = elapsed_ms(started);
    bool failed = false;
    const bool held = unit.ordered && result == 0 && !output.error && !output.valid;
    // The time of the picture that came out (if one did).
    const std::int64_t picture_time =
        unit.ordered && result == 0 && !output.error && output.valid ? pending_take(unit.time_ns) : unit.time_ns;
    if (result != 0 || output.error)
        pending_count = 0; // the sequence is broken: what was waiting will not come

    // Keep the picture. The decoder's rows are pitch_bytes apart and the
    // chroma plane starts after `height` luma rows; 8 bit NV12 only.
    unsigned copy_took = 0;
    if (result == 0 && !output.error && output.valid && output.buffer && output.width >= 2 && output.height >= 2 &&
        output.width <= static_cast<unsigned>(kMaxWidth) && output.height <= static_cast<unsigned>(kMaxHeight) &&
        output.pitch_bytes >= output.width && output.pitch_bytes == output.pitch &&
        static_cast<std::uint64_t>(output.pitch_bytes) * output.height * 3 / 2 <= frame_slot_bytes)
    {
        timespec copy_started{};
        (void)clock_gettime(CLOCK_MONOTONIC, &copy_started);
        const auto *source = static_cast<const std::uint8_t *>(output.buffer);
        const unsigned width = output.width & ~1u, height = output.height & ~1u;
        pthread_mutex_lock(&picture_lock);
        if (leftover)
            clear_pictures_locked(); // the new stream has a picture: the old one goes
        if (picture_count == kPictures)
            drop_oldest_picture_locked(); // nobody showed the oldest in time: it goes
        unsigned slot_index = 0;
        while (picture_slot_used[slot_index])
            ++slot_index; // there is a free one: fewer than kPictures are in use
        StoredPicture &stored = pictures[slot_index];
        for (unsigned row = 0; row < height; ++row)
            std::memcpy(stored.luma + row * width, source + static_cast<std::size_t>(row) * output.pitch_bytes, width);
        const std::uint8_t *chroma = source + static_cast<std::size_t>(output.pitch_bytes) * output.height;
        for (unsigned row = 0; row < height / 2; ++row)
            std::memcpy(stored.chroma + row * width, chroma + static_cast<std::size_t>(row) * output.pitch_bytes, width);
        stored.width = width;
        stored.height = height;
        stored.time_ns = picture_time;
        stored.serial = ++picture_serial;
        // Into its place by time; almost always the end.
        unsigned at = picture_count;
        while (at > 0 && pictures[picture_order[at - 1]].time_ns > picture_time)
        {
            picture_order[at] = picture_order[at - 1];
            --at;
        }
        picture_order[at] = slot_index;
        picture_slot_used[slot_index] = true;
        const bool out_of_order = at != picture_count;
        ++picture_count;
        pictures_waiting.store(picture_count);
        pthread_mutex_unlock(&picture_lock);
        if (out_of_order)
        {
            pthread_mutex_lock(&lock);
            ++status.reordered;
            pthread_mutex_unlock(&lock);
        }
        copy_took = elapsed_ms(copy_started);
    }

    pthread_mutex_lock(&lock);
    ++status.submitted;
    if (took > status.worst_decode_ms)
        status.worst_decode_ms = took;
    if (copy_took > status.worst_copy_ms)
        status.worst_copy_ms = copy_took;
    if (result != 0 || output.error)
    {
        ++status.errors;
        status.last_error = result != 0 ? result : -static_cast<int>(output.error);
        need_keyframe = true; // what follows refers to a picture that never was
        failed = true;
    }
    else if (held)
    {
        ++status.held; // a film's decoder keeping a picture back for now
    }
    else if (!output.valid)
    {
        ++status.no_picture;
    }
    else
    {
        ++status.pictures;
        status.width = output.width;
        status.height = output.height;
        status.pitch = output.pitch;
        status.pitch_bytes = output.pitch_bytes;
        status.format = output.frame_format;
    }
    pthread_mutex_unlock(&lock);

    // Recover: a reset after one error; after a second error with nothing
    // decoded in between, a new decoder. Either way the next unit taken is a
    // keyframe (need_keyframe is set above).
    if (!failed)
    {
        errors_in_a_row = 0;
        return;
    }
    ++errors_in_a_row;
    const bool recreate = errors_in_a_row >= 2;
    int recovery = recreate ? recreate_decoder() : sceVideodec2Reset(decoder);
    if (recreate && recovery != 0)
        decoder = nullptr; // tried again at the next keyframe
    pthread_mutex_lock(&lock);
    if (recreate)
        ++status.recreations;
    status.reset_result = recovery;
    pthread_mutex_unlock(&lock);
    if (recreate)
        errors_in_a_row = 0;
}

void *decoder_main(void *) noexcept
{
    if (!bring_up())
        return nullptr;
#ifdef MIRAGE_VDEC_BRINGUP_ONLY
    return nullptr; // "ready" stays false: every unit is counted as dropped
#else
    ready.store(true);
    for (;;)
    {
        Unit unit{};
        bool have = false;
        pthread_mutex_lock(&lock);
        // A film arrives a second ahead of time. Its pictures are large, so
        // they are made only shortly before they are due: while the picture
        // store is nearly full of pictures still waiting, the next unit waits.
        if (queue_count > 0 && queue[queue_read].ordered && pictures_waiting.load() + 3 >= kPictures)
        {
            pthread_mutex_unlock(&lock);
            (void)sceKernelUsleep(2000);
            continue;
        }
        if (queue_count > 0)
        {
            unit = queue[queue_read];
            queue_read = (queue_read + 1) % kQueueUnits;
            --queue_count;
            have = true;
        }
        pthread_mutex_unlock(&lock);
        if (!have)
        {
            (void)sceKernelUsleep(2000);
            continue;
        }
        decode(unit);
        std::free(unit.data);
    }
#endif
}

void clear_queue_locked() noexcept
{
    while (queue_count > 0)
    {
        std::free(queue[queue_read].data);
        queue_read = (queue_read + 1) % kQueueUnits;
        --queue_count;
    }
    need_keyframe = true;
}
} // namespace

const char *vdec_stage_name(VdecStage stage) noexcept
{
    switch (stage)
    {
    case VdecStage::not_started:
        return "NOT STARTED";
    case VdecStage::load_module:
        return "LOAD MODULE";
    case VdecStage::query_compute:
        return "QUERY COMPUTE";
    case VdecStage::allocate_compute:
        return "ALLOCATE COMPUTE";
    case VdecStage::compute_queue:
        return "COMPUTE QUEUE";
    case VdecStage::query_decoder:
        return "QUERY DECODER";
    case VdecStage::map_cpu:
        return "MAP CPU MEMORY";
    case VdecStage::allocate_gpu:
        return "ALLOCATE GPU MEMORY";
    case VdecStage::allocate_shared:
        return "ALLOCATE SHARED MEMORY";
    case VdecStage::create:
        return "CREATE";
    case VdecStage::allocate_pools:
        return "ALLOCATE POOLS";
    case VdecStage::reset:
        return "RESET";
    case VdecStage::ready:
        return "READY";
    }
    return "UNKNOWN";
}

bool vdec_start() noexcept
{
    // Threads are not named: pthread_setname_np hangs on this platform.
    pthread_t thread;
    return pthread_create(&thread, nullptr, &decoder_main, nullptr) == 0;
}

void vdec_submit(std::int64_t time_ns, bool h265, bool keyframe, bool ordered, const std::uint8_t *annexb,
                 std::size_t length) noexcept
{
    pthread_mutex_lock(&lock);
    // The decoder takes a moment to come up after the title starts, and a
    // title opened for a mirroring session is handed that session's only
    // keyframe the instant it connects: wait for the decoder rather than drop
    // it (unless its bring-up has failed, when nothing will ever be decoded).
    for (unsigned waited = 0; !ready.load() && !status.failed && waited < kQueueWaitMs; ++waited)
    {
        pthread_mutex_unlock(&lock);
        (void)sceKernelUsleep(1000);
        pthread_mutex_lock(&lock);
    }
    bool take = ready.load() && !h265 && length > 0 && length <= kInputSlotBytes;
    if (take && need_keyframe && !keyframe)
        take = false;
    for (unsigned waited = 0; take && queue_count == kQueueUnits && waited < kQueueWaitMs; ++waited)
    { // full: let the decoder work some off
        pthread_mutex_unlock(&lock);
        (void)sceKernelUsleep(1000);
        pthread_mutex_lock(&lock);
    }
    if (take && queue_count == kQueueUnits)
    { // the decoder is not keeping up: start again from the next keyframe
        clear_queue_locked();
        take = false;
    }
    std::uint8_t *copy = take ? static_cast<std::uint8_t *>(std::malloc(length)) : nullptr;
    if (copy)
    {
        std::memcpy(copy, annexb, length);
        queue[(queue_read + queue_count) % kQueueUnits] = Unit{copy, length, time_ns, keyframe, ordered};
        ++queue_count;
        if (queue_count > status.queue_high)
            status.queue_high = queue_count;
        need_keyframe = false;
    }
    else
    {
        ++status.dropped;
    }
    pthread_mutex_unlock(&lock);
}

void vdec_reset_stream() noexcept
{
    pthread_mutex_lock(&lock);
    clear_queue_locked();
    pthread_mutex_unlock(&lock);
    stream_restarted.store(true);
    pthread_mutex_lock(&picture_lock);
    // The stream is over. What was waiting to be shown goes; the picture on
    // screen (the oldest) stays for a moment.
    if (picture_count > 0)
    {
        for (unsigned i = 1; i < picture_count; ++i)
            picture_slot_used[picture_order[i]] = false;
        picture_count = 1;
        pictures_waiting.store(0); // it does not count against the decoder's room
        leftover = true;
        leftover_until = picture_clock_seconds() + kLeftoverSeconds;
    }
    pthread_mutex_unlock(&picture_lock);
}

bool vdec_lock_picture(Picture &picture, std::int64_t due_ns) noexcept
{
    pthread_mutex_lock(&picture_lock);
    if (leftover && picture_clock_seconds() > leftover_until)
        clear_pictures_locked();
    // Everything older than the newest picture that is due has had its turn.
    while (picture_count > 1 && pictures[picture_order[1]].time_ns <= due_ns)
        drop_oldest_picture_locked();
    if (picture_count == 0)
    {
        pthread_mutex_unlock(&picture_lock);
        return false;
    }
    const StoredPicture &stored = pictures[picture_order[0]];
    picture = Picture{stored.luma, stored.chroma, stored.width, stored.height, stored.time_ns, stored.serial};
    return true;
}

void vdec_unlock_picture() noexcept
{
    pthread_mutex_unlock(&picture_lock);
}

VdecStatus vdec_status() noexcept
{
    pthread_mutex_lock(&lock);
    const VdecStatus copy = status;
    pthread_mutex_unlock(&lock);
    return copy;
}
} // namespace mirage
