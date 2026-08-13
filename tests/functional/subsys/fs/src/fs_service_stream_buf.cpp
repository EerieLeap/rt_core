#include <array>
#include <ios>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <zephyr/ztest.h>

#include "subsys/fs/services/fs_service_stream_buf.h"

#include "fs_test_support.h"

using eerie_leap::subsys::fs::services::FsServiceStreamBuf;

using fs_test::GetFsService;
using fs_test::MakeText;
using fs_test::ReadText;
using fs_test::ResetFs;
using fs_test::WriteText;

using OpenMode = FsServiceStreamBuf::OpenMode;

ZTEST_SUITE(fs_service_stream_buf, NULL, NULL, ResetFs, ResetFs, NULL);

// Any other exception type propagates so an unexpected failure is not mistaken for the expected one.
template<typename Exception>
static bool Throws(auto&& action) {
    try {
        action();
    } catch(const Exception&) {
        return true;
    }

    return false;
}

ZTEST(fs_service_stream_buf, test_constructor_rejects_null_service) {
    zassert_true(Throws<std::invalid_argument>([] {
        FsServiceStreamBuf stream_buf(nullptr, "test_null_service.txt", OpenMode::Write);
    }));
}

ZTEST(fs_service_stream_buf, test_constructor_fails_for_missing_file) {
    zassert_true(Throws<std::system_error>([] {
        FsServiceStreamBuf stream_buf(GetFsService(), "test_missing_file.txt", OpenMode::Read);
    }));
}

ZTEST(fs_service_stream_buf, test_constructor_rejects_paths_longer_than_the_buffer) {
    zassert_true(Throws<std::invalid_argument>([] {
        FsServiceStreamBuf stream_buf(GetFsService(), std::string(300, 'a'), OpenMode::Write);
    }));
}

ZTEST(fs_service_stream_buf, test_constructor_rejects_traversal) {
    zassert_true(Throws<std::system_error>([] {
        FsServiceStreamBuf stream_buf(GetFsService(), "../test_traversal.txt", OpenMode::Write);
    }));
}

// The parent cannot be created when a file already occupies part of its path.
ZTEST(fs_service_stream_buf, test_constructor_reports_unusable_parent_directory) {
    zassert_true(WriteText("test_parent_is_a_file", "abc"));

    zassert_true(Throws<std::system_error>([] {
        FsServiceStreamBuf stream_buf(GetFsService(), "test_parent_is_a_file/sub/log.txt", OpenMode::Write);
    }));
}

ZTEST(fs_service_stream_buf, test_close_is_idempotent) {
    FsServiceStreamBuf stream_buf(GetFsService(), "test_close.txt", OpenMode::Write);

    zassert_true(stream_buf.is_open());
    zassert_true(stream_buf.close());
    zassert_false(stream_buf.is_open());
    zassert_true(stream_buf.close());
}

ZTEST(fs_service_stream_buf, test_write_creates_missing_parent_directories) {
    auto* fs_service = GetFsService();

    {
        FsServiceStreamBuf stream_buf(fs_service, "test_logs/2026/run.txt", OpenMode::Write);
        std::ostream stream(&stream_buf);

        stream << "entry";
    }

    zassert_true(fs_service->Exists("test_logs/2026"));
    zassert_str_equal(ReadText("test_logs/2026/run.txt").c_str(), "entry");
}

ZTEST(fs_service_stream_buf, test_destructor_closes_the_file) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_destructor.txt";

    {
        FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);
        std::ostream stream(&stream_buf);

        stream << "written";
    }

    zassert_equal(fs_service->GetFileSize(file_path).value(), 7);
    zassert_str_equal(ReadText(file_path).c_str(), "written");
}

ZTEST(fs_service_stream_buf, test_write_mode_truncates_existing_file) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_truncate.txt";

    zassert_true(WriteText(file_path, "0123456789"));

    {
        FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);
        std::ostream stream(&stream_buf);

        stream << "abc";
    }

    zassert_equal(fs_service->GetFileSize(file_path).value(), 3);
    zassert_str_equal(ReadText(file_path).c_str(), "abc");
}

ZTEST(fs_service_stream_buf, test_append_mode_keeps_existing_content) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_append.txt";

    zassert_true(WriteText(file_path, "abc"));

    {
        FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Append);
        std::ostream stream(&stream_buf);

        stream << "def";
    }

    zassert_str_equal(ReadText(file_path).c_str(), "abcdef");
}

ZTEST(fs_service_stream_buf, test_overflow_writes_single_character) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_overflow.txt";

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(stream_buf.sputc('x'), 'x');
    zassert_true(stream_buf.close());

    zassert_str_equal(ReadText(file_path).c_str(), "x");
}

ZTEST(fs_service_stream_buf, test_sync_flushes_written_data) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_sync.txt";

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);
    std::ostream stream(&stream_buf);

    stream << "abc";

    zassert_equal(stream_buf.pubsync(), 0);
    zassert_equal(fs_service->GetFileSize(file_path).value(), 3);
}

