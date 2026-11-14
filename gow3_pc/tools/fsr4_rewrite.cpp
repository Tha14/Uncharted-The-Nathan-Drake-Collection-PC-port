// tools/fsr4_rewrite.cpp: rewrites decompiled FSR 4 v07 passes (spirv-cross GLSL, LF lines) for
// tools/fsr4_optimize.sh, between its decompile and its recompile. GLSL on stdin, the rewritten
// GLSL on stdout; a pass that does not look as expected ends with a message and exit code 1.
//   fsr4_rewrite post-lds     < post.comp   > post_lds.comp
//   fsr4_rewrite pass11-guard < pass11.comp > pass11_guard.comp
// Build: clang++ -std=c++20 -O2 tools/fsr4_rewrite.cpp -o fsr4_rewrite
//
// post-lds: rewrites a post pass for RDNA3.
// Each invocation computes a 2x2 block of output pixels and stores them into three images
// (recurrent state, history, output). Stores of every other pixel from three images cost ~4x
// the arithmetic of the pass (2.9 of 6.1 ms at 2260x1272 -> 3840x2160 on an RX 7800 XT). Here
// the 8x8 workgroup collects its 16x16 block in shared memory and stores it in contiguous rows.
// Values stay float in shared memory and are converted by the store as before (a half value
// from shared memory would become a 16-bit store, which rounds differently), so the result is
// bit-exact with the original pass.
//
// pass11-guard: fixes a data race in model pass 11 (decoder, 1/4 -> 1/2 resolution).
// Each invocation is a pixel of its 1/4-resolution input and writes a 2x2 block of the 1/2-
// resolution output. The dispatch rounds the width up to 64 invocations, and invocations past
// the input width are not stopped: at the 1080 tier (input 480 wide) invocations 480..511 write
// output pixels 960..1023, which are the first pixels of the next output row (every tensor row
// is 15360 bytes), racing with their real writers. The upscaled frame then differs from run to
// run in a strip at the left edge. Here they return first. The input size comes from the pass's
// own neighbour bounds check.
//
// Both: spirv-cross mistranslates the signed int8 unpacks; signed_unpack corrects them.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr size_t npos = std::string::npos;

