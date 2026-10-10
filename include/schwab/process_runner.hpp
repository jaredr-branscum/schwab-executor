#pragma once

#include <string>
#include <string_view>

namespace schwab {

/**
 * @brief Result of an executed external process.
 */
struct ProcessResult {
    int exit_code = -1; // 0 denotes success; non-zero denotes error
    std::string output; // Captured stdout
};

/**
 * @brief Utility for executing external system commands and capturing their output.
 */
class ProcessRunner {
public:
    /**
     * @brief Executes shell command synchronously and captures its exit code and stdout.
     * @param command Executed command line (e.g. "python --version").
     * @return ProcessResult containing the integer exit code and captured output.
     */
    static ProcessResult execute(std::string_view command);
};

} // namespace schwab
