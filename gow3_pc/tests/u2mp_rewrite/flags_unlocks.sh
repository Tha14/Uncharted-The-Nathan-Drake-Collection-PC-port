# sourced by run.sh for the unlocks test: the old build's runtime_file_translate (and its stand-in in deps_unlocks.cpp)
# is renamed so both builds link against the same recording stand-in.
OLD_FLAGS="$OLD_FLAGS -Druntime_file_translate=runtime_file_translate_old"
