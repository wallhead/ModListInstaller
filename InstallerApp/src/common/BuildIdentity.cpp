#include "common/BuildIdentity.h"

#if __has_include("ModlistBuildIdentity.generated.h")
#include "ModlistBuildIdentity.generated.h"
#endif

std::string_view modlist::BuildIdentity() noexcept {
#ifdef MODLIST_BUILD_ID
  return MODLIST_BUILD_ID;
#else
  return "unavailable";
#endif
}
