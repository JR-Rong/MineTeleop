#include "mine_teleop/detail/isolated_mock_bench.hpp"
#include <iostream>
#include <stdexcept>
using namespace mine_teleop::detail;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main() {
  try {
    require(isolated_mock_bench("test_bench_001", "mock", 0.0, "bench"), "isolated bench rejected");
    require(!isolated_mock_bench("vehicle-001", "mock", 0.0, "bench"), "non-bench id accepted");
    require(!isolated_mock_bench("test_bench_001", "can", 0.0, "bench"), "real adapter accepted");
    require(!isolated_mock_bench("test_bench_001", "mock", 0.1, "bench"), "non-zero speed accepted");
    require(!isolated_mock_bench("test_bench_001", "mock", 0.0, "field"), "field commissioning accepted");
    require(isolated_mock_bench_offer({{"isolated_mock_bench_capability", "isolated-mock-bench-v1"}}), "attested offer rejected");
    require(!isolated_mock_bench_offer({{"isolated_mock_bench_capability", ""}}), "empty attestation accepted");
    require(!isolated_mock_bench_offer({{"isolated_mock_bench_capability", 1}}), "malformed attestation accepted");
    require(!isolated_mock_bench_offer(nlohmann::json::object()), "missing attestation accepted");
    // Per-command bench tracing needs all three at once; the compile switch alone can turn it off for a release build.
    for (int bits = 0; bits < 8; ++bits) {
      const bool compiled = bits & 1, requested = bits & 2, attested = bits & 4;
      require(isolated_mock_trace_enabled(compiled, requested, attested) == (compiled && requested && attested),
              "bench trace is not exactly compiled && requested && attested");
    }
    require(!isolated_mock_trace_enabled(false, true, true), "a release build traced a bench vehicle");
    std::cout << "PASS isolated mock bench attestation\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
