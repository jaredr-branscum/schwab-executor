#include "schwab/process_runner.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace schwab {

ProcessResult ProcessRunner::execute(std::string_view command) {
    ProcessResult result;

    // Merge standard error (2>&1) so all output is captured
    std::string cmd = std::string(command) + " 2>&1";

#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) {
        result.exit_code = -1;
        result.output = "Error: Failed to spawn process pipe.";
        return result;
    }

    // 512-byte stack buffer to stream process output
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }

#if defined(_WIN32)
    result.exit_code = _pclose(pipe);
#else
    int status = pclose(pipe);
    result.exit_code = (status >= 0) ? status : -1;
#endif

    return result;
}

} // namespace schwab
