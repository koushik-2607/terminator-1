#include "common.hpp"
#include "gui.hpp"
#include <iostream>
#include <string>
#include <cstring>

/**
 * Terminator: A Custom Terminal Emulator and Shell for Linux
 * Computing Lab (CS69201) · Term Project
 *
 * Execution Modes:
 *   ./terminator                  # Thread mode (default): tabs run as threads sharing memory
 *   ./terminator useProcess=True  # Process mode: GUI in parent, tabs fork child processes
 */
int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    bool use_process_mode = false;

    // Parse command line arguments to detect execution mode
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "useProcess=True" || arg == "useProcess=true" || arg == "--process" || arg == "-p") {
            use_process_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Terminator: A Custom Terminal Emulator and Shell\n";
            std::cout << "Usage:\n";
            std::cout << "  ./terminator                  # Launch in Thread Mode (default)\n";
            std::cout << "  ./terminator useProcess=True  # Launch in Process Mode\n";
            return 0;
        }
    }

    std::cout << "Launching Terminator in " 
              << (use_process_mode ? "PROCESS MODE (useProcess=True)" : "THREAD MODE (default)") 
              << "...\n";

    // Initialize the X11 GUI and Tab management engine
    Terminator::TerminalGUI gui(use_process_mode);

    if (!gui.init()) {
        std::cerr << "Fatal Error: Failed to initialize Terminator X11 graphical interface.\n";
        std::cerr << "Ensure that an active X11 server is running and $DISPLAY is set.\n";
        return 1;
    }

    // Run the non-busy-wait event loop
    gui.run();

    std::cout << "Terminator exited successfully.\n";
    return 0;
}

