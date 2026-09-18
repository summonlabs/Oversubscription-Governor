// Oversubscription Governor — version identification.
#ifndef OVERSUB_VERSION_HPP
#define OVERSUB_VERSION_HPP

// CMake sets OVERSUB_VERSION_STRING from the project version. A fallback keeps
// the headers usable when they are consumed without the CMake package.
#ifndef OVERSUB_VERSION_STRING
#define OVERSUB_VERSION_STRING "1.0.0"
#endif

namespace oversub {

inline constexpr const char* kVersionString = OVERSUB_VERSION_STRING;

}  // namespace oversub

#endif  // OVERSUB_VERSION_HPP
