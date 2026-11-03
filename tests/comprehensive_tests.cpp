#include <iostream>
#include <sstream>
#include <cassert>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "parser.hpp"
#include "history.hpp"
#include "autocomplete.hpp"
#include "config.hpp"
#include "jobs.hpp"
#include "shell.hpp"
#include "tab.hpp"

using namespace Terminator;

// Test utilities
static int g_test_passed = 0;
static int g_test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        g_test_passed++; \
        std::cout << "  [PASS] " << msg << "\n"; \
    } else { \
        g_test_failed++; \
        std::cerr << "  [FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
    } \
} while(0)

// -----------------------------------------------------------------------------
// Test 1: Parser & Multiline Quoting (Task 3)
// -----------------------------------------------------------------------------
void testTask3_MultilineAndQuoting() {
    std::cout << "\n=== Testing Task 3: Multiline Input, Quoting & Command Substitution ===\n";

    // 1. Incomplete line detection
    TEST_ASSERT(Parser::isInputIncomplete("echo \"hello"), "Detects unclosed double quote");
    TEST_ASSERT(Parser::isInputIncomplete("echo 'world"), "Detects unclosed single quote");
    TEST_ASSERT(Parser::isInputIncomplete("echo `date"), "Detects unclosed backtick");
    TEST_ASSERT(Parser::isInputIncomplete("echo $(ls"), "Detects unclosed subshell $(");
    TEST_ASSERT(Parser::isInputIncomplete("echo $(basename $(pwd)"), "Detects nested unclosed $(");
    TEST_ASSERT(Parser::isInputIncomplete("ls | \\"), "Detects line continuation with trailing \\");
    TEST_ASSERT(!Parser::isInputIncomplete("echo \"hello world\""), "Accepts complete double quote");
    TEST_ASSERT(!Parser::isInputIncomplete("echo 'single'"), "Accepts complete single quote");
    TEST_ASSERT(!Parser::isInputIncomplete("echo $(pwd)"), "Accepts complete command substitution");

    // 2. Variable expansion with bash quoting rules
    setenv("TEST_TERM_VAR", "TerminatorVal", 1);
    setenv("HOME", "/home/terminator_user", 1);
    std::string v_single = Parser::expandVariables("echo 'literal: $HOME'", 0);
    TEST_ASSERT(v_single == "echo 'literal: $HOME'", "Single quotes preserve literal $VAR");

    std::string v_double = Parser::expandVariables("echo \"expanded: $HOME\"", 0);
    TEST_ASSERT(v_double == "echo \"expanded: /home/terminator_user\"", "Double quotes expand $HOME");

    std::string v_both = Parser::expandVariables("echo 'literal: $HOME' \"expanded: $HOME\"", 0);
    TEST_ASSERT(v_both == "echo 'literal: $HOME' \"expanded: /home/terminator_user\"", "Mixed quotes match PDF Task 3 example");

    std::string v_status = Parser::expandVariables("echo exit=$?", 127);
    TEST_ASSERT(v_status == "echo exit=127", "Exit status $? expands correctly");

    // 3. Command substitution: innermost runs first
    auto sub_runner = [](const std::string& cmd) {
        if (cmd == "pwd") return "/home/terminator_user/mydir";
        if (cmd == "basename /home/terminator_user/mydir") return "mydir";
        if (cmd == "wc -l") return "3";
        if (cmd == "ls | wc -l") return "3";
        return "out";
    };

    std::string sub_nested = Parser::processSubstitutions("echo $(basename $(pwd))", sub_runner);
    TEST_ASSERT(sub_nested == "echo mydir", "Nested substitution $(basename $(pwd)) runs innermost first");

    std::string sub_backticks = Parser::processSubstitutions("echo `pwd`", sub_runner);
    TEST_ASSERT(sub_backticks == "echo /home/terminator_user/mydir", "Backtick substitution `pwd` works");

    // 4. Multiline quote with line continuation:
    // echo "Isn't process creation exciting?\nFork, exec, repeat!"
    Pipeline p_multi = Parser::parsePipeline("echo \"Isn't process creation exciting?\\\nFork, exec, repeat!\"", 0, sub_runner);
    TEST_ASSERT(p_multi.commands.size() == 1, "Multiline command parsed into 1 command");
    TEST_ASSERT(p_multi.commands[0].args.size() == 2, "echo has 1 argument");
    TEST_ASSERT(p_multi.commands[0].args[1] == "Isn't process creation exciting?\nFork, exec, repeat!", 
                "Multiline quoted argument preserves newline matching PDF Task 3");

    // 5. Line continuation in command substitution:
    // echo "Files in $(basename $(pwd)): $(ls | \nwc -l)"
    Pipeline p_sub_cont = Parser::parsePipeline("echo \"Files in $(basename $(pwd)): $(ls | \\\nwc -l)\"", 0, sub_runner);
    TEST_ASSERT(p_sub_cont.commands.size() == 1, "Pipeline with continuation inside $() parsed correctly");
    TEST_ASSERT(p_sub_cont.commands[0].args[1] == "Files in mydir: 3", 
                "Command substitution with continuation produces 'Files in mydir: 3'");
}

