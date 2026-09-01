#include <game_engine_shared/core_ipc.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace game_engine::ipc
{
    namespace
    {
        std::string lastErrorString()
        {
            const DWORD code = GetLastError();
            char buffer[256]{};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr,
                           code,
                           0,
                           buffer,
                           sizeof(buffer) - 1,
                           nullptr);
            std::string message = buffer;
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
                message.pop_back();
            return message + " (" + std::to_string(code) + ")";
        }
    }

    bool SharedMemory::create(const std::string& name, std::string& errorOut)
    {
        close();

        const auto size = static_cast<DWORD>(sizeof(ControlBlock));
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());
        if (!mapping)
        {
            errorOut = "CreateFileMapping failed: " + lastErrorString();
            return false;
        }

        controlBlock = static_cast<ControlBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
        if (!controlBlock)
        {
            errorOut = "MapViewOfFile failed: " + lastErrorString();
            close();
            return false;
        }

        // fresh mapping pages are zeroed, which is the valid initial state of
        // every field; only the identification needs stamping
        controlBlock->magic = Magic;
        controlBlock->version = Version;
        return true;
    }

    bool SharedMemory::open(const std::string& name, std::string& errorOut)
    {
        close();

        mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!mapping)
        {
            errorOut = "OpenFileMapping(" + name + ") failed: " + lastErrorString();
            return false;
        }

        controlBlock = static_cast<ControlBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
        if (!controlBlock)
        {
            errorOut = "MapViewOfFile failed: " + lastErrorString();
            close();
            return false;
        }

        if (controlBlock->magic != Magic || controlBlock->version != Version)
        {
            errorOut = "shared memory block has wrong magic/version";
            close();
            return false;
        }
        return true;
    }

    void SharedMemory::close()
    {
        if (controlBlock)
        {
            UnmapViewOfFile(controlBlock);
            controlBlock = nullptr;
        }
        if (mapping)
        {
            CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    void SharedMemory::publishFrame(const uint8_t* rgb24, uint32_t width, uint32_t height)
    {
        if (!controlBlock || width == 0 || height == 0 || width > MaxFrameWidth || height > MaxFrameHeight)
            return;

        const uint64_t sequence = controlBlock->latestSequence.load(std::memory_order_relaxed) + 2;
        const uint32_t slotIndex = (controlBlock->latestSlot.load(std::memory_order_relaxed) + 1) % FrameSlotCount;
        FrameSlot& slot = controlBlock->frames[slotIndex];

        slot.sequence.store(sequence - 1, std::memory_order_release);  // odd: write in progress
        slot.width = width;
        slot.height = height;
        std::memcpy(slot.pixels, rgb24, size_t{width} * height * 3);
        slot.sequence.store(sequence, std::memory_order_release);

        controlBlock->latestSlot.store(slotIndex, std::memory_order_relaxed);
        controlBlock->latestSequence.store(sequence, std::memory_order_release);
    }

    void SharedMemory::writeAudio(const uint8_t* pcm, size_t bytes)
    {
        if (!controlBlock || bytes == 0)
            return;

        const uint64_t written = controlBlock->audioWritten.load(std::memory_order_relaxed);
        const uint64_t read = controlBlock->audioRead.load(std::memory_order_acquire);
        const size_t freeBytes = AudioRingBytes - static_cast<size_t>(written - read);
        if (bytes > freeBytes)
            return;  // reader stalled - drop the batch rather than tear the ring

        const size_t offset = static_cast<size_t>(written % AudioRingBytes);
        const size_t firstChunk = std::min(bytes, AudioRingBytes - offset);
        std::memcpy(controlBlock->audioRing + offset, pcm, firstChunk);
        if (firstChunk < bytes)
            std::memcpy(controlBlock->audioRing, pcm + firstChunk, bytes - firstChunk);

        controlBlock->audioWritten.store(written + bytes, std::memory_order_release);
    }

    size_t SharedMemory::readAudio(std::vector<uint8_t>& pcm)
    {
        if (!controlBlock)
            return 0;

        const uint64_t read = controlBlock->audioRead.load(std::memory_order_relaxed);
        const uint64_t written = controlBlock->audioWritten.load(std::memory_order_acquire);
        const size_t bytes = static_cast<size_t>(written - read);
        if (bytes == 0)
            return 0;

        const size_t offset = static_cast<size_t>(read % AudioRingBytes);
        const size_t firstChunk = std::min(bytes, AudioRingBytes - offset);
        pcm.insert(pcm.end(), controlBlock->audioRing + offset, controlBlock->audioRing + offset + firstChunk);
        if (firstChunk < bytes)
            pcm.insert(pcm.end(), controlBlock->audioRing, controlBlock->audioRing + (bytes - firstChunk));

        controlBlock->audioRead.store(written, std::memory_order_release);
        return bytes;
    }

    bool SharedMemory::skipToLatestFrame(uint64_t& lastSequence) const
    {
        if (!controlBlock)
            return false;
        const uint64_t sequence = controlBlock->latestSequence.load(std::memory_order_acquire);
        if (sequence <= lastSequence)
            return false;
        lastSequence = sequence;
        return true;
    }

    bool SharedMemory::readLatestFrame(uint64_t& lastSequence, std::vector<uint8_t>& rgb24, uint32_t& width, uint32_t& height) const
    {
        if (!controlBlock)
            return false;

        for (int attempt = 0; attempt < 8; attempt++)
        {
            const uint64_t sequence = controlBlock->latestSequence.load(std::memory_order_acquire);
            if (sequence <= lastSequence)
                return false;

            const FrameSlot& slot = controlBlock->frames[controlBlock->latestSlot.load(std::memory_order_relaxed)];

            const uint64_t before = slot.sequence.load(std::memory_order_acquire);
            if (before % 2 != 0)
                continue;  // mid-write, retry against the (possibly new) latest slot

            const uint32_t slotWidth = slot.width;
            const uint32_t slotHeight = slot.height;
            if (slotWidth == 0 || slotHeight == 0 || slotWidth > MaxFrameWidth || slotHeight > MaxFrameHeight)
                return false;

            rgb24.resize(size_t{slotWidth} * slotHeight * 3);
            std::memcpy(rgb24.data(), slot.pixels, rgb24.size());
            std::atomic_thread_fence(std::memory_order_acquire);

            if (slot.sequence.load(std::memory_order_acquire) != before)
                continue;  // the host lapped us mid-copy

            width = slotWidth;
            height = slotHeight;
            lastSequence = before;
            return true;
        }
        return false;
    }
}
