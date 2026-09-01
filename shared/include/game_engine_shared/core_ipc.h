/*
 * Shared-memory protocol between the DAQCube module and core_host.exe.
 *
 * The module creates a named file mapping, spawns the core host with the
 * mapping name on its command line and controls it exclusively through this
 * block: a command word for lifecycle, per-player RetroPad input state, and
 * a small ring of RGB24 framebuffers written by the host (seqlock per slot,
 * newest complete slot published through latestSlot/latestSequence).
 *
 * The block is zero-initialized by the mapping itself; both sides only ever
 * reinterpret the same physical pages, so plain std::atomic members work as
 * long as they are lock-free (statically asserted below).
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace game_engine::ipc
{
    inline constexpr uint32_t Magic = 0x314D4A44;  // "DJM1"
    inline constexpr uint32_t Version = 2;

    inline constexpr unsigned MaxPlayers = 4;
    inline constexpr unsigned FrameSlotCount = 3;
    inline constexpr unsigned MaxFrameWidth = 1920;
    inline constexpr unsigned MaxFrameHeight = 1080;
    inline constexpr size_t MaxFrameBytes = size_t{MaxFrameWidth} * MaxFrameHeight * 3;  // RGB24

    // interleaved stereo s16 PCM ring: ~1.5 s at 44.1 kHz - the module drains
    // it every few ms, the headroom just absorbs scheduling hiccups
    inline constexpr size_t AudioRingBytes = size_t{1} << 18;
    inline constexpr size_t AudioFrameBytes = 4;  // 2 channels x int16

    // module -> host
    enum class Command : uint32_t
    {
        Run = 0,
        Stop = 1,
    };

    // host -> module
    enum class HostStatus : uint32_t
    {
        Starting = 0,  // mapping just created / core still loading
        Running = 1,
        Stopped = 2,  // clean exit (Stop command or host window closed)
        Error = 3,    // details in ControlBlock::error
    };

    struct FrameSlot
    {
        // seqlock: odd while the host is writing; readers retry on odd or on a
        // value change across the copy
        std::atomic<uint64_t> sequence;
        uint32_t width;
        uint32_t height;
        uint8_t pixels[MaxFrameBytes];  // row-major RGB, width * height * 3 bytes used
    };

    struct ControlBlock
    {
        uint32_t magic;
        uint32_t version;

        std::atomic<uint32_t> command;  // Command
        std::atomic<uint32_t> status;   // HostStatus
        char error[256];                // valid when status == Error

        // filled by the host after the core is loaded, before status -> Running
        double coreFps;
        double audioSampleRate;  // 0 = the core produces no audio
        uint32_t baseWidth;
        uint32_t baseHeight;

        // incremented by the host every frame; the module watchdog checks progress
        std::atomic<uint64_t> heartbeat;

        // RetroPad button bitmask per player (bit n = RETRO_DEVICE_ID_JOYPAD_n)
        std::atomic<uint32_t> inputs[MaxPlayers];

        // newest complete frame: slot index and its (even) sequence number
        std::atomic<uint32_t> latestSlot;
        std::atomic<uint64_t> latestSequence;

        // SPSC audio byte ring (host writes, module reads): monotonically
        // increasing cursors, offset = cursor % AudioRingBytes; the writer
        // only ever writes whole AudioFrameBytes frames
        std::atomic<uint64_t> audioWritten;
        std::atomic<uint64_t> audioRead;
        uint8_t audioRing[AudioRingBytes];

        FrameSlot frames[FrameSlotCount];
    };

    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
    static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

    // Owns a named Windows file mapping view of one ControlBlock.
    class SharedMemory
    {
    public:
        SharedMemory() = default;
        ~SharedMemory() { close(); }
        SharedMemory(const SharedMemory&) = delete;
        SharedMemory& operator=(const SharedMemory&) = delete;

        // module side: create the mapping (zero pages) and stamp magic/version
        bool create(const std::string& name, std::string& errorOut);

        // host side: open an existing mapping and validate magic/version
        bool open(const std::string& name, std::string& errorOut);

        ControlBlock* block() const { return controlBlock; }
        void close();

        // frame written by the host into the next round-robin slot
        void publishFrame(const uint8_t* rgb24, uint32_t width, uint32_t height);

        // newest complete frame copied out by the module; returns false when no
        // frame newer than lastSequence is available (torn reads are retried)
        bool readLatestFrame(uint64_t& lastSequence, std::vector<uint8_t>& rgb24, uint32_t& width, uint32_t& height) const;

        // consume the newest frame without copying its pixels (frame thinning);
        // returns false when no frame newer than lastSequence is available
        bool skipToLatestFrame(uint64_t& lastSequence) const;

        // host side: append interleaved PCM to the audio ring; whole frames
        // only, silently dropped when the module is not draining fast enough
        void writeAudio(const uint8_t* pcm, size_t bytes);

        // module side: move everything currently in the audio ring into `pcm`
        // (appended); returns the number of bytes taken
        size_t readAudio(std::vector<uint8_t>& pcm);

    private:
        void* mapping = nullptr;  // HANDLE
        ControlBlock* controlBlock = nullptr;
    };
}