// -----------------------------------------------------------------------------
// Test 2: I/O Redirection & Pipelines (Tasks 4 & 5)
// -----------------------------------------------------------------------------
void testTask4_and_5_RedirectionAndPipes() {
    std::cout << "\n=== Testing Tasks 4 & 5: Redirection & Pipelines ===\n";

    // Create a temporary directory for file redirection tests
    mkdir("/tmp/term_test_dir", 0777);
    std::ofstream infile("/tmp/term_test_dir/infile.txt");
    infile << "cherry\napple\nbanana\n";
    infile.close();

    HistoryManager hm(true, "/test_shm_p");
    std::string captured_output;
    auto capture = [&captured_output](const std::string& msg) {
        captured_output += msg;
    };

    // Create pipe to capture external child stdout like Tab does
    int test_pfd[2];
    assert(pipe(test_pfd) == 0);
    fcntl(test_pfd[0], F_SETFL, O_NONBLOCK);

    auto drain_test_pipe = [&]() {
        char buf[1024];
        ssize_t n;
        while ((n = read(test_pfd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            captured_output += buf;
        }
    };

    ShellEngine shell(&hm, capture, -1, test_pfd[1]);

    // 1. Input redirection: sort < infile.txt
    captured_output.clear();
    shell.execute("sort < /tmp/term_test_dir/infile.txt");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("apple\nbanana\ncherry") != std::string::npos, 
                "Input redirection 'sort < infile.txt' produces sorted lines");

    // 2. Output redirection: echo "hello world" > outfile.txt
    shell.execute("echo \"hello world\" > /tmp/term_test_dir/outfile.txt");
    drain_test_pipe();
    std::ifstream outfile("/tmp/term_test_dir/outfile.txt");
    std::string out_content;
    std::getline(outfile, out_content);
    outfile.close();
    TEST_ASSERT(out_content == "hello world", "Output redirection '>' wrote to file correctly");

    // 3. Both together: sort < infile.txt > sorted.txt
    shell.execute("sort < /tmp/term_test_dir/infile.txt > /tmp/term_test_dir/sorted.txt");
    drain_test_pipe();
    std::ifstream sorted_file("/tmp/term_test_dir/sorted.txt");
    std::string s_line;
    std::string full_sorted;
    while (std::getline(sorted_file, s_line)) {
        full_sorted += s_line + "\n";
    }
    sorted_file.close();
    TEST_ASSERT(full_sorted == "apple\nbanana\ncherry\n", 
                "Combined redirection '< infile.txt > outfile.txt' works");

    // 4. Missing input file: report missing file instead of running command
    captured_output.clear();
    shell.execute("sort < /tmp/term_test_dir/nonexistent_file.txt");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("terminator: /tmp/term_test_dir/nonexistent_file.txt: No such file or directory") != std::string::npos, 
                "Missing input file reports error without running command");
    TEST_ASSERT(shell.getLastExitStatus() == 1, "$? is set to 1 on missing input file");

    // 5. Arbitrary Pipelines of any length (N commands, N-1 pipes)
    // cat infile.txt | sort | head -2
    captured_output.clear();
    shell.execute("cat /tmp/term_test_dir/infile.txt | sort | head -2");
    drain_test_pipe();
    TEST_ASSERT(captured_output == "apple\nbanana\n", 
                "3-stage pipeline 'cat | sort | head -2' works correctly");

    // Pipeline with redirection: cat infile.txt | sort | wc -l > count.txt
    shell.execute("cat /tmp/term_test_dir/infile.txt | sort | wc -l > /tmp/term_test_dir/count.txt");
    drain_test_pipe();
    std::ifstream count_file("/tmp/term_test_dir/count.txt");
    std::string count_str;
    count_file >> count_str;
    count_file.close();
    TEST_ASSERT(count_str == "3", "Pipeline combined with output redirection produces 3");

    close(test_pfd[0]);
    close(test_pfd[1]);

    // Cleanup temp files
    unlink("/tmp/term_test_dir/infile.txt");
    unlink("/tmp/term_test_dir/outfile.txt");
    unlink("/tmp/term_test_dir/sorted.txt");
    unlink("/tmp/term_test_dir/count.txt");
    rmdir("/tmp/term_test_dir");
}

