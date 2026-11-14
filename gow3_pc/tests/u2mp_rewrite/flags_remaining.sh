# sourced by run.sh for the remaining test: the old build's C text-input calls (clantag.cpp) and their stand-ins in
# deps_remaining.cpp get their own names, so both builds link against their own recording stand-ins
OLD_FLAGS="$OLD_FLAGS -Dgow3gpu_text_input_begin=gow3gpu_text_input_begin_old -Dgow3gpu_text_input_poll=gow3gpu_text_input_poll_old"
