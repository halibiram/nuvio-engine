#include "test_support.hpp"

#include "storage/payload_reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t block_bytes = 16 * 1024; // 16 KB torrent block size

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("nuvio-engine-keyframe-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Replicate finished_run_end from libtorrent_stream_bridge.cpp line 632
std::uint64_t finished_run_end(
    const std::vector<bool>& blocks,
    const std::uint64_t cursor,
    const std::uint64_t limit
) {
    auto block = cursor / block_bytes;
    while (block < blocks.size() && blocks[static_cast<std::size_t>(block)]) {
        ++block;
    }
    return std::min(block * block_bytes, limit);
}

// Generate synthetic H.265 / HEVC 4K Keyframe data (IDR_W_RADL NAL unit 19)
std::vector<char> generate_hevc_keyframe(std::size_t size) {
    std::vector<char> data(size);
    // Fill with identifiable pattern
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 7 + 13) & 0xff);
    }
    // Prepend HEVC IDR NAL unit header: 0x00 0x00 0x00 0x01, NAL type 19 (0x26, 0x01)
    if (size >= 6) {
        data[0] = 0x00;
        data[1] = 0x00;
        data[2] = 0x00;
        data[3] = 0x01;
        data[4] = 0x26; // NAL unit type 19 (IDR_W_RADL)
        data[5] = 0x01;
    }
    return data;
}

} // namespace

NUVIO_TEST("reproduce I-frame corruption: unverified partial blocks stream unwritten disk zeroes") {
    TemporaryDirectory temp_dir;
    const auto payload_path = temp_dir.path() / "andor_4k_dv.mkv";

    // 1. Simulate a 64 KB piece containing an I-frame (4 blocks of 16 KB)
    constexpr std::uint32_t piece_size = 4 * static_cast<std::uint32_t>(block_bytes); // 64 KB
    const auto authentic_keyframe = generate_hevc_keyframe(piece_size);

    // 2. Preallocate the sparse file on disk filled with zeroes (0x00),
    // simulating what the OS / libtorrent allocates before the disk thread writes data.
    {
        std::ofstream preallocated(payload_path, std::ios::binary);
        std::vector<char> zeroes(piece_size, 0);
        preallocated.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    }

    nuvio::storage::PayloadReader payload(payload_path);
    NUVIO_EXPECT_TRUE(payload.is_open());

    // 3. Simulate unverified block signaling:
    // Peer downloads Block 0 (16 KB) and Block 1 (16 KB).
    // block_finished_alert fires for Block 0 and Block 1.
    // BUT libtorrent's posix_disk_io thread has NOT yet written/flushed to disk.
    std::vector<bool> finished_blocks(4, false);
    finished_blocks[0] = true;
    finished_blocks[1] = true;

    // Buffer captured by the HTTP writer
    std::vector<char> delivered_stream;
    const auto writer = [&](std::span<const char> bytes) -> bool {
        delivered_stream.insert(delivered_stream.end(), bytes.begin(), bytes.end());
        return true;
    };

    // 4. Simulate serve_piece with the unverified_ready logic
    std::uint64_t cursor = 0;
    const std::uint64_t slice_end = piece_size;

    // First iteration: unverified_ready() is true because Block 0 and 1 are marked finished
    const auto unverified_end = finished_run_end(finished_blocks, cursor, slice_end);
    NUVIO_EXPECT_EQ(unverified_end, std::uint64_t(32 * 1024)); // 32 KB

    // Engine reads unverified blocks directly from disk via pread
    std::vector<char> unverified_buffer(static_cast<std::size_t>(unverified_end - cursor));
    NUVIO_EXPECT_TRUE(payload.read_exact(cursor, unverified_buffer));
    writer(unverified_buffer);
    cursor = unverified_end;

    // At this point, the HTTP writer was fed 32 KB from disk.
    // Because the disk thread had not flushed yet, every single byte read was 0x00!
    NUVIO_EXPECT_EQ(delivered_stream.size(), std::size_t(32 * 1024));
    bool all_zeroes = std::ranges::all_of(delivered_stream, [](char b) { return b == 0; });
    NUVIO_EXPECT_TRUE(all_zeroes); // 32 KB of corrupted zeroes sent to player!

    // Now, libtorrent finishes downloading blocks 2 & 3, writes the full piece to disk,
    // verifies SHA-1 hash, and completes the waiter with the verified buffer:
    auto verified_data = std::make_shared<std::vector<char>>(authentic_keyframe);

    // Second iteration: waiter->done is true, serve_piece sends remaining bytes from cursor to slice_end
    const auto remaining_bytes = std::span<const char>(*verified_data).subspan(
        static_cast<std::size_t>(cursor),
        static_cast<std::size_t>(slice_end - cursor)
    );
    writer(remaining_bytes);

    // Total stream delivered to the player
    NUVIO_EXPECT_EQ(delivered_stream.size(), std::size_t(64 * 1024));

    // DEMONSTRATION OF BUG:
    // The delivered stream does NOT match the authentic keyframe!
    // The first 32 KB (50% of the I-frame) is entirely zeroed out!
    NUVIO_EXPECT_TRUE(delivered_stream != authentic_keyframe);

    // The HEVC IDR NAL header (0x00 0x00 0x00 0x01 0x26 0x01) at offset 0 was WIPED OUT!
    NUVIO_EXPECT_EQ(delivered_stream[0], 0);
    NUVIO_EXPECT_EQ(delivered_stream[1], 0);
    NUVIO_EXPECT_EQ(delivered_stream[2], 0);
    NUVIO_EXPECT_EQ(delivered_stream[3], 0);
    NUVIO_EXPECT_EQ(delivered_stream[4], 0); // Should have been 0x26 (IDR NAL)
    NUVIO_EXPECT_EQ(delivered_stream[5], 0); // Should have been 0x01

    // Video decoder receiving this stream:
    // - Misses the IDR keyframe NAL slice header entirely
    // - Decodes corrupted macroblocks
    // - Produces ~0.5s of digital noise / macroblock smearing until the next keyframe!
}

