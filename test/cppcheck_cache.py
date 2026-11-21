# Extra script for [env] in platformio.ini.
#
# check_flags gives cppcheck --cppcheck-build-dir=.pio/cppcheck-cache, so it
# keeps its results for each source file there and only checks a file again
# when the file or a header it includes has changed. That makes `pio check`
# about three times faster when only a few files have changed, with the same
# results. cppcheck refuses to start if the folder is missing, though, and
# .pio is not in git, so this script creates it.
#
# `pio check` runs the [env] extra scripts while it reads the build settings,
# before cppcheck starts, so the folder is always there in time.

import os

Import("env")

os.makedirs(os.path.join(env["PROJECT_DIR"], ".pio", "cppcheck-cache"), exist_ok=True)
