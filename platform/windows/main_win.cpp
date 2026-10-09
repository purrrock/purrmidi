#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <csignal>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

#include "midi_input.h"
#include "midi_decode.h"
#include "audio_out.h"
#include "wav_recorder.h"
#include "synth_engine.h"
#include "midi_dispatch.h"

extern "C" {
#include "midi_queue.h"
}

using namespace purrmidi;

static std::atomic<bool> g_running{true};
static std::mutex g_console_mutex;

#ifdef _WIN32
static BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
static void SignalHandler(int signal) {
    (void)signal;
    g_running.store(false);
}
#endif

struct AppConfig {
    enum class Mode {
        Default,
        List,
        Monitor,
        Play
    };

    Mode mode{Mode::Default};
    int midi_port{-1};
    bool show_realtime{false};
    uint32_t buffer_frames{256};
    float gain{1.0f};
    std::string wav_filename;
    uint32_t wav_seconds{120};
    bool verbose{false};
};

static void PrintUsage(const char* prog_name) {
    std::cout << "PurrMidi Windows Host Synth & MIDI Monitor\n"
              << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  --list                      List available MIDI input ports and audio devices\n"
              << "  --monitor                   MIDI monitor mode (no audio output)\n"
              << "  --play                      Synthesizer player mode (audio output enabled)\n"
              << "  --midi-port N               MIDI input port index\n"
              << "  --show-realtime             Show MIDI realtime messages (Clock 0xF8, ActiveSensing 0xFE)\n"
              << "  --buffer-frames N           Audio buffer period size in frames (default: 256)\n"
              << "  --gain G                    Output audio gain factor (default: 1.0)\n"
              << "  --wav FILE                  Record output audio to WAV file\n"
              << "  --wav-seconds S             Max WAV recording buffer length in seconds (default: 120)\n"
              << "  --verbose                   Print detailed diagnostic messages\n"
              << "  --help, -h                  Show this help text\n";
}

