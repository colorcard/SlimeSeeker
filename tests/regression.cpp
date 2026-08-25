#include "slimeseeker/slimeseeker.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {
struct Expected { int64_t seed; const char *csv; };
constexpr std::array fixtures{
    Expected{0, "2046,-4323,46\n2047,-4323,46\n2046,-4324,46\n2047,-4324,46\n2046,-4327,46\n-9721,5188,46\n9421,9264,46\n2046,-4322,45\n2047,-4322,45\n-2288,-6444,45\n-2289,-6444,45\n3534,5863,45\n-6516,3925,45\n-4835,-7943,45\n-9633,-4999,45\n-9721,5189,45\n"},
    Expected{5023147298867078368LL, "-2456,-1598,46\n7701,2339,46\n9189,-2120,46\n9191,-2120,46\n9901,1600,46\n-1841,-729,45\n-2457,-1599,45\n4395,-2964,45\n-1458,6638,45\n-8244,146,45\n-8245,145,45\n9188,-2119,45\n9190,-2120,45\n"},
    Expected{-184718958561915LL, "-909,-9008,48\n-909,-9009,48\n8561,7912,48\n8563,7912,48\n8562,7911,47\n8561,7913,47\n8562,7912,47\n8565,7912,47\n1864,-1345,46\n3053,-174,46\n3055,-173,46\n1680,5067,46\n6668,-2956,46\n6669,-2957,46\n6670,-2956,46\n7016,-4929,46\n-909,-9010,46\n8564,7911,46\n8564,7912,46\n8564,7913,46\n-9325,-9724,46\n3053,-173,45\n3054,-173,45\n1713,-5158,45\n1712,-5159,45\n1713,-5159,45\n5857,-135,45\n6670,-2957,45\n7522,1405,45\n3697,-7615,45\n-5326,-7296,45\n-908,-9009,45\n-5676,-7630,45\n-5678,-7629,45\n-5677,-7631,45\n-6308,-9418,45\n8563,7910,45\n8563,7911,45\n8562,7913,45\n8565,7911,45\n8566,7912,45\n-9324,-9723,45\n"}
};

bool better(const ss_result &a, const ss_result &b) {
    if (a.count != b.count) return a.count > b.count;
    const int64_t da = static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.z) * a.z;
    const int64_t db = static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.z) * b.z;
    if (da != db) return da < db;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}
int collect(void *opaque, const ss_result *results, size_t count) {
    auto &out = *static_cast<std::vector<ss_result> *>(opaque);
    out.insert(out.end(), results, results + count); return 0;
}
}

int main() {
    for (const auto &fixture : fixtures) {
        std::vector<ss_result> results;
        ss_search_params_v1 params{sizeof(params), fixture.seed, -10000, 10000, -10000, 10000, 45, 0};
        ss_search_options_v1 options{sizeof(options), 0, SS_BACKEND_AUTO, 1024};
        ss_callbacks_v1 callbacks{sizeof(callbacks), &results, collect, nullptr, nullptr};
        if (ss_search(&params, &options, &callbacks) != SS_OK) return 1;
        std::sort(results.begin(), results.end(), better);
        std::string actual;
        char line[80];
        for (const auto &result : results) {
            std::snprintf(line, sizeof(line), "%d,%d,%u\n", result.x, result.z, result.count);
            actual += line;
        }
        if (actual != fixture.csv) {
            std::fprintf(stderr, "golden mismatch for seed %lld\n", static_cast<long long>(fixture.seed));
            return 1;
        }
    }
    std::puts("full golden regression passed");
    return 0;
}
