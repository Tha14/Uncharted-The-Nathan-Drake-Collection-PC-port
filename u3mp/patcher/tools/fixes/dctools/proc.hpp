// dctools/proc.hpp: child processes and small text / JSON helpers of the C++ recipe steps that still start a
// converter script (u3fix.exe levels / actors): Python's subprocess behaviour on Windows.
//   run       one child to the end; a non-zero exit stops with '<script> failed (<code>): <stdout tail> <stderr tail>'
//   Worker    a long-running child fed one line at a time (stdin) that answers one line each (stdout)
#pragma once
#include "u3fix.hpp"

#include <functional>

namespace u3 {

using Env = std::map<std::string, std::string>;   // overrides on top of this process's environment ("" value: removed)

std::wstring wide(const std::string &s);
std::string P(const fs::path &p);                                 // a path as Python's os.path gives it on Windows
std::wstring quote_arg(const std::wstring &a);                   // subprocess.list2cmdline
std::string run(const std::vector<std::string> &cmd, const Env &env = {}, const std::string &cwd = "");
int run_pipe(const std::vector<std::string> &cmd, const Bytes &input, Bytes &out, std::string &err);   // stdin fed, exit code
int run_to_file(const std::vector<std::string> &cmd, const fs::path &out);                             // stdout to a file, exit code
int run_to_lines(const std::vector<std::string> &cmd, const std::function<void(const std::string &)> &on_line);   // stdout line by line, exit code
std::string text_file(const fs::path &p);                         // read in text mode (CRLF -> LF)
void write_text(const fs::path &p, const std::string &s);         // write in text mode (LF -> CRLF)
std::string sha_file(const fs::path &p);
void dump_compact(std::string &o, const json::Value &v);          // json.dump(v): one line, ', ' / ': '
std::string env_text(const json::Value &v);                       // an environment value from a JSON file

class Worker {
public:
    Worker(const std::vector<std::string> &cmd, const Env &env);
    ~Worker();
    bool request(const std::string &line, std::string &answer);   // false: the child is gone
private:
    void *proc = nullptr, *in = nullptr, *out = nullptr;
    std::string buf;
};

}  // namespace u3
