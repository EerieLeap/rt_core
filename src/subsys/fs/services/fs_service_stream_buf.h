#pragma once

#include <streambuf>
#include <string>

#include "i_fs_service.h"

namespace eerie_leap::subsys::fs::services {

class FsServiceStreamBuf : public std::streambuf {
private:
    IFsService* fs_service_;
    struct fs_file_t file_;
    bool file_opened_;

    static constexpr size_t PATH_BUFFER_SIZE = 256;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<char> input_buffer_;

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    std::streambuf::int_type overflow(std::streambuf::int_type c) override;
    std::streamsize xsgetn(char* s, std::streamsize n) override;
    std::streambuf::int_type underflow() override;
    int sync() override;
    std::streambuf::pos_type seekoff(
        std::streambuf::off_type off,
        std::ios_base::seekdir way,
        std::ios_base::openmode which) override;
    std::streambuf::pos_type seekpos(std::streambuf::pos_type sp, std::ios_base::openmode which) override;

public:
    enum class OpenMode {
        Read,
        Write,
        Append
    };

    FsServiceStreamBuf(IFsService* fs_service, const std::string_view relative_path, OpenMode mode);
    ~FsServiceStreamBuf();

    bool close();
    bool is_open() const;

    FsServiceStreamBuf(const FsServiceStreamBuf&) = delete;
    FsServiceStreamBuf& operator=(const FsServiceStreamBuf&) = delete;
};

} // namespace eerie_leap::subsys::fs::services
