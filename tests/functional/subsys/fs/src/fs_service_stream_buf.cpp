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

    zassert_equal(static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::cur, std::ios_base::in)), 0);

    // Reading a single character makes underflow() pull in a whole buffer.
    for(int i = 0; i < 10; ++i)
        stream_buf.sbumpc();

    zassert_equal(static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::cur, std::ios_base::in)), 10);
    zassert_equal(static_cast<std::streamoff>(stream_buf.pubseekoff(5, std::ios_base::cur, std::ios_base::in)), 15);
    zassert_equal(stream_buf.sbumpc(), content[15]);
}

ZTEST(fs_service_stream_buf, test_operations_fail_after_close) {
    auto* fs_service = GetFsService();

    FsServiceStreamBuf stream_buf(fs_service, "test_closed.txt", OpenMode::Write);
    zassert_true(stream_buf.close(), "closing an untouched stream must succeed");

    zassert_equal(stream_buf.sputn("abc", 3), 0, "a closed stream must refuse sputn");
    zassert_equal(
        stream_buf.sputc('x'), std::streambuf::traits_type::eof(),
        "a closed stream must refuse sputc, not store it in a put area nobody flushes"
    );
    zassert_equal(stream_buf.GetBytesWritten(), 0, "refused bytes must not be counted");
    zassert_equal(stream_buf.pubsync(), -1, "a closed stream cannot sync");
    zassert_equal(static_cast<std::streamoff>(stream_buf.pubseekoff(0, std::ios_base::beg, std::ios_base::out)), -1);
}

// The tests below exercise the put area, so they assume the default (non-zero) buffer size.
static constexpr size_t kWriteBufferSize = CONFIG_EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE;

ZTEST(fs_service_stream_buf, test_buffered_writes_reach_the_file_on_sync) {
    if(kWriteBufferSize == 0)
        ztest_test_skip();

    auto* fs_service = GetFsService();
    std::string file_path = "test_buffered_sync.bin";

    auto content = MakeText(100);

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(stream_buf.sputn(content.data(), 100), 100, "a write that fits the buffer is accepted whole");
    zassert_equal(stream_buf.GetBytesWritten(), 100, "buffered bytes count as written");
    zassert_equal(stream_buf.GetWriteCallCount(), 0, "a write that fits the buffer must not reach the file system");
    zassert_equal(fs_service->GetFileSize(file_path).value(), 0, "nothing is in the file before the sync");

    zassert_equal(stream_buf.pubsync(), 0, "a sync with room on the volume must succeed");
    zassert_equal(stream_buf.GetWriteCallCount(), 1, "the sync flushes the put area in one write");
    zassert_equal(fs_service->GetFileSize(file_path).value(), 100, "the sync must put the buffered bytes in the file");

    zassert_true(stream_buf.close(), "close must report the flush and the close");
    zassert_equal(stream_buf.GetWriteCallCount(), 1, "an empty put area must not be written at close");
    zassert_true(ReadText(file_path) == content);
}

ZTEST(fs_service_stream_buf, test_close_flushes_the_put_area) {
    if(kWriteBufferSize == 0)
        ztest_test_skip();

    auto* fs_service = GetFsService();
    std::string file_path = "test_close_flush.txt";

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(stream_buf.sputn("buffered", 8), 8, "a write that fits the buffer is accepted whole");
    zassert_equal(stream_buf.GetWriteCallCount(), 0, "a write that fits the buffer must not reach the file system");

    zassert_true(stream_buf.close(), "close must report the flush and the close");
    zassert_equal(stream_buf.GetWriteCallCount(), 1, "close flushes the put area in one write");
    zassert_str_equal(ReadText(file_path).c_str(), "buffered");
}

ZTEST(fs_service_stream_buf, test_write_larger_than_the_buffer_is_written_directly) {
    if(kWriteBufferSize == 0)
        ztest_test_skip();

    auto* fs_service = GetFsService();
    std::string file_path = "test_direct_write.bin";

    auto head = MakeText(10);
    auto body = MakeText(kWriteBufferSize + 1);

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(stream_buf.sputn(head.data(), static_cast<std::streamsize>(head.size())), 10);
    zassert_equal(stream_buf.GetWriteCallCount(), 0, "the head fits the buffer");

    zassert_equal(
        static_cast<size_t>(stream_buf.sputn(body.data(), static_cast<std::streamsize>(body.size()))), body.size()
    );
    zassert_equal(
        stream_buf.GetWriteCallCount(), 2,
        "a write of at least the buffer size flushes the pending head, then goes to the file directly"
    );
    zassert_equal(stream_buf.GetBytesWritten(), head.size() + body.size(), "both writes count as written");

    zassert_true(stream_buf.close(), "close must report the flush and the close");
    zassert_equal(stream_buf.GetWriteCallCount(), 2, "nothing was left to flush at close");
    zassert_true(ReadText(file_path) == head + body, "the head must precede the body in the file");
}

