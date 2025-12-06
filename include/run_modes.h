#pragma once

#include <filesystem>
#include <memory>

enum class Mode { Standalone, Server };
enum class AllocatorPolicy { Arena, Heap };

struct RunConfig {
  Mode mode = Mode::Standalone;
  std::filesystem::path inputFile = "data/fix_messages.txt";
  std::filesystem::path outputDir = "output";
  std::filesystem::path serverConfig = "config/app.properties";
};

class RunMode {
 public:
  explicit RunMode(const RunConfig& config) : config_(config) {}
  virtual ~RunMode() = default;
  virtual void run() const = 0;

 protected:
  const RunConfig& config_;
};

class StandaloneMode final : public RunMode {
 public:
  using RunMode::RunMode;
  void run() const override;
};

class ServerMode final : public RunMode {
 public:
  using RunMode::RunMode;
  void run() const override;
};

std::unique_ptr<RunMode> makeRunMode(const RunConfig& config);
