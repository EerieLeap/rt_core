#pragma once

#include <cstdint>
#include <memory>

#include "subsys/smp/smp_group.h"
#include "domain/adc_domain/services/calibration_service.h"

namespace eerie_leap::domain::adc_domain::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::domain::adc_domain::services::CalibrationService;

/**
 * @brief SMP `calibration` group: samples one ADC channel for a client that calibrates it.
 *
 * START pauses the service that owns the ADC and starts a CalibrationService job on the channel,
 * READ returns the job's latest result, STOP ends the job and resumes what START paused. The
 * handlers only start, stop and query the service: the sampling runs on its own thread, and the
 * MCUmgr work queue waits at most for a tick or a sample in progress when pausing or stopping.
 */
class CalibrationMgmtGroup : public SmpGroup<CalibrationMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        START = 0, ///< Write `{channel}`: `{}`. Samples the channel until STOP.
        STOP = 1,  ///< Write: `{}`.
        /// Read: `{channel, adc_v, sensor_v, ts, seq, adc_err, running}` of the running or last job:
        /// volts as float32, `ts` in ms since the epoch, `seq` 0 until the job's first result,
        /// `adc_err` 0 or the AdcError of the sample (`err` is SMP v2's group error map).
        READ = 2,
    };

    /** @brief Group error codes, returned in the SMP v2 `err` map. */
    enum class Error : uint16_t {
        OK = 0,
        UNKNOWN = 1,
        BAD_CHANNEL = 2, ///< The unit has no such ADC channel.
        BUSY = 3,        ///< A job runs, or the service that owns the ADC could not be paused.
        NOT_RUNNING = 4, ///< No job to stop, or no job and no result to read.
    };

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<CalibrationService> calibration_service_;
    int channel_count_;

    int HandleStart(smp_streamer* ctxt);
    int HandleStop(smp_streamer* ctxt);
    int HandleRead(smp_streamer* ctxt);

    static int Fail(smp_streamer* ctxt, Error error);
    static int TranslateError(uint16_t error);

public:
    /**
     * @param channel_count ADC channels of the unit; START rejects any other channel itself, so the
     *        service's channel lookup never throws on the MCUmgr work queue.
     */
    CalibrationMgmtGroup(std::shared_ptr<CalibrationService> calibration_service, int channel_count);
    ~CalibrationMgmtGroup();
};

} // namespace eerie_leap::domain::adc_domain::smp
