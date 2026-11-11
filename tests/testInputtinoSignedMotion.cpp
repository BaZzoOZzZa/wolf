#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <motion_encoding.hpp> // Actual private helper used by patched PS5Joypad::set_motion.

TEST_CASE("Inputtino signed motion encodes negative and boundary samples without devices", "[security][inputtino]") {
  using inputtino::detail::motion_to_le;
  struct Vector {
    float sample;
    double scale;
    uint16_t wire;
  };
  // Literal signed-16 wire expectations, including truncation toward zero.
  const Vector vectors[] = {{0, 1, 0x0000},
                            {-0.0f, 1, 0x0000},
                            {1, 1, 0x0001},
                            {-1, 1, 0xffff},
                            {-1.75f, 1, 0xffff},
                            {1.75f, 1, 0x0001},
                            {32767, 1, 0x7fff},
                            {-32768, 1, 0x8000},
                            {32768, 1, 0x7fff},
                            {-32769, 1, 0x8000},
                            {1, 1145, 0x0479},
                            {-1, 1145, 0xfb87},
                            {1, static_cast<double>(9.80665f) * 100, 0x03d4},
                            {-1, static_cast<double>(9.80665f) * 100, 0xfc2c}};
  for (auto vector : vectors) {
    CAPTURE(vector.sample, vector.scale);
    REQUIRE(le16toh(motion_to_le(vector.sample, vector.scale)) == vector.wire);
  }
  for (double scale : {1.0, 1145.0, static_cast<double>(9.80665f) * 100}) {
    REQUIRE(le16toh(motion_to_le(std::numeric_limits<float>::max(), scale)) == 0x7fff);
    REQUIRE(le16toh(motion_to_le(std::numeric_limits<float>::lowest(), scale)) == 0x8000);
    REQUIRE(le16toh(motion_to_le(std::numeric_limits<float>::min(), scale)) == 0);
    REQUIRE(le16toh(motion_to_le(std::numeric_limits<float>::denorm_min(), scale)) == 0);
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
      REQUIRE(le16toh(motion_to_le(bad, scale)) == 0); // Explicit invalid-sample policy.
    }
  }
}
