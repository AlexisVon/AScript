#ifndef ASH_MAIN_IMPL_H
#define ASH_MAIN_IMPL_H

#include <string>

namespace cli {

    // Command-line mode entry (no -ui). Argument parsing/execution skeleton;
    // full behavior pending design decisions.
    int impl(int _argc, char** _argv);


    // help — print usage
    void help();

    // print_langref — print the language reference as plain text; _re filters
    // chapter and section titles. Returns 0 on success, 1 on a bad pattern.
    int print_langref(const std::string& _re);

} // namespace cli

#endif // ASH_MAIN_IMPL_H
