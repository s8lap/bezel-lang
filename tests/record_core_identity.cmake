# per #4529 (follow-up to #2877): record, AT LINK TIME, which bytes of the
# compiler archive a test binary was linked from.
#
# Run as a POST_BUILD step of every bezel_tests_<group> target (see
# tests/CMakeLists.txt). It writes SHA-256(libbezel_core.a) to <binary>.coreid,
# which tests/support/aot_support.hpp's buildIdentity() reads to decide whether
# the k2 cache identity may key on the archive.
#
# WHY A SCRIPT AND NOT `cmake -E sha256sum`: that prints to stdout, and
# add_custom_command has no portable redirect. file(SHA256) streams the file, so
# a ~50 MB archive costs ~0.1 s and no memory spike.
#
# This must FAIL LOUDLY rather than write nothing: a silently absent record
# degrades the guard to the old mtime ordering, which is precisely the
# false-negative #4529 exists to remove, and it would degrade INVISIBLY.
if(NOT DEFINED BEZEL_CORE_LIB OR NOT DEFINED BEZEL_OUT)
    message(FATAL_ERROR
        "record_core_identity.cmake: -DBEZEL_CORE_LIB=<archive> and "
        "-DBEZEL_OUT=<file> are both required")
endif()
if(NOT EXISTS "${BEZEL_CORE_LIB}")
    message(FATAL_ERROR
        "record_core_identity.cmake: no compiler archive at "
        "'${BEZEL_CORE_LIB}' — it is a link input of the target this step "
        "follows, so its absence here means the build graph moved")
endif()
file(SHA256 "${BEZEL_CORE_LIB}" _bezel_core_digest)
file(WRITE "${BEZEL_OUT}" "${_bezel_core_digest}\n")
