#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace mine_teleop::detail {
// Fixed memory, no allocation/locking in the streaming callback. Values are
// integer milliseconds. Quantiles above the range are unknown, not clipped.
class LatencyHistogram {
 public:
  void add(std::uint64_t ms) {
    auto maximum=maximum_.load(std::memory_order_relaxed);
    while(ms>maximum && !maximum_.compare_exchange_weak(maximum,ms,std::memory_order_relaxed)) {}
    bins_[ms<=limit?static_cast<std::size_t>(ms):limit+1].fetch_add(1,std::memory_order_relaxed);
  }
  nlohmann::json json() const {
    std::array<std::uint64_t,limit+2> bins{};
    std::uint64_t count=0;
    for(std::size_t i=0;i<bins.size();++i) { bins[i]=bins_[i].load(std::memory_order_relaxed);count+=bins[i]; }
    const auto quantile=[&](unsigned percent)->nlohmann::json {
      if(!count)return nullptr;
      const auto rank=(count/100)*percent+((count%100)*percent+99)/100;
      std::uint64_t sum=0;
      for(std::size_t i=0;i<=limit;++i)if((sum+=bins[i])>=rank)return i;
      return nullptr;
    };
    return {{"count",count},{"p50_ms",quantile(50)},{"p95_ms",quantile(95)},{"p99_ms",quantile(99)},
        {"max_ms",count?nlohmann::json(maximum_.load(std::memory_order_relaxed)):nlohmann::json(nullptr)},
        {"overflow",bins.back()},{"resolution_ms",1},{"quantile_limit_ms",limit},
        {"snapshot", "concurrent counters; final stopped snapshot is exact"}};
  }
 private:
  static constexpr std::size_t limit=2000;
  std::array<std::atomic<std::uint64_t>,limit+2> bins_{};
  std::atomic<std::uint64_t> maximum_{0};
};
} // namespace mine_teleop::detail
