#pragma once
// Raw environment-variable read as a UTF-8 string. Var names are ASCII.
// Keeps <windows.h> confined to kuli-platform (R-NF-05).
#include <optional>
#include <string>
#include <string_view>

namespace kuli::platform {

std::optional<std::string> get_env(std::string_view name);

}  // namespace kuli::platform
