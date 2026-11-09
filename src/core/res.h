#pragma once
#include <string>

namespace core {
    // Query resource file content
    // name: filename or relative path (e.g. "io.json", "xxx/a.json")
    // returns: JSON string, nullptr if not found
    const char* get_res(const std::string& name);
}
