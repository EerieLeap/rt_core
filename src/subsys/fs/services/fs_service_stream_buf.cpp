#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "utilities/string/filesystem_path.hpp"

#include "fs_service_stream_buf.h"

namespace eerie_leap::subsys::fs::services {

using namespace eerie_leap::utilities::string;

FsServiceStreamBuf::FsServiceStreamBuf(IFsService* fs_service, std::string_view relative_path, OpenMode mode) :
    fs_service_(fs_service),
    file_opened_(false) {

    if(!fs_service_)
        throw std::invalid_argument("fs_service cannot be null");

    if(!fs_service_->IsAvailable())
        throw std::runtime_error("Filesystem not available");

    if(relative_path.size() > PATH_BUFFER_SIZE)
        throw std::invalid_argument("relative_path is too long");

    if(mode == OpenMode::Write || mode == OpenMode::Append) {
        auto parent = FilesystemPath<PATH_BUFFER_SIZE>(relative_path).parent_path();

        if(!parent.String().Empty()
            && !fs_service_->Exists(parent.String().ToString())
            && !fs_service_->CreateDirectory(parent.String().ToString()))
            throw std::system_error(EIO, std::generic_category(), "Failed to create parent directory");
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

    // Allocated before the file is opened so that a failure here cannot leak the handle.
    if(mode == OpenMode::Read)
        input_buffer_.resize(BUFFER_SIZE);
    else
        output_buffer_.resize(CONFIG_EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE);

    const int rc = fs_service_->OpenFile(relative_path, open_mode, &file_);

    if(rc < 0)
        throw std::system_error(-rc, std::generic_category(), "Failed to open file");

    file_opened_ = true;

    setg(nullptr, nullptr, nullptr);
    ResetPutArea();
}

FsServiceStreamBuf::~FsServiceStreamBuf() {
    close();
}

bool FsServiceStreamBuf::close() {
    if(file_opened_) {
        const bool flushed = FlushPutArea();
        const int rc = fs_close(&file_);
        file_opened_ = false;

        // Without a put area every later sputc() reaches overflow() and is refused like sputn() is.
        setp(nullptr, nullptr);
        setg(nullptr, nullptr, nullptr);

        return flushed && rc == 0;
    }

    return true;
}

bool FsServiceStreamBuf::is_open() const {
    return file_opened_;
}

// An empty put area sends every sputc() through overflow() and every sputn() through xsputn(),
// which is how a latched stream refuses them; a healthy one spans the whole buffer.
void FsServiceStreamBuf::ResetPutArea() {
    if(write_failed_)
        setp(nullptr, nullptr);
    else
        setp(output_buffer_.data(), output_buffer_.data() + output_buffer_.size());
}

void FsServiceStreamBuf::MarkWriteFailed() {
    write_failed_ = true;
    setp(nullptr, nullptr);
}

std::streamsize FsServiceStreamBuf::WriteToFile(const char* s, std::streamsize n) {
    write_calls_++;

    const ssize_t rc = fs_write(&file_, s, static_cast<size_t>(n));
    const std::streamsize written = rc < 0 ? 0 : static_cast<std::streamsize>(rc);
    bytes_written_ += static_cast<uint64_t>(written);

    if(written != n)
        MarkWriteFailed();

    return written;
}

bool FsServiceStreamBuf::FlushPutArea() {
    const std::streamsize pending = pptr() - pbase();
    if(pending <= 0)
        return true;

    const char* data = pbase();
    // Reset either way: after a failed flush nothing is accepted any more, so the bytes would never leave.
    ResetPutArea();

    if(!file_opened_) {
        MarkWriteFailed();
        return false;
    }

    return WriteToFile(data, pending) == pending;
}

std::streamsize FsServiceStreamBuf::xsputn(const char* s, std::streamsize n) {
    if(!file_opened_ || write_failed_ || n <= 0)
        return 0;

    if(output_buffer_.empty())
        return WriteToFile(s, n);

    // A request that would fill the whole buffer anyway goes straight to the file, behind what is pending.
    if(static_cast<size_t>(n) >= output_buffer_.size()) {
        if(!FlushPutArea())
            return 0;

        return WriteToFile(s, n);
    }

    std::streamsize remaining = n;
    while(remaining > 0) {
        const std::streamsize chunk = std::min<std::streamsize>(remaining, epptr() - pptr());
        std::memcpy(pptr(), s + (n - remaining), static_cast<size_t>(chunk));
        pbump(static_cast<int>(chunk));
        remaining -= chunk;

        if(pptr() == epptr() && !FlushPutArea())
            return 0;
    }

    return n;
}

std::streambuf::int_type FsServiceStreamBuf::overflow(std::streambuf::int_type c) {
    if(c == traits_type::eof() || !file_opened_ || write_failed_)
        return traits_type::eof();

    const char ch = traits_type::to_char_type(c);

    if(output_buffer_.empty())
        return WriteToFile(&ch, 1) == 1 ? c : traits_type::eof();

    if(pptr() == epptr() && !FlushPutArea())
        return traits_type::eof();

    *pptr() = ch;
    pbump(1);

    return c;
}

std::streamsize FsServiceStreamBuf::xsgetn(char* s, std::streamsize n) {
    if(!file_opened_ || n <= 0)
        return 0;

    // Reads and writes share the file position, so pending output has to land first.
    if(!FlushPutArea())
        return 0;

    // Whatever underflow() already pulled in has to be handed out before reading the file again.
    const std::streamsize copied = std::min<std::streamsize>(egptr() - gptr(), n);
    if(copied > 0) {
        std::memcpy(s, gptr(), static_cast<size_t>(copied));
        gbump(static_cast<int>(copied));

        if(copied == n)
            return copied;
    }

    const ssize_t rc = fs_read(&file_, s + copied, static_cast<size_t>(n - copied));
    if(rc < 0)
        return copied;

    return copied + rc;
}

std::streambuf::int_type FsServiceStreamBuf::underflow() {
    if(!file_opened_ || input_buffer_.empty())
        return traits_type::eof();

    if(gptr() < egptr())
        return traits_type::to_int_type(*gptr());

    if(!FlushPutArea())
        return traits_type::eof();

    const ssize_t bytes_read = fs_read(&file_, input_buffer_.data(), input_buffer_.size());

    if(bytes_read <= 0)
        return traits_type::eof();

    setg(input_buffer_.data(), input_buffer_.data(), input_buffer_.data() + bytes_read);

    return traits_type::to_int_type(*gptr());
}

int FsServiceStreamBuf::sync() {
    // A latched stream has nothing pending, but what it dropped never reached the file, so a
    // sync of it cannot report success.
    if(!file_opened_ || write_failed_)
        return -1;

    if(!FlushPutArea())
        return -1;

    return fs_sync(&file_) == 0 ? 0 : -1;
}

std::streambuf::pos_type
FsServiceStreamBuf::seekoff(std::streambuf::off_type off, std::ios_base::seekdir way, std::ios_base::openmode which) {

    if(!file_opened_)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    // Reads and writes share the file position, so any of the two areas can be seeked.
    if((which & (std::ios_base::in | std::ios_base::out)) == 0)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    // Pending output belongs at the current position, so it goes out before the position moves.
    if(!FlushPutArea())
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    int whence = FS_SEEK_SET;
    std::streambuf::off_type target = off;

    if(way == std::ios_base::cur) {
        whence = FS_SEEK_CUR;
        // fs_tell() already counts the bytes sitting unread in the get area.
        target = off - static_cast<std::streambuf::off_type>(egptr() - gptr());
    } else if(way == std::ios_base::end) {
        whence = FS_SEEK_END;
    }

    const int rc = fs_seek(&file_, static_cast<off_t>(target), whence);
    if(rc != 0)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    const off_t pos = fs_tell(&file_);
    if(pos < 0)
        return std::streambuf::pos_type(std::streambuf::off_type(-1));

    setg(nullptr, nullptr, nullptr);
    ResetPutArea();

    return std::streambuf::pos_type(static_cast<std::streambuf::off_type>(pos));
}

std::streambuf::pos_type FsServiceStreamBuf::seekpos(std::streambuf::pos_type sp, std::ios_base::openmode which) {
    const auto off = static_cast<std::streambuf::off_type>(sp);
    return seekoff(off, std::ios_base::beg, which);
}

} // namespace eerie_leap::subsys::fs::services
