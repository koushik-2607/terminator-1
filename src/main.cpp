#include "common.hpp" // Shared POSIX types, constants, and helpers used throughout Terminator.
#include "gui.hpp"   // Declares TerminalGUI, which owns the X11 window and tab event loop.
#include <iostream>  // Provides std::cout and std::cerr for startup and error messages.
#include <string>    // Provides std::string for readable command-line argument handling.
#include <cstring>   // C string utilities are available here; this file currently needs no direct call from them.

/**
 * Terminator: A Custom Terminal Emulator and Shell for Linux
 * Computing Lab (CS69201) · Term Project
 *
 * Execution Modes:
 *   ./terminator                  # Thread mode (default): tabs run as threads sharing memory
 *   ./terminator useProcess=True  # Process mode: GUI in parent, tabs fork child processes
 */
int main(int argc, char* argv[]) { // argc is the number of command-line words; argv stores their null-terminated text.
    signal(SIGPIPE, SIG_IGN); // Ignore writes to a pipe whose reader has closed; this prevents an unexpected SIGPIPE from killing the GUI process.
    bool use_process_mode = false; // Default to one process with tab worker threads unless the user asks for process mode.

    // Parse command line arguments to detect execution mode
    for (int i = 1; i < argc; ++i) { // Start at 1 because argv[0] is the program name, not an option.
        std::string arg = argv[i]; // Copy one argument from C-style argv storage into a convenient C++ string.
        if (arg == "useProcess=True" || arg == "useProcess=true" || arg == "--process" || arg == "-p") { // Accept the documented setting and two shorter aliases.
            use_process_mode = true; // The GUI will configure newly opened tabs to run in separate child processes.
        } else if (arg == "--help" || arg == "-h") { // Help can appear among the arguments and exits without opening X11.
            std::cout << "Terminator: A Custom Terminal Emulator and Shell\n"; // Print the program's short description to standard output.
            std::cout << "Usage:\n"; // Introduce the available invocation forms.
            std::cout << "  ./terminator                  # Launch in Thread Mode (default)\n"; // Show the default execution mode.
            std::cout << "  ./terminator useProcess=True  # Launch in Process Mode\n"; // Show the process-based alternative.
            return 0; // Exit successfully; no GUI object or X11 display has been created yet.
        }
    }

    std::cout << "Launching Terminator in " // Write a startup message so the selected mode is visible in the launching terminal.
              << (use_process_mode ? "PROCESS MODE (useProcess=True)" : "THREAD MODE (default)") // Choose the label using the boolean mode flag.
              << "...\n"; // Finish the message with an ellipsis and newline; this text goes to standard output.

    // Initialize the X11 GUI and Tab management engine
    Terminator::TerminalGUI gui(use_process_mode); // Construct the GUI controller and pass it the execution-mode choice; construction itself does not yet open the display.

    if (!gui.init()) { // init() opens the X server connection and creates window resources; false means the GUI cannot safely run.
        std::cerr << "Fatal Error: Failed to initialize Terminator X11 graphical interface.\n"; // Report the failure on standard error for visibility in a terminal.
        std::cerr << "Ensure that an active X11 server is running and $DISPLAY is set.\n"; // Explain the usual system-level cause: no reachable X display.
        return 1; // Use a nonzero exit status so scripts and the OS can detect startup failure.
    }

    // Run the non-busy-wait event loop
    gui.run(); // Enter the blocking X11/poll event loop; it returns after the user closes the application.

    std::cout << "Terminator exited successfully.\n"; // Announce normal completion after the GUI event loop has ended.
    return 0; // Report successful process termination to the operating system.
}


// EOF// EOF

// EOF
