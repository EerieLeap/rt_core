#include "domain/can_monitor_domain/utilities/can_notification_cutter.h"

#include "domain/can_monitor_domain/utilities/can_record_reader.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

CanNotificationCut CanNotificationCutter::Cut(std::span<const uint8_t> records, size_t room, size_t max_records) {
    CanNotificationCut cut;

    while(cut.bytes < records.size()) {
        const CanRecordInspection record = InspectRecord(records.subspan(cut.bytes));

        if(record.status == CanRecordStatus::INCOMPLETE) {
            cut.incomplete_tail = true;
            break;
        }

        if(record.status == CanRecordStatus::MALFORMED) {
            cut.malformed = true;
            break;
        }

        if(record.size > room - cut.bytes) {
            if(cut.records == 0) {
                cut.first_too_large = true;
                cut.first_record_size = record.size;
                cut.first_record_dt_us = record.dt_us;
            }
            break;
        }

        if(cut.records >= max_records)
            break;

        cut.bytes += record.size;
        cut.dt_sum_us += record.dt_us;
        ++cut.records;
    }

    return cut;
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
