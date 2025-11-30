#!/bin/bash
echo "🚀 Starting Cyber-Shell Comprehensive Tests..."
echo "=============================================="

# Path to your shell
MYSHELL="./mysh"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Counters
TESTS_PASSED=0
TESTS_FAILED=0

# Print test result
print_result() {
    if [ "$1" -eq 0 ]; then
        echo -e "${GREEN}✓ PASS${NC}: $2"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ FAIL${NC}: $2"
        ((TESTS_FAILED++))
    fi
}

# Run mysh with input
run_mysh() {
    echo -e "$1" | $MYSHELL > /dev/null 2>&1
    return $?
}

# ===== BASIC COMMAND EXECUTION TESTS =====
echo -e "\n${CYAN}=== BASIC COMMAND EXECUTION TESTS ===${NC}"

run_mysh "ls"; print_result $? "Basic 'ls' command"
run_mysh "ls -la"; print_result $? "ls with arguments"
run_mysh "/bin/echo test"; print_result $? "Absolute path command"
run_mysh "echo test"; print_result $? "Relative path command"

# ===== BUILT-IN COMMANDS TESTS =====
echo -e "\n${CYAN}=== BUILT-IN COMMANDS TESTS ===${NC}"

run_mysh "cd /tmp"; print_result $? "cd command"
run_mysh "mkdir tdir && rmdir tdir"; print_result $? "mkdir command"
run_mysh "touch tf && rm tf"; print_result $? "touch command"
run_mysh "clear"; print_result $? "clear command"

# ===== PIPING TESTS =====
echo -e "\n${CYAN}=== PIPING TESTS ===${NC}"

run_mysh "echo hello | grep h"; print_result $? "Simple pipe"
run_mysh "echo hello world | grep hello | wc -l"; print_result $? "Multiple pipes"
run_mysh "ls -la | head -5"; print_result $? "Pipe with arguments"

# ===== REDIRECTION TESTS =====
echo -e "\n${CYAN}=== REDIRECTION TESTS ===${NC}"

run_mysh "echo test > out1.txt"; print_result $? "Output redirection >"
rm -f out1.txt

run_mysh "echo hi > a.txt && echo bye >> a.txt"; print_result $? "Append >> redirection"
rm -f a.txt

run_mysh "echo hello > b.txt && cat < b.txt"; print_result $? "Input < redirection"
rm -f b.txt

# ===== QUOTES & ESCAPES =====
echo -e "\n${CYAN}=== QUOTES & ESCAPES ===${NC}"

run_mysh "echo 'hello world'"; print_result $? "Single quotes"
run_mysh "echo \"hello world\""; print_result $? "Double quotes"
run_mysh "echo hello\ world"; print_result $? "Escaped space"

# ===== BACKGROUND JOBS TESTS =====
echo -e "\n${CYAN}=== BACKGROUND JOBS TESTS ===${NC}"

run_mysh "sleep 1 &"; print_result $? "Background job"
run_mysh "sleep 1 &\njobs"; print_result $? "jobs command"

# ===== ALIAS TESTS =====
echo -e "\n${CYAN}=== ALIAS TESTS ===${NC}"

run_mysh "alias hi='echo hey'"; run_mysh "hi"; print_result $? "Alias expansion"

# ===== ENVIRONMENT TESTS =====
echo -e "\n${CYAN}=== ENVIRONMENT TESTS ===${NC}"

run_mysh "set X=123"; run_mysh "echo \$X"; print_result $? "Variable expansion"
run_mysh "cd $HOME"; print_result $? "Home directory expansion"

# ===== ERROR HANDLING TESTS =====
echo -e "\n${CYAN}=== ERROR HANDLING TESTS ===${NC}"

run_mysh "invalid_command_xyz"; print_result $? "Invalid command handling"
run_mysh "thisdoesnotexist"; print_result $? "Command not found"

run_mysh "touch noperms && chmod 000 noperms && cat noperms"
PERMRESULT=$?
chmod 644 noperms && rm noperms
print_result $PERMRESULT "Permission denied"

# ===== SUMMARY =====
echo -e "\n${CYAN}=== SUMMARY ===${NC}"
echo -e "${BLUE}Tests run: $((TESTS_PASSED + TESTS_FAILED))${NC}"
echo -e "${GREEN}Passed: $TESTS_PASSED${NC}"
echo -e "${RED}Failed: $TESTS_FAILED${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}🎉 ALL TESTS PASSED!${NC}"
else
    echo -e "${YELLOW}⚠️  Some tests failed. Fix required.${NC}"
fi