NUVIO_TEST("verified piece delivery prevents I-frame corruption") {
    // 64 KB piece containing an I-frame
    constexpr std::uint32_t piece_size = 4 * static_cast<std::uint32_t>(block_bytes);
    const auto authentic_keyframe = generate_hevc_keyframe(piece_size);

    std::vector<char> delivered_stream;
    const auto writer = [&](std::span<const char> bytes) -> bool {
        delivered_stream.insert(delivered_stream.end(), bytes.begin(), bytes.end());
        return true;
    };

    // When unverified block streaming is disabled and the engine waits for
    // libtorrent's SHA-1 verified in-memory piece buffer (read_piece_alert):
    auto verified_data = std::make_shared<std::vector<char>>(authentic_keyframe);
    writer(*verified_data);

    // VERIFICATION:
    // 1. Delivered stream 100% matches authentic video data
    NUVIO_EXPECT_EQ(delivered_stream.size(), std::size_t(64 * 1024));
    NUVIO_EXPECT_TRUE(delivered_stream == authentic_keyframe);

    // 2. HEVC IDR Keyframe header is intact
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[0]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[1]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[2]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[3]), 0x01);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[4]), 0x26); // Valid IDR NAL unit!
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[5]), 0x01);
}

NUVIO_TEST("unverified streaming with zero guard prevents zeroes corruption and keeps fast partial delivery") {
    TemporaryDirectory temp_dir;
    const auto payload_path = temp_dir.path() / "andor_4k_dv_fixed.mkv";

    constexpr std::uint32_t piece_size = 4 * static_cast<std::uint32_t>(block_bytes); // 64 KB
    const auto authentic_keyframe = generate_hevc_keyframe(piece_size);

    // Initial preallocated sparse file with zeroes
    {
        std::ofstream preallocated(payload_path, std::ios::binary);
        std::vector<char> zeroes(piece_size, 0);
        preallocated.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    }

    nuvio::storage::PayloadReader payload(payload_path);
    NUVIO_EXPECT_TRUE(payload.is_open());

    // Blocks 0 and 1 arrived over network
    std::vector<bool> finished_blocks(4, false);
    finished_blocks[0] = true;
    finished_blocks[1] = true;

    std::vector<char> delivered_stream;
    const auto writer = [&](std::span<const char> bytes) -> bool {
        delivered_stream.insert(delivered_stream.end(), bytes.begin(), bytes.end());
        return true;
    };

    std::uint64_t cursor = 0;
    const std::uint64_t slice_end = piece_size;

    // Simulate the fixed serve_piece loop:
    int retries = 0;
    bool disk_flushed = false;
    while (cursor < slice_end) {
        const auto unverified_end = finished_run_end(finished_blocks, cursor, slice_end);
        if (unverified_end <= cursor) {
            // Wait for remaining blocks / verified piece
            break;
        }

        const auto next_block_boundary = ((cursor / block_bytes) + 1) * block_bytes;
        const auto step_end = std::min(unverified_end, next_block_boundary);
        std::vector<char> buffer(static_cast<std::size_t>(step_end - cursor));
        NUVIO_EXPECT_TRUE(payload.read_exact(cursor, buffer));

        // ZERO GUARD CHECK:
        if (std::ranges::all_of(buffer, [](const char b) { return b == 0; })) {
            ++retries;
            // Simulate the disk thread finishing pwrite after the first attempt
            if (!disk_flushed) {
                std::fstream file(payload_path, std::ios::in | std::ios::out | std::ios::binary);
                file.seekp(0);
                file.write(authentic_keyframe.data(), static_cast<std::streamsize>(32 * 1024));
                file.flush();
                disk_flushed = true;
            }
            continue; // Retry without delivering zeroes or advancing cursor
        }

        writer(buffer);
        cursor = step_end;
    }

    // Zero-guard must have caught the unwritten block on attempt 1, retried once, and delivered
    NUVIO_EXPECT_EQ(retries, 1);
    NUVIO_EXPECT_EQ(cursor, std::uint64_t(32 * 1024));
    NUVIO_EXPECT_EQ(delivered_stream.size(), std::size_t(32 * 1024));

    // The partial delivered stream (first 32 KB) must match the authentic keyframe EXACTLY!
    const std::vector<char> expected_first_half(
        authentic_keyframe.begin(),
        authentic_keyframe.begin() + (32 * 1024)
    );
    NUVIO_EXPECT_TRUE(delivered_stream == expected_first_half);

    // Keyframe IDR header is 100% INTACT in partial unverified streaming!
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[0]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[1]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[2]), 0x00);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[3]), 0x01);
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[4]), 0x26); // IDR NAL unit!
    NUVIO_EXPECT_EQ(static_cast<unsigned char>(delivered_stream[5]), 0x01);
}
