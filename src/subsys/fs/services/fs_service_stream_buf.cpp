#include <cstring>
#include <system_error>

#include "utilities/string/filesystem_path.hpp"

#include "fs_service_stream_buf.h"

namespace eerie_leap::subsys::fs::services {

using namespace eerie_leap::utilities::string;

FsServiceStreamBuf::FsServiceStreamBuf(IFsService* fs_service, std::string_view relative_path, OpenMode mode)
    : fs_service_(fs_service), file_opened_(false), input_buffer_(BUFFER_SIZE) {

    if(!fs_service_)
        throw std::invalid_argument("fs_service cannot be null");

    if(!fs_service_->IsAvailable())
        throw std::runtime_error("Filesystem not available");

    if(mode == OpenMode::Write || mode == OpenMode::Append) {
        auto parent = FilesystemPath<PATH_BUFFER_SIZE>(relative_path).parent_path();
        if(!fs_service_->Exists(parent.String().ToString()) && !parent.String().Empty())
            fs_service_->CreateDirectory(parent.String().ToString());
    }

    fs_mode_t open_mode = 0;
    switch(mode) {
        case OpenMode::Read:
            open_mode = FS_O_READ;
            break;
        case OpenMode::Write:
            open_mode = FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC;
            break;
        case OpenMode::Append:
            open_mode = FS_O_WRITE | FS_O_CREATE | FS_O_APPEND;
            break;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path{fs_service_->GetMountpoint().mnt_point};
    full_path /= relative_path;

    fs_file_t_init(&file_);
    int rc = fs_open(&file_, full_path.String().CStr(), open_mode);

    if(rc < 0)
        throw std::system_error(-rc, std::generic_category(), "Failed to open file");

    file_opened_ = true;

    setg(nullptr, nullptr, nullptr);
}

FsServiceStreamBuf::~FsServiceStreamBuf() {
    close();
}

bool FsServiceStreamBuf::close() {
    if(file_opened_) {
        int rc = fs_close(&file_);
        file_opened_ = false;
        return rc == 0;
    }

    return true;
}

bool FsServiceStreamBuf::is_open() const {
    return file_opened_;
}

std::streamsize FsServiceStreamBuf::xsputn(const char* s, std::streamsize n) {
    if(!file_opened_)
        return 0;

    ssize_t rc = fs_write(&file_, s, static_cast<size_t>(n));
    if(rc < 0)
        return 0;

    return rc;
}

std::streambuf::int_type FsServiceStreamBuf::overflow(std::streambuf::int_type c) {
    if(c != traits_type::eof()) {
        char ch = traits_type::to_char_type(c);
        if(xsputn(&ch, 1) == 1)
            return c;
    }

    return traits_type::eof();
}

std::streamsize FsServiceStreamBuf::xsgetn(char* s, std::streamsize n) {
    if(!file_opened_)
        return 0;

    ssize_t rc = fs_read(&file_, s, static_cast<size_t>(n));
    if(rc < 0)
        return 0;

    return rc;
}

std::streambuf::int_type FsServiceStreamBuf::underflow() {
    if(!file_opened_)
        return traits_type::eof();

    if(gptr() < egptr())
        return traits_type::to_int_type(*gptr());

    ssize_t bytes_read = fs_read(&file_, input_buffer_.data(), input_buffer_.size());

    if(bytes_read <= 0)
        return traits_type::eof();

    setg(input_buffer_.data(), input_buffer_.data(), input_buffer_.data() + bytes_read);

    return traits_type::to_int_type(*gptr());
}

int FsServiceStreamBuf::sync() {
    if(file_opened_)
        return fs_sync(&file_);

    return -1;
}

std::streambuf::pos_type FsServiceStreamBuf::seekoff(
    std::streambuf::off_type off,
    std::ios_base::seekdir way,
    std::ios_base::openmode which) {

    if(!file_opened_)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    int whence = FS_SEEK_SET;
    if(way == std::ios_base::cur)
        whence = FS_SEEK_CUR;
    else if(way == std::ios_base::end)
        whence = FS_SEEK_END;

    int rc = fs_seek(&file_, static_cast<off_t>(off), whence);
    if(rc != 0)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    off_t pos = fs_tell(&file_);
    if(pos < 0)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    setg(nullptr, nullptr, nullptr);

    return std::streambuf::pos_type(static_cast<std::streambuf::off_type>(pos));
}

std::streambuf::pos_type FsServiceStreamBuf::seekpos(std::streambuf::pos_type sp, std::ios_base::openmode /*which*/) {
    auto off = static_cast<std::streambuf::off_type>(sp);
    return seekoff(off, std::ios_base::beg, std::ios_base::openmode(0));
}

} // namespace eerie_leap::subsys::fs::services
