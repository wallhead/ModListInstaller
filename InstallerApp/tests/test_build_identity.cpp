#include "common/BuildIdentity.h"

#include <iostream>
#include <string_view>
#include <type_traits>

static_assert(std::is_same_v<decltype(modlist::BuildIdentity()), std::string_view>);
static_assert(noexcept(modlist::BuildIdentity()));

int main(int argc, char** argv) {
  auto id = modlist::BuildIdentity();
  std::cout << id << '\n';
  if (argc == 2) {
    return id == argv[1] ? 0 : 1;
  }
  if (id == "unavailable") {
    return 0;
  }
  if (id.ends_with("-dirty")) {
    id.remove_suffix(std::string_view("-dirty").size());
  }
  const bool valid = (id.size() == 40 || id.size() == 64) &&
                     id.find_first_not_of("0123456789abcdef") == std::string_view::npos;
  return valid ? 0 : 1;
}