[[noreturn]] void die(const std::string& message) {
    std::fputs(message.c_str(), stderr);
    std::exit(1);
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_word(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// The first `word` at or after `from` that does not continue a longer word to its left.
size_t find_word(std::string_view src, std::string_view word, size_t from = 0) {
    size_t at = src.find(word, from);
    while (at != npos && at > 0 && is_word(src[at - 1])) {
        at = src.find(word, at + 1);
    }
    return at;
}

// The first `text` that starts a line.
size_t find_line(std::string_view src, std::string_view text) {
    size_t at = src.find(text);
    while (at != npos && at > 0 && src[at - 1] != '\n') {
        at = src.find(text, at + 1);
    }
    return at;
}

struct Match {
    size_t begin = npos, end = npos;
    std::vector<std::string> numbers;
};

// The first place at or after `from` where `pattern` matches: its characters stand for
// themselves, except '#', which is one or more decimal digits (as many as there are; no
// pattern continues with a digit after '#').
Match find_pattern(std::string_view src, std::string_view pattern, size_t from = 0) {
    const std::string_view prefix = pattern.substr(0, pattern.find('#'));
    for (size_t at = src.find(prefix, from); at != npos; at = src.find(prefix, at + 1)) {
        Match match{at, at, {}};
        bool matched = true;
        for (const char c : pattern) {
            if (c == '#') {
                const size_t first = match.end;
                while (match.end < src.size() && is_digit(src[match.end])) {
                    match.end++;
                }
                matched = match.end > first;
                match.numbers.emplace_back(src.substr(first, match.end - first));
            } else {
                matched = match.end < src.size() && src[match.end] == c;
                match.end++;
            }
            if (!matched) {
                break;
            }
        }
        if (matched) {
            return match;
        }
    }
    return {};
}

// spirv-cross translates OpBitcast uint -> v4char (signed int8) as unpack8(uint), which returns
// an unsigned u8vec4. The passes only ever unpack signed int8 tensors (no OpBitcast to v4uchar),
// so every unpack8 goes through a signed helper, declared before main.
void signed_unpack(std::string& src) {
    const std::string_view unpack = "unpack8(";
    size_t count = 0;
    for (size_t at = find_word(src, unpack); at != npos; at = find_word(src, unpack, at + 1)) {
        src.replace(at, unpack.size(), "gow3UnpackS8(");
        count++;
    }
    if (!count) {
        die("expected signed int8 unpacks\n");
    }
    const size_t main_at = find_line(src, "void main()\n");
    if (main_at == npos) {
        die("no main\n");
    }
    src.insert(main_at, R"(// gow3: signed int8 unpack (spirv-cross emits the unsigned unpack8(uint)).
i8vec4 gow3UnpackS8(uint v)
{
    return unpack8(int(v));
}

)");
}

void post_lds(std::string& src) {
    if (src.find("layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;") == npos) {
        die("unexpected local size\n");
    }
    signed_unpack(src);

    // imageStore(<image>, ivec2(_<id>), <value>) becomes <array>[gow3Local(_<id>)] = vec4(<value>)
    const std::string_view slots[][2] = {{"rw_history_color", "gow3_hist"},
                                         {"rw_mlsr_output_color", "gow3_out"},
                                         {"rw_recurrent_0", "gow3_rec"}};
    for (const auto& [image, array] : slots) {
        const std::string pattern = "imageStore(" + std::string{image} + ", ivec2(_#), ";
        std::string out;
        size_t count = 0, from = 0;
        for (Match store; (store = find_pattern(src, pattern, from)).begin != npos; from = store.end) {
            out.append(src, from, store.begin - from);
            out.append(array).append("[gow3Local(_").append(store.numbers[0]).append(")] = vec4(");
            count++;
        }
        if (count != 1) {
            // No number after "found" when there is no store: the message of the Perl script.
            die("expected one store to " + std::string{image} + ", found " +
                (count ? std::to_string(count) : "") + "\n");
        }
        src = out.append(src, from);
    }

    const size_t main_at = find_line(src, "void main()\n");
    if (main_at == npos) {
        die("no main\n");
    }
    src.insert(main_at, R"(// gow3: the workgroup's 16x16 output block, stored in contiguous rows after the loop.
shared vec4 gow3_rec[256];
shared vec4 gow3_hist[256];
shared vec4 gow3_out[256];
uint gow3Local(uvec2 p)
{
    uvec2 l = p - gl_WorkGroupID.xy * 16u;
    return l.y * 16u + l.x;
}

)");

    // main is the last function: its closing brace, alone at the start of the last line, ends
    // the file (only white space may follow).
    const size_t brace = src.find_last_not_of(" \t\n\r\f\v");
    if (brace == npos || brace == 0 || src[brace] != '}' || src[brace - 1] != '\n') {
        die("no end of main\n");
    }
    src.resize(brace);
    src += R"(    barrier();
    uvec2 gow3Base = gl_WorkGroupID.xy * 16u;
    for (uint gow3K = 0u; gow3K < 4u; gow3K++)
    {
        uint gow3I = gl_LocalInvocationIndex + 64u * gow3K;
        ivec2 gow3P = ivec2(gow3Base + uvec2(gow3I % 16u, gow3I / 16u));
        imageStore(rw_recurrent_0, gow3P, gow3_rec[gow3I]);
        imageStore(rw_history_color, gow3P, gow3_hist[gow3I]);
        imageStore(rw_mlsr_output_color, gow3P, gow3_out[gow3I]);
    }
}
)";
}

void pass11_guard(std::string& src) {
    const Match bounds = find_pattern(src, "lessThan(uvec3(_#), uvec3(#u, #u, 32u))");
    if (bounds.begin == npos) {
        die("no input bounds check\n");
    }
    bool shared = false;
    for (size_t at = find_word(src, "shared"); at != npos && !shared; at = find_word(src, "shared", at + 1)) {
        shared = at + 6 == src.size() || !is_word(src[at + 6]);
    }
    if (shared || find_word(src, "barrier(") != npos) {
        die("pass uses shared memory or barriers\n");
    }
    signed_unpack(src);

    const std::string_view main_start = "void main()\n{\n";
    const size_t main_at = find_line(src, main_start);
    if (main_at == npos) {
        die("no main\n");
    }
    src.insert(main_at + main_start.size(),
               "    // gow3: invocations past the input width would write the next row's first pixels.\n"
               "    if (gl_GlobalInvocationID.x >= " + bounds.numbers[1] + "u || gl_GlobalInvocationID.y >= " +
                   bounds.numbers[2] + "u)\n"
               "    {\n"
               "        return;\n"
               "    }\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc == 2 ? argv[1] : "";
    if (command != "post-lds" && command != "pass11-guard") {
        std::fputs("usage: fsr4_rewrite post-lds|pass11-guard < pass.comp > rewritten.comp\n", stderr);
        return 2;
    }
#ifdef _WIN32
    // Bytes as they are: no CRLF lines written, no end of input at a 0x1a byte.
    for (FILE* stream : {stdin, stdout, stderr}) {
        _setmode(_fileno(stream), _O_BINARY);
    }
#endif
    std::string src;
    char buffer[65536];
    for (size_t size; (size = std::fread(buffer, 1, sizeof buffer, stdin)) > 0;) {
        src.append(buffer, size);
    }
    if (command == "post-lds") {
        post_lds(src);
    } else {
        pass11_guard(src);
    }
    std::fwrite(src.data(), 1, src.size(), stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
