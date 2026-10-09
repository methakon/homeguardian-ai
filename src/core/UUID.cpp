#include "UUID.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace homeguardian {

std::string UUID::generate() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t high = dis(gen);
    uint64_t low = dis(gen);

    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << (high >> 32) << "-"
       << std::setw(4) << ((high >> 16) & 0xFFFF) << "-"
       << std::setw(4) << (high & 0xFFFF) << "-"
       << std::setw(4) << (low >> 48) << "-"
       << std::setw(12) << (low & 0xFFFFFFFFFFFFULL);

    return ss.str();
}

} // namespace homeguardian
