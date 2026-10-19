#include <streamcore/version.hpp>

#include <iostream>

int main() {
    std::cout << "StreamCore library call succeeded: version "
              << streamcore::version() << '\n';
    return 0;
}
