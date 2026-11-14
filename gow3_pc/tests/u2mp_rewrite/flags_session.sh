# sourced by run.sh for the session test: the old and the new sources read the test's clock (fake_clock.h), and the
# old build's runtime_file_translate (and its stand-in in deps_session.cpp) is renamed so both builds link.
OLD_FLAGS="$OLD_FLAGS -include $here/fake_clock.h -Druntime_file_translate=runtime_file_translate_old"
NEW_FLAGS="$NEW_FLAGS -include $here/fake_clock.h"
