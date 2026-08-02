# Wrapper around MicroPython's embed.mk that adds extmod modules to the build.
#
# The embed port only scans py/ for MP_REGISTER_MODULE and for qstrs, so an
# extmod module copied in afterwards compiles and links but is invisible at
# runtime ("ImportError: no module named 'time'") and its method names have no
# qstrs. Extending SRC_QSTR here, *before* including embed.mk, works because
# py.mk appends to it with += — a value passed on the make command line would
# instead suppress that append and lose the core qstrs.

SRC_QSTR += $(MICROPYTHON_TOP)/extmod/modtime.c

include $(MICROPYTHON_TOP)/ports/embed/embed.mk