ZTEST(fs_service_stream_buf, test_seek_flushes_before_moving) {
    if(kWriteBufferSize == 0)
        ztest_test_skip();

    auto* fs_service = GetFsService();
    std::string file_path = "test_seek_flush.bin";

    auto content = MakeText(1000);
    const std::string patch = "ABCDEFGH";

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(
        stream_buf.sputn(content.data(), static_cast<std::streamsize>(content.size())), 1000,
        "a write that fits the buffer is accepted whole"
    );
    zassert_equal(stream_buf.GetWriteCallCount(), 0, "a write that fits the buffer must not reach the file system");

    zassert_equal(static_cast<std::streamoff>(stream_buf.pubseekpos(0, std::ios_base::out)), 0);
    zassert_equal(stream_buf.GetWriteCallCount(), 1, "the pending bytes must be written before the position moves");

    zassert_equal(stream_buf.sputn(patch.data(), 8), 8, "the patch fits the buffer");
    zassert_true(stream_buf.close(), "close must report the flush and the close");
    zassert_equal(stream_buf.GetWriteCallCount(), 2, "close flushes the patch in one write");

    auto expected = content;
    expected.replace(0, patch.size(), patch);

    auto read_back = ReadText(file_path);
    zassert_equal(read_back.size(), expected.size(), "the rewrite must not change the file size");
    zassert_true(read_back == expected, "the patch must land at the seeked position, after the earlier bytes");
}

// The buffer size is a Kconfig symbol, so it cannot be varied at run time: this test asserts the
// behaviour the build configured. The functional.subsys.fs.unbuffered scenario builds the suite
// with a 0 buffer, so both branches run under twister.
ZTEST(fs_service_stream_buf, test_unbuffered_when_the_buffer_size_is_zero) {
    auto* fs_service = GetFsService();
    std::string file_path = "test_unbuffered.txt";

    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    zassert_equal(stream_buf.sputn("abc", 3), 3, "sputn must accept the bytes whether buffered or not");
    zassert_equal(stream_buf.sputc('d'), 'd', "sputc must accept the byte whether buffered or not");
    zassert_equal(stream_buf.GetBytesWritten(), 4, "every accepted byte counts as written");

    if(kWriteBufferSize == 0)
        zassert_equal(stream_buf.GetWriteCallCount(), 2, "without a buffer every write goes straight to the file");
    else
        zassert_equal(stream_buf.GetWriteCallCount(), 0, "with a buffer nothing reaches the file before a flush");

    zassert_true(stream_buf.close(), "close must report the flush and the close");
    zassert_str_equal(ReadText(file_path).c_str(), "abcd");
}

// Fills the volume with a separate file until a write fails, then checks that a buffered stream
// reports the write it cannot complete instead of crashing or pretending.
ZTEST(fs_service_stream_buf, test_short_write_on_a_full_volume_is_reported) {
    if(kWriteBufferSize == 0)
        ztest_test_skip();

    auto* fs_service = GetFsService();
    std::string file_path = "test_full_volume.bin";
    std::string filler_path = "test_full_volume_filler.bin";

    // Opened before the fill so the open itself cannot fail for lack of space.
    FsServiceStreamBuf stream_buf(fs_service, file_path, OpenMode::Write);

    auto chunk = MakeText(4096);
    int appends = 0;
    while(appends < 4096 && WriteText(filler_path, chunk, true))
        appends++;
    zassert_true(appends > 0 && appends < 4096, "the volume did not fill up");

    // Fits the buffer, so the failure can only surface when the stream tries to flush.
    zassert_equal(stream_buf.sputn(chunk.data(), 100), 100, "a write that fits the buffer is accepted");
    bool reported = stream_buf.pubsync() != 0;

    // The filler's failed append may have left a block or two behind; direct writes through the
    // stream take them, and the one that does not fit has to come back short.
    int direct_writes = 0;
    while(!reported && direct_writes < 512) {
        const auto written = stream_buf.sputn(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        reported = static_cast<size_t>(written) < chunk.size();
        direct_writes++;
    }
    zassert_true(reported, "a write on a full volume must be reported by a failed sync or a short write");

    zassert_equal(stream_buf.sputn("x", 1), 0, "once a write failed every later write must be refused");
    zassert_equal(
        stream_buf.sputc('x'), std::streambuf::traits_type::eof(), "once a write failed sputc must be refused as well"
    );
    zassert_equal(stream_buf.pubsync(), -1, "a sync of a latched stream must not report success");

    zassert_true(stream_buf.close(), "the file still closes once the failed bytes were dropped");
    zassert_false(stream_buf.is_open());

    if(!fs_service->DeleteFile(filler_path))
        zassert_true(fs_service->Format(), "the filler could neither be deleted nor formatted away");
}
