#pragma once

#pragma warning(push)
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <mmsystem.h>
#include <commdlg.h>
#pragma warning(pop)

using namespace std::literals;
using namespace std;

namespace logger = SKSE::log;

#define DLLEXPORT __declspec(dllexport)
