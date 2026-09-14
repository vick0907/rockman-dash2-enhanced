#pragma once
#include <sstream>
#include <string>

void WriteLocalLog(const char* level, const std::string& message);

namespace spdlog {
template<typename... Arguments>
void Write(const char* level, const char* message, const Arguments&... arguments) {
    std::ostringstream output;
    output << message;
    ((output << " | " << arguments), ...);
    WriteLocalLog(level, output.str());
}
template<typename... Arguments>
void trace(const char* message, const Arguments&... arguments) { Write("trace", message, arguments...); }
template<typename... Arguments>
void info(const char* message, const Arguments&... arguments) { Write("info", message, arguments...); }
template<typename... Arguments>
void error(const char* message, const Arguments&... arguments) { Write("error", message, arguments...); }
}