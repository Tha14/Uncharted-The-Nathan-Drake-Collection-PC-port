# sourced by run.sh for the gameplay test: the old and the new sources read the test's clock (fake_clock.h)
OLD_FLAGS="$OLD_FLAGS -include $here/fake_clock.h"
NEW_FLAGS="$NEW_FLAGS -include $here/fake_clock.h"
