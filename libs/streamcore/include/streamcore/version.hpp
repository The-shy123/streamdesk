#pragma once

#include <string_view>

namespace streamcore {

// Returns the library version; the referenced text lives for the whole process.
std::string_view version() noexcept;

}  // namespace streamcore
