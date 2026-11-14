# sourced by run.sh for the progression test: time() is the test's (fake_time.h), and the C runtime functions the
# sources call get an _old name in the old build (deps_progression.cpp defines both).
OLD_FLAGS="$OLD_FLAGS -include $here/fake_time.h -Druntime_file_user_dir=runtime_file_user_dir_old -Druntime_file_translate=runtime_file_translate_old"
NEW_FLAGS="$NEW_FLAGS -include $here/fake_time.h"
