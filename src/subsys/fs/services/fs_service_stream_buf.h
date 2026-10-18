#pragma once

#include <cstdint>
#include <streambuf>
#include <string>
#include <vector>

#include "i_fs_service.h"

namespace eerie_leap::subsys::fs::services {

class FsServiceStreamBuf : public std::streambuf {
private:
    IFsService* fs_service_;
    struct fs_file_t file_;
    bool file_opened_{ false };

    static constexpr size_t PATH_BUFFER_SIZE = 256;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<char> input_buffer_;

    // Put area of the Write and Append modes (CONFIG_EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE bytes,
    // 0 = every write goes straight to the file), flushed on sync, seek, read, close and overflow.
    // A flush that fails or falls short latches write_failed_: every later write is refused, so the
    // caller sees one failure instead of a file with a hole in it.
    std::vector<char> output_buffer_;
    bool write_failed_ = false;
    uint64_t bytes_written_ = 0; // handed to fs_write(); what is still in the put area is added on read
    uint32_t write_calls_ = 0;

    void ResetPutArea();
    void MarkWriteFailed();
    bool FlushPutArea();
    std::streamsize WriteToFile(const char* s, std::streamsize n);

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    std::streambuf::int_type overflow(std::streambuf::int_type c) override;
    std::streamsize xsgetn(char* s, std::streamsize n) override;
    std::streambuf::int_type underflow() override;
    int sync() override;
    std::streambuf::pos_type
    seekoff(std::streambuf::off_type off, std::ios_base::seekdir way, std::ios_base::openmode which) override;
    std::streambuf::pos_type seekpos(std::streambuf::pos_type sp, std::ios_base::openmode which) override;

public:
    enum class OpenMode {
        Read,
        Write,
        Append
    };

    FsServiceStreamBuf(IFsService* fs_service, const std::string_view relative_path, OpenMode mode);
    ~FsServiceStreamBuf() override;

    /** Flushes the put area and closes the file; true only when both succeeded. */
    bool close();
    bool is_open() const;

    /** Bytes accepted by writes so far, whether still in the put area or already in the file. */
    [[nodiscard]] uint64_t GetBytesWritten() const {
        return bytes_written_ + static_cast<uint64_t>(pptr() - pbase());
    }

    /** Number of fs_write() calls issued so far: one per flush of the put area or direct write. */
    [[nodiscard]] uint32_t GetWriteCallCount() const {
        return write_calls_;
    }

    FsServiceStreamBuf(const FsServiceStreamBuf&) = delete;
    FsServiceStreamBuf& operator=(const FsServiceStreamBuf&) = delete;
};

} // namespace eerie_leap::subsys::fs::services
