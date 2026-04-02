#pragma once

#include <dawn/webgpu_cpp.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace renderer_utils {

inline std::string loadFile(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::error("Failed to open: {}", path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string preprocessShader(const std::string& source,
                                    const std::filesystem::path& shaderDir,
                                    std::unordered_set<std::string>& visited)
{
    std::istringstream stream(source);
    std::ostringstream result;
    std::string line;

    while (std::getline(stream, line)) {
        auto pos = line.find("#include");
        if (pos != std::string::npos) {
            auto q1 = line.find('"', pos);
            auto q2 = line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string filename = line.substr(q1 + 1, q2 - q1 - 1);
                auto includePath = shaderDir / filename;
                auto canonical = includePath.string();

                if (visited.count(canonical)) {
                    result << "// [skipped circular include: " << filename << "]\n";
                    continue;
                }
                visited.insert(canonical);

                std::string included = loadFile(canonical.c_str());
                if (included.empty()) {
                    spdlog::error("Failed to include shader: {}", canonical);
                    continue;
                }
                result << preprocessShader(included, shaderDir, visited) << "\n";
                continue;
            }
        }
        result << line << "\n";
    }
    return result.str();
}

inline std::string loadShaderSource(const char* path)
{
    auto fsPath = std::filesystem::path(path);
    auto shaderDir = fsPath.parent_path();
    std::string source = loadFile(path);
    if (source.empty()) return {};

    if (source.find("#include") == std::string::npos) {
        return source;
    }

    std::unordered_set<std::string> visited;
    visited.insert(fsPath.string());
    return preprocessShader(source, shaderDir, visited);
}

inline wgpu::ShaderModule createShaderModule(wgpu::Device& device, const char* path)
{
    std::string source = loadShaderSource(path);
    if (source.empty()) return nullptr;

    wgpu::ShaderSourceWGSL wgslSource;
    wgslSource.code = {source.data(), source.size()};

    wgpu::ShaderModuleDescriptor desc;
    desc.nextInChain = &wgslSource;
    return device.CreateShaderModule(&desc);
}

} // namespace renderer_utils
