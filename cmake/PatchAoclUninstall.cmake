# Renames AOCL-FFTZ's global custom target "uninstall" to "aoclfftz_uninstall"
# so it does not collide with the same-named target defined by other fetched
# projects (e.g. KFR) in the same build. Invoked as a FetchContent PATCH_COMMAND
# from the populated source directory. Idempotent: after the rename the search
# pattern "uninstall" (with a leading quote) no longer matches.
file(READ "CMakeLists.txt" _contents)
string(REPLACE "\"uninstall\"" "\"aoclfftz_uninstall\"" _contents "${_contents}")
file(WRITE "CMakeLists.txt" "${_contents}")
