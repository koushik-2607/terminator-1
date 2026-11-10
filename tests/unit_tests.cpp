#include <iostream> // Prints progress so a person can see which test group is running.
#include <cassert> // Stops immediately when a required invariant is false.
#include <fstream> // Creates temporary files used to verify filename completion.
#include <sys/stat.h> // Provides mkdir/rmdir for isolated test directories.
#include "parser.hpp" // Tests quoting, substitution, pipeline, and redirection parsing.
#include "history.hpp" // Tests shared-memory history storage.
#include "autocomplete.hpp" // Tests filesystem-name completion.
#include "config.hpp" // Tests hotkey string parsing and action mapping.

using namespace Terminator; // Keep project type names short in this standalone test file.

void testParser() { // Exercise pure parser helpers without starting the X11 application.
    std::cout << "[Test] Parser:\n";

    // Multiline continuation check
    assert(Parser::isInputIncomplete("echo \"hello"));
    assert(Parser::isInputIncomplete("echo 'world"));
    assert(Parser::isInputIncomplete("echo `date"));
    assert(Parser::isInputIncomplete("echo $(ls"));
    assert(Parser::isInputIncomplete("ls | \\"));
    assert(!Parser::isInputIncomplete("echo \"hello world\""));
    assert(!Parser::isInputIncomplete("echo 'single quote'"));
    std::cout << "  - Multiline continuation tests passed.\n";

    // Variable expansion
    setenv("TEST_TERM_VAR", "TERMINATOR_VAL", 1);
    std::string exp1 = Parser::expandVariables("echo $TEST_TERM_VAR", 0);
    assert(exp1 == "echo TERMINATOR_VAL");

    std::string exp2 = Parser::expandVariables("echo '$TEST_TERM_VAR' \"$TEST_TERM_VAR\"", 0);
    assert(exp2 == "echo '$TEST_TERM_VAR' \"TERMINATOR_VAL\"");

    std::string exp_status = Parser::expandVariables("status=$?", 42);
    assert(exp_status == "status=42");
    std::cout << "  - Variable and exit status expansion tests passed.\n";

    // Command substitution
    auto dummy_runner = [](const std::string& cmd) {
        if (cmd == "basename /tmp/mydir") return "mydir";
        if (cmd == "echo nested") return "nested";
        return "out";
    };

    std::string sub1 = Parser::processSubstitutions("echo $(basename /tmp/mydir)", dummy_runner);
    assert(sub1 == "echo mydir");
    std::cout << "  - Command substitution tests passed.\n";

    // multiWatch parser
    std::vector<std::string> mw = Parser::parseMultiWatch("multiWatch [\"date +%T\", \"ls | wc -l\"]");
    assert(mw.size() == 2);
    assert(mw[0] == "date +%T");
    assert(mw[1] == "ls | wc -l");
    std::cout << "  - multiWatch parser tests passed.\n";

    // Pipeline parsing
    Pipeline p = Parser::parsePipeline("ls *.txt | wc -l > count.txt &", 0, dummy_runner);
    assert(p.run_in_background);
    assert(p.commands.size() == 2);
    assert(p.commands[0].args[0] == "ls");
    assert(p.commands[1].args[0] == "wc");
    assert(p.commands[1].output_file == "count.txt");
    std::cout << "  - Pipeline and redirection parsing tests passed.\n";
}

void testHistory() { // Check append, duplicate suppression, indexed reads, and isolated file persistence.
    std::cout << "[Test] Shared History:\n";
    char home_template[] = "/tmp/terminator_unit_history_XXXXXX";
    char* test_home = mkdtemp(home_template); // Create a unique temporary HOME so test commands never touch the user's real history.
    assert(test_home != nullptr);
    const char* previous_home_ptr = getenv("HOME");
    std::string previous_home = previous_home_ptr ? previous_home_ptr : "";
    setenv("HOME", test_home, 1); // HistoryManager resolves its persistent file from HOME.
    {
        HistoryManager hm(true, "/test_terminator_shm"); // Create a named shared-memory region for this isolated test.
        hm.add("echo 1");
        hm.add("echo 2");
        hm.add("echo 2"); // consecutive duplicate should be ignored

        assert(hm.size() == 2); // The second identical consecutive command should not be added twice.
        assert(hm.get(0) == "echo 1"); // First entry remains chronological index zero.
        assert(hm.get(1) == "echo 2"); // The nonduplicate second command occupies the next slot.
    }
    if (previous_home.empty()) unsetenv("HOME"); else setenv("HOME", previous_home.c_str(), 1);
    unlink((std::string(test_home) + "/.bash_history").c_str());
    rmdir(test_home);
    std::cout << "  - History tests passed.\n";
}

void testConfig() { // Check conversion from user-facing key/action strings to internal types.
    std::cout << "[Test] Config & Hotkeys:\n";
    ConfigManager cm;
    // Default left arrow
    KeyCombo kc;
    assert(ConfigManager::parseKeyCombo("Ctrl+A", kc));
    assert(kc.modifiers == ControlMask);
    assert(ConfigManager::parseActionName("line_start") == TerminalAction::START_OF_LINE);
    std::cout << "  - Hotkey configuration tests passed.\n";
}

void testAutocomplete() { // Verify unique and ambiguous filenames are found in the supplied tab cwd.
    std::cout << "[Test] Autocomplete:\n";
    mkdir("/tmp/term_autocomplete_test", 0777);
    std::ofstream("/tmp/term_autocomplete_test/abc.txt").close();
    std::ofstream("/tmp/term_autocomplete_test/abcd.txt").close();
    std::ofstream("/tmp/term_autocomplete_test/def.txt").close();
    auto res = Autocomplete::complete("unknown de", 10, "/tmp/term_autocomplete_test"); // Completion must use files even when the command name is unknown.
    assert(res.is_unique && res.prefix == "def.txt");
    auto ambiguous = Autocomplete::complete("unknown abc", 11, "/tmp/term_autocomplete_test"); // Two abc-prefixed files should expose their common prefix.
    assert(ambiguous.candidates.size() == 2 && ambiguous.prefix == "abc");
    unlink("/tmp/term_autocomplete_test/abc.txt");
    unlink("/tmp/term_autocomplete_test/abcd.txt");
    unlink("/tmp/term_autocomplete_test/def.txt");
    rmdir("/tmp/term_autocomplete_test");
    std::cout << "  - Autocomplete tests passed.\n";
}

int main() { // Run each independent group and return zero if no assert fails.
    std::cout << "================ Running Terminator Unit Tests ================\n";
    testParser();
    testHistory();
    testConfig();
    testAutocomplete();
    std::cout << "================ ALL UNIT TESTS PASSED SUCCESSFULLY! ================\n";
    return 0;
}