static bool ParseArgs(int argc, char* argv[], AppConfig& config) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            config.mode = AppConfig::Mode::List;
        } else if (arg == "--monitor") {
            config.mode = AppConfig::Mode::Monitor;
        } else if (arg == "--play") {
            config.mode = AppConfig::Mode::Play;
        } else if (arg == "--midi-port") {
            if (i + 1 < argc) {
                config.midi_port = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --midi-port requires an index argument." << std::endl;
                return false;
            }
        } else if (arg == "--show-realtime") {
            config.show_realtime = true;
        } else if (arg == "--buffer-frames") {
            if (i + 1 < argc) {
                config.buffer_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
            } else {
                std::cerr << "Error: --buffer-frames requires a frame count." << std::endl;
                return false;
            }
        } else if (arg == "--gain") {
            if (i + 1 < argc) {
                config.gain = static_cast<float>(std::atof(argv[++i]));
            } else {
                std::cerr << "Error: --gain requires a numeric value." << std::endl;
                return false;
            }
        } else if (arg == "--wav") {
            if (i + 1 < argc) {
                config.wav_filename = argv[++i];
            } else {
                std::cerr << "Error: --wav requires a filename." << std::endl;
                return false;
            }
        } else if (arg == "--wav-seconds") {
            if (i + 1 < argc) {
                config.wav_seconds = static_cast<uint32_t>(std::atoi(argv[++i]));
            } else {
                std::cerr << "Error: --wav-seconds requires duration in seconds." << std::endl;
                return false;
            }
        } else if (arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

static void DoList() {
    std::cout << "--- MIDI Input Ports ---\n";
    auto midi_ports = MidiInput::ListPorts();
    if (midi_ports.empty()) {
        std::cout << "No MIDI input ports found.\n";
    } else {
        for (const auto& p : midi_ports) {
            std::cout << "  [" << p.index << "] " << p.name << "\n";
        }
    }

    std::cout << "\n--- Audio Output Devices ---\n";
    auto audio_devs = AudioOut::ListDevices();
    if (audio_devs.empty()) {
        std::cout << "No audio playback devices found.\n";
    } else {
        for (const auto& d : audio_devs) {
            std::cout << "  [" << d.index << "] " << d.name
                      << (d.is_default ? " (Default)" : "") << "\n";
        }
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#endif

    AppConfig config;
    if (!ParseArgs(argc, argv, config)) {
        return 1;
    }

    if (config.mode == AppConfig::Mode::List) {
        DoList();
        return 0;
    }

    auto midi_ports = MidiInput::ListPorts();

    if (config.mode == AppConfig::Mode::Default) {
        if (midi_ports.empty()) {
            std::cout << "No MIDI input ports available.\n";
            DoList();
            return 1;
        }
        if (midi_ports.size() == 1) {
            config.midi_port = 0;
            config.mode = AppConfig::Mode::Play;
            std::cout << "Auto-selected single MIDI port: [" << midi_ports[0].index << "] " << midi_ports[0].name << "\n";
        } else {
            DoList();
            std::cout << "\nMultiple MIDI ports found. Please specify --midi-port N.\n";
            return 0;
        }
    }

    if (config.midi_port < 0) {
        if (midi_ports.empty()) {
            std::cerr << "Error: No MIDI input ports found." << std::endl;
            return 1;
        }
        config.midi_port = 0;
    }

    if (static_cast<size_t>(config.midi_port) >= midi_ports.size()) {
        std::cerr << "Error: Invalid MIDI port index " << config.midi_port << ". Available ports:\n";
        DoList();
        return 1;
    }

    SynthEngine_Init();
    MIDI_Queue_Init();

    MidiInput midi_in;
    auto start_time = std::chrono::steady_clock::now();

    midi_in.SetMonitorCallback([&](double timeStamp, const std::vector<uint8_t>& msg) {
        (void)timeStamp;
        if (msg.empty()) return;
        uint8_t status = msg[0];

        if (!config.show_realtime && (status == 0xF8 || status == 0xFE)) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - start_time).count();

        std::string line = FormatMidiLine(elapsed_sec, msg);
        {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cout << line << "\n";
        }
    });

    if (!midi_in.OpenPort(config.midi_port, false, !config.show_realtime, true)) {
        std::cerr << "Failed to open MIDI port " << config.midi_port << std::endl;
        return 1;
    }

    std::cout << "Opened MIDI Port [" << config.midi_port << "]: " << midi_ports[config.midi_port].name << "\n";

    WavRecorder wav_recorder;
    AudioOut audio_out;

    if (config.mode == AppConfig::Mode::Play) {
        if (!config.wav_filename.empty()) {
            wav_recorder.Init(config.wav_seconds, 48000, 2);
            std::cout << "WAV Recording enabled: " << config.wav_filename
                      << " (max " << config.wav_seconds << " s)\n";
        }

        if (!audio_out.Init(config.buffer_frames, config.gain, &wav_recorder)) {
            std::cerr << "Failed to initialize audio output." << std::endl;
            return 1;
        }

        std::cout << "Audio Output: " << audio_out.GetDeviceName() << "\n"
                  << "  Period size: " << audio_out.GetActualPeriodFrames() << " frames ("
                  << audio_out.GetSampleRate() << " Hz)\n"
                  << "  Est. Latency: " << audio_out.GetEstimatedLatencyMs() << " ms\n";

        if (!audio_out.Start()) {
            std::cerr << "Failed to start audio output." << std::endl;
            return 1;
        }
        std::cout << "Synth engine: " << SYNTH_ENGINE_NAME << "\n";
        std::cout << "Synth player running. Press ENTER or Ctrl+C to stop...\n";
    } else {
        std::cout << "MIDI Monitor running. Press ENTER or Ctrl+C to stop...\n";
    }

    std::thread stdin_thread([]() {
        std::cin.get();
        g_running.store(false);
    });

    while (g_running.load()) {
        MIDI_Event_t event;
        while (MIDI_Queue_Pop(&event)) {
            MIDI_Dispatch(&event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "\nStopping purrmidi_win...\n";

    if (config.mode == AppConfig::Mode::Play) {
        audio_out.Stop();
        if (!config.wav_filename.empty()) {
            if (wav_recorder.SaveToFile(config.wav_filename)) {
                std::cout << "Saved audio to " << config.wav_filename << "\n";
            } else {
                std::cerr << "Failed to save WAV file " << config.wav_filename << "\n";
            }
        }
    }

    midi_in.ClosePort();

    uint32_t overruns = MIDI_Queue_GetOverrunCount();
    std::cout << "MIDI Queue overrun count: " << overruns << "\n";

    if (stdin_thread.joinable()) {
        stdin_thread.detach();
    }

    return 0;
}