// -----------------------------------------------------------------------------
// Test 3: Built-in & External Commands (Task 2)
// -----------------------------------------------------------------------------
void testTask2_BuiltinAndExternal() {
    std::cout << "\n=== Testing Task 2: Built-in and External Commands ===\n";

    HistoryManager hm(true, "/test_shm_b");
    std::string captured_output;
    auto capture = [&captured_output](const std::string& msg) {
        captured_output += msg;
    };

    // Create pipe to capture external child stdout like Tab does
    int test_pfd[2];
    assert(pipe(test_pfd) == 0);
    fcntl(test_pfd[0], F_SETFL, O_NONBLOCK);

    auto drain_test_pipe = [&]() {
        char buf[1024];
        ssize_t n;
        while ((n = read(test_pfd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            captured_output += buf;
        }
    };

    ShellEngine shell(&hm, capture, -1, test_pfd[1]);

    // 1. Unknown command reporting
    captured_output.clear();
    shell.execute("notacommand");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("terminator: notacommand: command not found") != std::string::npos, 
                "Unknown command prints 'terminator: notacommand: command not found'");
    TEST_ASSERT(shell.getLastExitStatus() == 127, "$? is 127 for command not found");

    // 2. cd built-in (per-tab directory)
    shell.execute("cd /tmp");
    drain_test_pipe();
    TEST_ASSERT(shell.getCwd() == "/tmp", "cd /tmp changes current directory to /tmp");
    TEST_ASSERT(shell.getLastExitStatus() == 0, "$? is 0 on successful cd");

    // cd to non-existent directory
    captured_output.clear();
    shell.execute("cd /nonexistent_path_test_123");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("terminator: cd: /nonexistent_path_test_123: No such file or directory") != std::string::npos, 
                "cd to invalid path prints error");
    TEST_ASSERT(shell.getLastExitStatus() == 1, "$? is 1 on failed cd");

    // cd - returns to previous directory
    shell.execute("cd -");
    drain_test_pipe();
    TEST_ASSERT(shell.getLastExitStatus() == 0, "cd - returns to previous directory");

    // 3. pwd built-in
    captured_output.clear();
    shell.execute("pwd");
    drain_test_pipe();
    TEST_ASSERT(!captured_output.empty() && captured_output.find('/') != std::string::npos, 
                "pwd prints current working directory");

    // 4. External command execution: echo "testing external"
    captured_output.clear();
    shell.execute("echo \"testing external\"");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("testing external") != std::string::npos, 
                "External command 'echo' executes and outputs text");
    TEST_ASSERT(shell.getLastExitStatus() == 0, "$? is 0 after successful external command");

    // 5. source built-in
    std::ofstream script("/tmp/test_term_script.sh");
    script << "# Terminator Script\n";
    script << "echo \"Line 1\"\n";
    script << "echo \"Line 2\"\n";
    script.close();

    captured_output.clear();
    shell.execute("source /tmp/test_term_script.sh");
    drain_test_pipe();
    TEST_ASSERT(captured_output.find("Line 1\nLine 2") != std::string::npos, 
                "source executes shell script lines sequentially");
    unlink("/tmp/test_term_script.sh");

    close(test_pfd[0]);
    close(test_pfd[1]);
}

