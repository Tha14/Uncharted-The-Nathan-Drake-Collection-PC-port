// Portable environment setter for the C++ unit tests.
#ifndef GOW3_TESTS_TEST_ENVIRONMENT_H
#define GOW3_TESTS_TEST_ENVIRONMENT_H
#include <cassert>  // assert
#include <cstdlib>  // setenv / unsetenv, or _putenv_s on Windows
namespace gow3_test_detail {
// Same contract as the C library calls it wraps: 0 on success.
inline int PutEnv(const char* var, const char* text) {
#if defined(_WIN32)
    return _putenv_s(var, text ? text : "");  // an empty value removes the variable on Windows
#else // POSIX
    return text ? setenv(var, text, /*overwrite=*/1) : unsetenv(var);
#endif // _WIN32
}
} // namespace gow3_test_detail
// SetTestEnv(name, text) puts `text` into the process environment; a null `text` removes the
// variable. Any failure stops the test through assert.
inline void SetTestEnv(const char* var, const char* text) {
    [[maybe_unused]] const int status = gow3_test_detail::PutEnv(var, text);
    assert(status == 0 && "SetTestEnv failed");
}
#endif // GOW3_TESTS_TEST_ENVIRONMENT_H
