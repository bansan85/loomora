// SHA-256 (FIPS 180-4): the NIST vectors, split updates at every offset, unaligned input.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "format/sha256.hpp"

using namespace loomora::format;

namespace {

std::string hex_of(const std::string &s) { return sha256_hex(s.data(), s.size()); }

} // namespace

TEST_CASE("sha256 of the NIST short messages", "[sha256]") {
    CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("sha256 of one million a", "[sha256]") {
    Sha256 h;
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        h.update(block.data(), block.size());
    CHECK(h.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 gives the same digest for a split at every offset", "[sha256]") {
    std::string msg;
    for (int i = 0; i < 200; ++i)
        msg.push_back(static_cast<char>('A' + (i * 7) % 26));
    const std::string expected = hex_of(msg);
    for (std::size_t cut = 0; cut <= msg.size(); ++cut) {
        Sha256 h;
        h.update(msg.data(), cut);
        h.update(msg.data() + cut, msg.size() - cut);
        CHECK(h.hex_digest() == expected);
    }
    // Byte by byte, and with a zero-length update in the middle.
    Sha256 h;
    for (std::size_t i = 0; i < msg.size(); ++i) {
        h.update(msg.data() + i, 1);
        h.update(msg.data() + i, 0);
    }
    CHECK(h.hex_digest() == expected);
}

TEST_CASE("sha256 lengths around the padding boundaries", "[sha256]") {
    // Cross-check every length 0..130 between the one-shot and the byte-by-byte paths, and the
    // known digests of the boundary lengths 55, 56, 63, 64 of 'a'.
    CHECK(hex_of(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hex_of(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hex_of(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(hex_of(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    for (std::size_t n = 0; n <= 130; ++n) {
        const std::string s(n, 'x');
        Sha256 h;
        for (std::size_t i = 0; i < n; ++i)
            h.update(s.data() + i, 1);
        CHECK(h.hex_digest() == hex_of(s));
    }
}

TEST_CASE("sha256 reads input at any alignment", "[sha256]") {
    std::vector<unsigned char> buf(300 + 8);
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<unsigned char>(i * 31u + 5u);
    const std::string expected = sha256_hex(buf.data(), 300);
    for (std::size_t shift = 1; shift < 8; ++shift) {
        std::vector<unsigned char> moved(300 + 8);
        std::memcpy(moved.data() + shift, buf.data(), 300);
        CHECK(sha256_hex(moved.data() + shift, 300) == expected);
    }
}

TEST_CASE("sha256 digest bytes, reset and reuse", "[sha256]") {
    Sha256 h;
    h.update("abc", 3);
    std::uint8_t d[32];
    h.finish(d);
    CHECK(d[0] == 0xba);
    CHECK(d[1] == 0x78);
    CHECK(d[31] == 0xad);
    h.reset();
    CHECK(h.hex_digest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(to_hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
