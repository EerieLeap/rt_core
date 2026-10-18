#pragma once

#include <cstdint>

#include <zephyr/kernel.h>

#include "domain/sensor_domain/models/indicator_input_mode.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_type.h"

namespace eerie_leap::domain::sensor_domain::runtime {

struct SensorRuntime;

} // namespace eerie_leap::domain::sensor_domain::runtime

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::models::IndicatorInputMode;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;

class ReadingPipeline;

// A Toggle indicator's switch with what it was built from, kept across a restart so the next
// generation can take it over (SensorPipelineBuilder::Build()).
struct IndicatorSwitchState {
    uint32_t sensor_id_hash = 0;
    SensorType type = SensorType::NONE;
    uint32_t debounce_ms = 0;
    IndicatorInputMode input_mode = IndicatorInputMode::Toggle;
    bool is_on = false;
};

// Debounces an indicator and, in Toggle mode, makes it a switch. One per indicator with
// conditioning, owned by its SensorRuntime and built with its generation. ReadingPipeline offers
// it every processed reading of the sensor before the commit; it returns the reading to commit
// with the conditioned value, or holds a change until the input has kept it for the window and
// then commits it through the pipeline from a delayable work item on the processing work queue.
// Processing work queue only, apart from construction and the Cancel, Suspend and Resume calls.
class IndicatorConditioner {
private:
    struct SettleWork {
        k_work_delayable work;
        IndicatorConditioner* conditioner;
    };

    const k_ticks_t debounce_ticks_;
    const IndicatorInputMode input_mode_;

    bool has_input_ = false;    // A first value arrived in this generation.
    bool input_ = false;        // The conditioned input.
    bool is_switch_on_ = false; // Toggle only; seeded by the builder.

    bool has_pending_ = false;    // The raw input differs from input_ and its window runs.
    int64_t deadline_ = 0;        // Uptime ticks at which it settles.
    int64_t remaining_ticks_ = 0; // What was left of the window when processing paused.
    SensorReading held_;          // The latest reading carrying the pending value.
    ReadingPipeline* pipeline_ = nullptr;
    const runtime::SensorRuntime* runtime_ = nullptr;

    SettleWork settle_work_{};
    k_work_sync settle_sync_{};

    static void SettleWorkHandler(k_work* work);
    void Settle() noexcept;
    void Accept(bool input);
    void Apply(SensorReading& reading) const;

public:
    IndicatorConditioner(uint32_t debounce_ms, IndicatorInputMode input_mode, bool is_switch_on);
    ~IndicatorConditioner();

    // The work item points at the conditioner.
    IndicatorConditioner(const IndicatorConditioner&) = delete;
    IndicatorConditioner& operator=(const IndicatorConditioner&) = delete;
    IndicatorConditioner(IndicatorConditioner&&) = delete;
    IndicatorConditioner& operator=(IndicatorConditioner&&) = delete;

    /**
     * @brief Conditions a processed reading of the sensor. Processing work queue.
     * @return True to commit @p reading, which then carries the conditioned value; false when a
     *         change is held, to be committed through @p pipeline once it has settled.
     */
    bool Offer(ReadingPipeline& pipeline, const runtime::SensorRuntime& runtime, SensorReading& reading);

    /**
     * @brief Drops a held change and waits for a settle in progress. Only while no work of the
     *        generation can reach the conditioner: its sources are stopped or paused and the
     *        conditioners before it in processing order are cancelled. Never from the processing
     *        work queue.
     */
    void CancelSettle();

    /**
     * @brief Stops the window of a held change for a pause, keeping what is left of it, and waits
     *        for a settle in progress. Called as CancelSettle().
     */
    void SuspendSettle();

    /**
     * @brief Runs what was left of a suspended window from now. Before the sources resume, and
     *        after the conditioners after it in processing order, whose settles never reach it.
     */
    void ResumeSettle();

    [[nodiscard]] bool IsSwitch() const {
        return input_mode_ == IndicatorInputMode::Toggle;
    }
    [[nodiscard]] bool IsSwitchOn() const {
        return is_switch_on_;
    }
};

} // namespace eerie_leap::domain::sensor_domain::processors