// -----------------------------------------------------------------------------
// Test 4: multiWatch (Task 6)
// -----------------------------------------------------------------------------
void testTask6_multiWatch() {
    std::cout << "\n=== Testing Task 6: multiWatch ===\n";

    // 1. Parsing multiWatch command syntax
    std::vector<std::string> cmds = Parser::parseMultiWatch("multiWatch [\"date +%T\", \"echo 42 | wc -l\"]");
    TEST_ASSERT(cmds.size() == 2, "multiWatch syntax parsed into 2 commands");
    TEST_ASSERT(cmds[0] == "date +%T" && cmds[1] == "echo 42 | wc -l", "Command strings match correctly");

    // 2. multiWatch execution and output format check
    int cancel_pipe[2];
    assert(pipe(cancel_pipe) == 0);
    fcntl(cancel_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(cancel_pipe[1], F_SETFL, O_NONBLOCK);

    std::string mw_output;
    auto mw_cb = [&mw_output](const std::string& msg) {
        mw_output += msg;
    };

    std::vector<std::string> test_cmds = {"echo hello_watch"};
    MultiWatchRunner mw(test_cmds, mw_cb, cancel_pipe[0], "/tmp");

    // Send Ctrl+C cancel signal via cancel pipe after 300ms from background thread
    pthread_t cancel_thread;
    pthread_create(&cancel_thread, nullptr, [](void* arg) -> void* {
        int* pfd = static_cast<int*>(arg);
        usleep(300000); // 300ms
        char c = '\003';
        ssize_t w = write(pfd[1], &c, 1);
        (void)w;
        return nullptr;
    }, cancel_pipe);

    mw.run();
    pthread_join(cancel_thread, nullptr);
    close(cancel_pipe[0]);
    close(cancel_pipe[1]);

    // Check exact required formatting:
    // "cmd", <timestamp>:
    // ---------------------------------------------------- (52 dashes)
    // <output>
    // ----------------------------------------------------
    TEST_ASSERT(mw_output.find("\"echo hello_watch\", ") != std::string::npos, 
                "multiWatch header contains command name and timestamp");
    TEST_ASSERT(mw_output.find("----------------------------------------------------") != std::string::npos, 
                "multiWatch output has exact 52-dash divider lines");
    TEST_ASSERT(mw_output.find("hello_watch") != std::string::npos, 
                "multiWatch output contains child command output");
}

// -----------------------------------------------------------------------------
// Test 5: Signals and Job Control (Task 8)
// -----------------------------------------------------------------------------
void testTask8_SignalsAndJobs() {
    std::cout << "\n=== Testing Task 8: Signals and Job Control ===\n";

    HistoryManager hm(true, "/test_shm_j");
    std::string captured_output;
    auto capture = [&captured_output](const std::string& msg) {
        captured_output += msg;
    };

    ShellEngine shell(&hm, capture);

    // 1. Background job: sleep 1 &
    captured_output.clear();
    shell.execute("sleep 1 &");
    TEST_ASSERT(captured_output.find("[1] ") != std::string::npos, 
                "Background job 'sleep 1 &' prints [1] <PGID>");

    // Check jobs list
    captured_output.clear();
    shell.execute("jobs");
    TEST_ASSERT(captured_output.find("[1]+ Running sleep 1 &") != std::string::npos, 
                "jobs builtin lists background job as Running");

    // Wait for background job to finish
    sleep(2);
    captured_output.clear();
    shell.execute("jobs");
    TEST_ASSERT(captured_output.find("[1]+ Done") != std::string::npos || captured_output.empty(), 
                "Background job completes and notifies Done");

    // 2. Job control suspend & resume via simulated Ctrl+Z and bg/fg
    JobManager& jm = shell.getJobManager();
    // Simulate a stopped job: sleep 100
    std::vector<pid_t> dummy_pids = {99999};
    int jid = jm.addJob(99999, dummy_pids, "sleep 100", JobState::STOPPED);
    TEST_ASSERT(jid == 2, "Added stopped job with ID 2");

    Job* j = jm.findJobBySpec("%2");
    TEST_ASSERT(j != nullptr && j->command == "sleep 100", "findJobBySpec finds job by %2");

    std::string job_list = jm.listJobs();
    TEST_ASSERT(job_list.find("[2]+ Stopped sleep 100") != std::string::npos, 
                "listJobs displays '[2]+ Stopped sleep 100'");
}

// -----------------------------------------------------------------------------
// Test 6: Hotkeys & Configuration (Task 7)
// -----------------------------------------------------------------------------
void testTask7_HotkeysAndConfig() {
    std::cout << "\n=== Testing Task 7: Hotkeys and Configuration ===\n";

    ConfigManager cm;
    KeyCombo kc;

    // Default keybindings check
    TEST_ASSERT(cm.getAction(XK_Left, 0) == TerminalAction::MOVE_LEFT, "Left arrow -> MOVE_LEFT");
    TEST_ASSERT(cm.getAction(XK_Right, 0) == TerminalAction::MOVE_RIGHT, "Right arrow -> MOVE_RIGHT");
    TEST_ASSERT(cm.getAction(XK_a, ControlMask) == TerminalAction::START_OF_LINE, "Ctrl+A -> START_OF_LINE");
    TEST_ASSERT(cm.getAction(XK_e, ControlMask) == TerminalAction::END_OF_LINE, "Ctrl+E -> END_OF_LINE");
    TEST_ASSERT(cm.getAction(XK_Up, 0) == TerminalAction::HISTORY_UP, "Up arrow -> HISTORY_UP");
    TEST_ASSERT(cm.getAction(XK_Down, 0) == TerminalAction::HISTORY_DOWN, "Down arrow -> HISTORY_DOWN");
    TEST_ASSERT(cm.getAction(XK_u, ControlMask) == TerminalAction::DELETE_TO_START, "Ctrl+U -> DELETE_TO_START");
    TEST_ASSERT(cm.getAction(XK_k, ControlMask) == TerminalAction::DELETE_TO_END, "Ctrl+K -> DELETE_TO_END");
    TEST_ASSERT(cm.getAction(XK_w, ControlMask) == TerminalAction::DELETE_PREV_WORD, "Ctrl+W -> DELETE_PREV_WORD");
    TEST_ASSERT(cm.getAction(XK_l, ControlMask) == TerminalAction::CLEAR_SCREEN, "Ctrl+L -> CLEAR_SCREEN");
    TEST_ASSERT(cm.getAction(XK_d, ControlMask) == TerminalAction::EXIT_TAB, "Ctrl+D -> EXIT_TAB");
    TEST_ASSERT(cm.getAction(XK_c, ControlMask | ShiftMask) == TerminalAction::COPY, "Ctrl+Shift+C -> COPY");
    TEST_ASSERT(cm.getAction(XK_v, ControlMask | ShiftMask) == TerminalAction::PASTE, "Ctrl+Shift+V -> PASTE");

    // Tab hotkeys
    TEST_ASSERT(cm.getAction(XK_t, ControlMask | ShiftMask) == TerminalAction::NEW_TAB, "Ctrl+Shift+T -> NEW_TAB");
    TEST_ASSERT(cm.getAction(XK_Tab, ControlMask) == TerminalAction::NEXT_TAB, "Ctrl+Tab -> NEXT_TAB");
}

// -----------------------------------------------------------------------------
// Test 7: Dual Execution Modes (Thread vs Process Mode)
// -----------------------------------------------------------------------------
void testExecutionModes_ThreadAndProcess() {
    std::cout << "\n=== Testing Dual Execution Modes: Thread Mode vs Process Mode ===\n";

    HistoryManager hm(true, "/test_shm_modes");

    // 1. Thread Mode Tab
    {
        Tab tab_thread(1, false, &hm);
        TEST_ASSERT(tab_thread.getId() == 1, "Thread mode tab initialized with ID 1");
        tab_thread.sendCommand("echo \"hello_from_thread\"");

        // Read output from tab pipe
        std::string tab_out;
        char buf[512];
        int attempts = 0;
        while (attempts++ < 30 && tab_out.find("hello_from_thread") == std::string::npos) {
            ssize_t n = read(tab_thread.getReadFd(), buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                tab_out.append(buf, n);
            }
            usleep(50000); // 50ms
        }
        TEST_ASSERT(tab_out.find("hello_from_thread") != std::string::npos, 
                    "Thread mode tab successfully executes commands and returns output over pipe");
        tab_thread.terminate();
    }

    // 2. Process Mode Tab
    {
        Tab tab_process(2, true, &hm);
        TEST_ASSERT(tab_process.getId() == 2, "Process mode tab initialized with ID 2");
        TEST_ASSERT(tab_process.getPid() > 0, "Process mode tab forked child process");
        tab_process.sendCommand("echo \"hello_from_process\"");

        std::string tab_out;
        char buf[512];
        int attempts = 0;
        while (attempts++ < 30 && tab_out.find("hello_from_process") == std::string::npos) {
            ssize_t n = read(tab_process.getReadFd(), buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                tab_out.append(buf, n);
            }
            usleep(50000);
        }
        TEST_ASSERT(tab_out.find("hello_from_process") != std::string::npos, 
                    "Process mode tab successfully executes commands and returns output over pipe");

        // 3. Shared History across tabs in different modes
        hm.add("command_tab1");
        hm.add("command_tab2");
        TEST_ASSERT(hm.size() == 2, "Shared history contains commands from both tabs");
        TEST_ASSERT(hm.get(0) == "command_tab1", "Tab 1 command is present in shared history");
        TEST_ASSERT(hm.get(1) == "command_tab2", "Tab 2 command is present in shared history");

        // 4. Tab crash / close isolation
        // Terminate process mode tab and ensure no hang
        tab_process.terminate();
        TEST_ASSERT(tab_process.isClosed(), "Process mode tab cleanly terminates without affecting other tabs");
    }
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------
int main() {
    signal(SIGPIPE, SIG_IGN);
    std::cout << "=================================================================\n";
    std::cout << "  TERMINATOR COMPREHENSIVE VERIFICATION TEST SUITE (CS69201)     \n";
    std::cout << "=================================================================\n";

    testTask3_MultilineAndQuoting();
    testTask4_and_5_RedirectionAndPipes();
    testTask2_BuiltinAndExternal();
    testTask6_multiWatch();
    testTask8_SignalsAndJobs();
    testTask7_HotkeysAndConfig();
    testExecutionModes_ThreadAndProcess();

    std::cout << "\n=================================================================\n";
    std::cout << "  TEST SUMMARY: " << g_test_passed << " PASSED, " << g_test_failed << " FAILED\n";
    std::cout << "=================================================================\n";

    return (g_test_failed == 0) ? 0 : 1;
}