ZTEST(fs_service_stream_buf, test_read_returns_whole_file) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_read.bin";

    // Larger than the internal read buffer so a single request cannot be served from it.
    auto content = MakeText(10000);
    zassert_true(WriteText(file_path, content));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);
    std::istream stream(&stream_buf);

    std::string read_back(content.size(), '\0');
    stream.read(read_back.data(), static_cast<std::streamsize>(read_back.size()));

    zassert_equal(static_cast<size_t>(stream.gcount()), content.size());
    zassert_true(read_back == content);
}

ZTEST(fs_service_stream_buf, test_underflow_refills_the_read_buffer) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_underflow.bin";

    auto content = MakeText(9000);
    zassert_true(WriteText(file_path, content));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);
    std::istream stream(&stream_buf);

    std::string read_back;
    read_back.reserve(content.size());

    char character = 0;
    while(stream.get(character))
        read_back.push_back(character);

    zassert_true(stream.eof());
    zassert_true(read_back == content);
}

// Mirrors how the MDF writer patches the header once the payload length is known.
ZTEST(fs_service_stream_buf, test_seek_rewrites_beginning_of_file) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_seek_rewrite.txt";

    {
        FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);
        std::ostream stream(&stream_buf);

        stream << "0000567";
        stream.seekp(0);
        stream << "1234";

        zassert_true(stream.good());
    }

    zassert_str_equal(ReadText(file_path).c_str(), "1234567");
}

ZTEST(fs_service_stream_buf, test_seek_from_end_reports_file_size) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_seek_end.txt";

    zassert_true(WriteText(file_path, "0123456789"));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);

    auto end_position = stream_buf.pubseekoff(0, std::ios_base::end, std::ios_base::in);
    zassert_equal(static_cast<std::streamoff>(end_position), 10);

    auto tail_position = stream_buf.pubseekoff(-4, std::ios_base::end, std::ios_base::in);
    zassert_equal(static_cast<std::streamoff>(tail_position), 6);

    std::array<char, 4> tail = {};
    zassert_equal(stream_buf.sgetn(tail.data(), tail.size()), 4);
    zassert_mem_equal(tail.data(), "6789", tail.size());
}

ZTEST(fs_service_stream_buf, test_seek_to_position_restarts_reading) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_seek_pos.txt";

    zassert_true(WriteText(file_path, "0123456789"));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);

    zassert_equal(stream_buf.sbumpc(), '0');

    auto position = stream_buf.pubseekpos(4, std::ios_base::in);
    zassert_equal(static_cast<std::streamoff>(position), 4);
    zassert_equal(stream_buf.sbumpc(), '4');
}

// The buffer filled by underflow() must not be skipped by the bulk read path.
ZTEST(fs_service_stream_buf, test_buffered_and_bulk_reads_stay_in_sync) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_mixed_reads.bin";

    auto content = MakeText(6000);
    zassert_true(WriteText(file_path, content));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);
    std::istream stream(&stream_buf);

    char first = 0;
    zassert_true(static_cast<bool>(stream.get(first)));
    zassert_equal(first, content[0]);

    std::string read_back(content.size() - 1, '\0');
    stream.read(read_back.data(), static_cast<std::streamsize>(read_back.size()));

    zassert_equal(static_cast<size_t>(stream.gcount()), read_back.size());
    zassert_true(read_back == content.substr(1));
}

// The reported position has to account for the bytes still sitting in the read buffer.
// std::istream::seekg is not linkable on every supported platform, hence the streambuf API.
ZTEST(fs_service_stream_buf, test_seek_from_current_accounts_for_the_read_buffer) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_seek_cur.bin";

    auto content = MakeText(6000);
    zassert_true(WriteText(file_path, content));

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Read);

    zassert_equal(
        static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::cur, std::ios_base::in)), 0);

    // Reading a single character makes underflow() pull in a whole buffer.
    for(int i = 0; i < 10; ++i)
        stream_buf.sbumpc();

    zassert_equal(
        static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::cur, std::ios_base::in)), 10);
    zassert_equal(
        static_cast<std::streamoff>(stream_buf.pubseekoff(5, std::ios_base::cur, std::ios_base::in)), 15);
    zassert_equal(stream_buf.sbumpc(), content[15]);
}

ZTEST(fs_service_stream_buf, test_operations_fail_after_close) {
    auto* fs_service = GetFsService();

    FsServiceStreamBuf stream_buf(fs_service, "test_closed.txt", OpenMode::Write);
    zassert_true(stream_buf.close());

    zassert_equal(stream_buf.sputn("abc", 3), 0);
    zassert_equal(stream_buf.pubsync(), -1);
    zassert_equal(
        static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::beg, std::ios_base::out)), -1);
}
