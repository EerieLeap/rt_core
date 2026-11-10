#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

#include "subsys/canbus/can_frame.h"

// The header is pulled in by code that can be built without the transmit part.
#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE
    #define CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE 65536
#endif

#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_LATE_MS
    #define CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_LATE_MS 10
#endif

namespace eerie_leap::domain::can_monitor_domain::transmit {

/** @brief What AddChunk() made of a chunk. */
enum class CanReplayAdd : uint8_t {
    OK,
    /** @brief The replay is not started, or Finish() was called. */
    BAD_STATE,
    /** @brief Not the next @c seq. */
    BAD_SEQ,
    /** @brief More bytes than Room(). */
    NO_ROOM,
    /** @brief Not whole kind 0 frame records: a record of another kind, a malformed one, or one split at the end. */
    MALFORMED,
};

/** @brief A frame whose time has come. */
struct CanReplayFrame {
    /** @brief Ready for Canbus::SendFrame(): the record's IDE, FDF, BRS, RTR and payload; ESI is not carried. */
    eerie_leap::subsys::canbus::CanFrame frame;
    /** @brief More than CanReplayBuffer::LATE_US after its time: sent at once, and counted. */
    bool late = false;
    /** @brief How long after its time the frame was handed out. */
    uint64_t late_us = 0;
};

/**
 * @brief The pure part of REPLAY (§6.5): the app's chunks of kind 0 frame records, played back in time.
 *
 * The caller owns the clock and the bus: it passes the time of every call and sends what Pop() hands out.
 * Nothing here blocks, waits or reads a clock, and nothing is thread safe: the caller serialises the calls
 * (a chunk arrives on one thread and the playback runs on another).
 *
 * - **Storage.** CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE bytes from a memory resource, used as a
 *   ring. Room() is what a chunk may take: the bytes not waiting to be played.
 * - **Timing.** A record's dt is the time since the record before it in the app's capture. Its due time is
 *   the previous frame's plus @c dt * 100 / speed_pct, carrying the division's remainder so that no time is
 *   lost however long the replay runs. The first frame is due at the replay's start plus its own dt. A
 *   frame handed out more than LATE_US after its time is late: it goes at once and is counted, and the
 *   frames behind it count from the time it went (the gaps are kept; a stall does not turn into a burst).
 * - **Chunks.** In order of @c seq from 0 after Start(), each whole kind 0 records (a chunk boundary never
 *   splits a record). A refused chunk leaves everything as it was, @c seq included, so it may be sent again.
 *   A chunk that arrives into an empty buffer (the first one, or after an underrun) restarts the timing:
 *   its first frame is due at the time the chunk arrived plus its dt. For the first chunk, that is the
 *   start's time when it comes with the start.
 * - **Consumed bytes.** Without @c loop a frame's bytes are free once it was handed out. With @c loop they
 *   stay, so the replay can start over, until a chunk needs the room: then they go, the replay did not
 *   fit, and @c loop has no effect any more (the app sends the range again).
 * - **Loop.** After Finish(), when the last frame has been handed out and nothing was ever discarded,
 *   playback starts over from the first frame. The timing restarts at the time the last frame went: the
 *   first frame is due then plus its own dt.
 * - **Underrun.** The buffer is empty before Finish(): IsUnderrun(). Underruns() counts the times a Pop()
 *   emptied it, so the caller writes one tx event for each; an empty buffer after Start() is not counted.
 *   Playback goes on when a chunk comes, with the timing restarted as above.
 * - **Done.** The buffer is empty after Finish() and the replay does not loop: IsDone().
 */
class CanReplayBuffer {
private:
    std::pmr::memory_resource* memory_;
    uint8_t* data_;
    size_t capacity_;

    // The ring: retained_ bytes consumed and kept for a loop, then pending_ bytes still to play, from start_.
    size_t start_ = 0;
    size_t retained_ = 0;
    size_t pending_ = 0;

    bool active_ = false;
    bool loop_ = false;
    bool finished_ = false;
    bool discarded_ = false;
    uint32_t speed_pct_ = 100;
    uint16_t next_seq_ = 0;

    // The time the dt of the next frame counts from, the remainder of the last scaled dt, and the
    // time the next frame is due (valid while pending_ > 0).
    uint64_t base_us_ = 0;
    uint64_t carry_ = 0;
    uint64_t due_us_ = 0;

    uint32_t frames_ = 0;
    uint32_t late_ = 0;
    uint32_t underruns_ = 0;
    uint32_t loops_ = 0;

    CanReplayBuffer(std::pmr::memory_resource* memory, uint8_t* data, size_t capacity);

