# Copies resources/ next to an executable (cmake -DSOURCE=... -DDESTINATION=... -P copy_resources.cmake),
# leaving out the large sample scenes, which are read where they are (see docs/Demo-Scenes.md) : copying
# them would double gigabytes of data at every build. file(COPY) skips files that haven't changed.
file(COPY "${SOURCE}/" DESTINATION "${DESTINATION}"
    PATTERN "Bistro_v5_2" EXCLUDE
    PATTERN "*.blend1" EXCLUDE)
