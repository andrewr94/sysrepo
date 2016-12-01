#!/bin/sh
set -e

COREFILES=$(find . -name "core*")

for CORE in ${COREFILES}
do
    echo -e "\n\n>>> Core file '${CORE}' was found: "
    cat "${CORE}"
    EXE=$(file $CORE | sed -n "s/^.*, from '\(.\+\)'$/\1/p")
    gdb -ex "core ${CORE}" -ex "thread apply all bt" -batch --args ${EXE}
    gdb -ex "core ${CORE}" -ex "thread apply all bt" -batch --args sysrepoctl
done
