#include <iostream>
#include <cassert>
#include "parser.hpp"
#include "history.hpp"
#include "autocomplete.hpp"
#include "config.hpp"

using namespace Terminator;

void testParser() {
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

void testHistory() {
    std::cout << "[Test] Shared History:\n";
    HistoryManager hm(true, "/test_terminator_shm");
    hm.add("echo 1");
    hm.add("echo 2");
    hm.add("echo 2"); // consecutive duplicate should be ignored

    assert(hm.size() == 2);
    assert(hm.get(0) == "echo 1");
    assert(hm.get(1) == "echo 2");
    std::cout << "  - History tests passed.\n";
}

void testConfig() {
    std::cout << "[Test] Config & Hotkeys:\n";
    ConfigManager cm;
    // Default left arrow
    KeyCombo kc;
    assert(ConfigManager::parseKeyCombo("Ctrl+A", kc));
    assert(kc.modifiers == ControlMask);
    assert(ConfigManager::parseActionName("line_start") == TerminalAction::START_OF_LINE);
    std::cout << "  - Hotkey configuration tests passed.\n";
}

void testAutocomplete() {
    std::cout << "[Test] Autocomplete:\n";
    auto res = Autocomplete::complete("c", 1);
    assert(!res.candidates.empty());
    // Should include 'cd'
    bool found_cd = false;
    for (const auto& c : res.candidates) {
        if (c == "cd") found_cd = true;
    }
    assert(found_cd);
    std::cout << "  - Autocomplete tests passed.\n";
}

int main() {
    std::cout << "================ Running Terminator Unit Tests ================\n";
    testParser();
    testHistory();
    testConfig();
    testAutocomplete();
    std::cout << "================ ALL UNIT TESTS PASSED SUCCESSFULLY! ================\n";
    return 0;
}

