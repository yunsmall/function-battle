// SHA-256 单测：FIPS 180-4 标准示例向量（空串 / abc / 56 字节跨块填充）
#include <gtest/gtest.h>

#include "sha256.h"

TEST(Sha256, FipsVectors) {
    EXPECT_EQ(fg::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(fg::sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 字节：填充把 0x80 顶出第一块，覆盖"跨块填充"路径
    EXPECT_EQ(fg::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
