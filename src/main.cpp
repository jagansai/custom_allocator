#include "run_modes.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

[[noreturn]] void printUsageAndExit() {
  std::cout << "Usage: fix_allocator_demo [--mode standalone|server]"
            << " [--file <path>] [--output-dir <path>] [--server-config <path>]\n";
  std::exit(0);
}

static std::string toLower(std::string_view text) {
  std::string lowered{text};
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

static RunConfig parseArguments(int argc, char** argv) {
  RunConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      printUsageAndExit();
    }
    std::string key;
    std::string value;
    const auto equalPos = arg.find('=');
    if (equalPos != std::string_view::npos) {
      key = std::string(arg.substr(0, equalPos));
      value = std::string(arg.substr(equalPos + 1));
    } else {
      key = std::string(arg);
      if (key == "--mode" || key == "--file" || key == "--output-dir" ||
          key == "--server-config") {
        if (++i >= argc) {
          throw std::runtime_error("missing value for " + key);
        }
        value = argv[i];
      }
    }
    if (key == "--mode") {
      const auto lowered = toLower(value);
      if (lowered == "server") {
        config.mode = Mode::Server;
      } else if (lowered == "standalone") {
        config.mode = Mode::Standalone;
      } else {
        throw std::runtime_error("unknown mode: " + value);
      }
    } else if (key == "--file") {
      config.inputFile = value;
    } else if (key == "--output-dir") {
      config.outputDir = value;
    } else if (key == "--server-config") {
      config.serverConfig = value;
    } else {
      throw std::runtime_error("unknown argument: " + key);
    }
  }
  return config;
}

int main(int argc, char** argv) {
  try {
    const auto config = parseArguments(argc, argv);
    std::filesystem::create_directories(config.outputDir);
    
    const auto mode = makeRunMode(config);
    mode->run();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}
