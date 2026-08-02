# MicroPython, compiled in as a library via its embed port.
#
# micropython_embed/ is generated, not checked in. Regenerate with:
#   make -f $MP/ports/embed/embed.mk MICROPYTHON_TOP=$MP \
#        BUILD=build-embed EMBED_DIR=micropython_embed
# where $MP is a micropython checkout.

# Plain GLOB: CONFIGURE_DEPENDS is rejected here because ESP-IDF evaluates
# component CMakeLists in script mode during early expansion. The glob is
# therefore a configure-time snapshot, and regenerating the package behind it
# gives "undefined reference to `mp_module_time'" with no hint why — so
# regen.sh touches this file afterwards to force a reconfigure.
file(GLOB_RECURSE MP_SRCS ${CMAKE_CURRENT_LIST_DIR}/micropython_embed/*.c)

# Their mphalport.c sends Python's stdout to printf, which would land on the
# serial console no matter who asked. main/py.c provides these symbols instead,
# routed to whichever sink is running the script.
list(FILTER MP_SRCS EXCLUDE REGEX ".*/port/mphalport\\.c$")

idf_component_register(
    SRCS ${MP_SRCS}
    INCLUDE_DIRS
        "."
        "micropython_embed"
        "micropython_embed/port"
)

# MicroPython's own build is warning-clean under its flags, not ESP-IDF's.
# These are its style, not defects, and are not worth patching 136 files over.
# NDEBUG: without it, embed_util.c defines its own __assert_func, which newlib
# already provides — "multiple definition of `__assert_func'".
#
# mp_init: wolfSSL's TomsFastMath exports a function of that name too, and the
# linker cannot have both. Renaming from this side keeps the fix inside our own
# build — the alternative is editing managed_components/, which is regenerated
# on every dependency resolve and gitignored, so the fix would vanish for
# anyone who clones this repo. The macro rewrites the definition in runtime.c
# and every call inside MicroPython consistently; nothing outside the component
# refers to it (we call mp_embed_init).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NDEBUG
    mp_init=mpy_runtime_init
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-type-limits
    -Wno-unused-variable
)
