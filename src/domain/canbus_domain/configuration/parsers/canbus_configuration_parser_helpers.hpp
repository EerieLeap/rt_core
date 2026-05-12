#pragma once

#include <stdexcept>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/fs/services/fs_service_stream_buf.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration::parsers {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::fs::services::FsServiceStreamBuf;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;

class CanbusConfigurationParserHelpers {
public:
    static void LoadDbcConfiguration(IFsService* fs_service, CanChannelConfiguration& channel_configuration) {
        if(fs_service != nullptr && fs_service->Exists(channel_configuration.dbc_file_path)) {
            FsServiceStreamBuf fs_stream_buf(
                fs_service,
                std::string(channel_configuration.dbc_file_path),
                FsServiceStreamBuf::OpenMode::Read);

            bool res = channel_configuration.dbc->LoadDbcFile(fs_stream_buf);
            fs_stream_buf.close();

            if(!res)
                throw std::runtime_error("Failed to load DBC file. " + std::string(channel_configuration.dbc_file_path));
        }
    }
};

} // namespace eerie_leap::domain::canbus_domain::configuration::parsers
