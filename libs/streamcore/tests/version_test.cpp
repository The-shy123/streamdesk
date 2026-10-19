#include <streamcore/version.hpp>

#include <iostream>
#include <string_view>

int main() {
    constexpr std::string_view expected = "0.1.0";
    const auto actual = streamcore::version();
    if (actual != expected) {
        std::cerr << "FAIL: expected version " << expected
                  << ", got " << actual << '\n';
        return 1;
    }

    std::cout << "PASS: StreamCore version is " << actual << '\n';
    return 0;
}