    [[nodiscard]] size_t Wrap(size_t position) const;
    void CopyOut(size_t offset, std::span<uint8_t> out) const;
    void CopyIn(size_t offset, std::span<const uint8_t> in);
    void PrepareNext();
    void TryRestart();
    [[nodiscard]] std::optional<CanReplayFrame> ReadNext(size_t& record_size, uint64_t& dt_us) const;

public:
    /** @brief The slowest and the fastest replay Start() takes, in percent. */
    static constexpr uint32_t MIN_SPEED_PCT = 1;
    static constexpr uint32_t MAX_SPEED_PCT = 10000;

    /** @brief A frame later than this is late, in µs. */
    static constexpr uint64_t LATE_US = static_cast<uint64_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_LATE_MS) * 1000;

    /** @brief A record's dt is cut to this (about 31 years), so the timing arithmetic cannot overflow. */
    static constexpr uint64_t MAX_DT_US = 1'000'000'000'000'000;

    /**
     * @brief Makes a buffer of @p capacity bytes from @p memory.
     * @return nullptr when @p capacity is 0, @p memory is null or the memory can't be had.
     */
    static std::unique_ptr<CanReplayBuffer>
    Create(std::pmr::memory_resource* memory, size_t capacity = CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE);

    ~CanReplayBuffer();

    CanReplayBuffer(const CanReplayBuffer&) = delete;
    CanReplayBuffer& operator=(const CanReplayBuffer&) = delete;
    CanReplayBuffer(CanReplayBuffer&&) = delete;
    CanReplayBuffer& operator=(CanReplayBuffer&&) = delete;

    /**
     * @brief Starts a replay: the buffer empties, @c seq goes back to 0 and the counters to 0.
     * @param speed_pct 100 plays at the capture's pace, 200 twice as fast; MIN_SPEED_PCT to MAX_SPEED_PCT.
     * @return false (and nothing changed) for a speed out of range.
     */
    bool Start(uint32_t speed_pct, bool loop, uint64_t now_us);

    /**
     * @brief Adds a chunk of kind 0 frame records.
     * @param seq The chunk's number: 0 for the first after Start(), then one more for each accepted chunk.
     * @param bytes Whole records. An empty chunk is accepted.
     * @param now_us The time the chunk arrived (it restarts the timing of an empty buffer).
     */
    CanReplayAdd AddChunk(uint16_t seq, std::span<const uint8_t> bytes, uint64_t now_us);

    /** @brief No more chunks will come. */
    void Finish();

    /** @brief Ends the replay and empties the buffer. The counters stay until the next Start(). */
    void Stop();

    /** @brief The bytes a chunk may take now: the capacity less the bytes still to be played. */
    [[nodiscard]] size_t Room() const {
        return capacity_ - pending_;
    }

    [[nodiscard]] size_t Capacity() const {
        return capacity_;
    }

    /**
     * @brief The time the next frame is due, on the base of the times passed in; nothing when no frame waits.
     *
     * A looping replay that has just started over has its first frame due at the time the last one went
     * plus the first one's dt.
     */
    [[nodiscard]] std::optional<uint64_t> NextDueUs() const;

    /** @brief The next frame whatever its time (to cost it before it goes); nothing when none waits. */
    [[nodiscard]] std::optional<CanReplayFrame> Peek() const;

    /**
     * @brief Hands out the next frame if its time has come (@p now_us >= NextDueUs()).
     *
     * The frame is counted as handed out whether or not the caller manages to send it.
     */
    std::optional<CanReplayFrame> Pop(uint64_t now_us);

    [[nodiscard]] bool IsActive() const {
        return active_;
    }
    [[nodiscard]] bool IsFinished() const {
        return active_ && finished_;
    }
    /** @brief Frames wait to be played. */
    [[nodiscard]] bool HasFrames() const {
        return pending_ > 0;
    }
    /** @brief Started, nothing waits and Finish() was not called: the caller waits for the app. */
    [[nodiscard]] bool IsUnderrun() const {
        return active_ && !finished_ && pending_ == 0;
    }
    /** @brief Started, Finish() called, nothing waits and the replay does not start over. */
    [[nodiscard]] bool IsDone() const {
        return active_ && finished_ && pending_ == 0;
    }
    /** @brief Whether the replay would start over: @c loop was asked for and nothing was discarded yet. */
    [[nodiscard]] bool CanLoop() const {
        return active_ && loop_ && !discarded_;
    }

    /** @brief Frames handed out since Start(), a looping replay's repeats included. */
    [[nodiscard]] uint32_t FramesPlayed() const {
        return frames_;
    }
    /** @brief The frames among them that were late. */
    [[nodiscard]] uint32_t LateFrames() const {
        return late_;
    }
    /** @brief The times a Pop() emptied the buffer before Finish(). */
    [[nodiscard]] uint32_t Underruns() const {
        return underruns_;
    }
    /** @brief The times a looping replay started over. */
    [[nodiscard]] uint32_t Loops() const {
        return loops_;
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::transmit
