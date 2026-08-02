#include <port/mpconfigport_common.h>

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)

#define MICROPY_GCREGS_SETJMP               (1)

#define MICROPY_NLR_SETJMP                  (1)
#define MICROPY_PY_GC                       (1)
#define MICROPY_PY_SYS                      (1)

#define MICROPY_PY_SYS_PLATFORM             "esp32s3"

#define MICROPY_PY_BUILTINS_FLOAT           (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)

#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_CPYTHON_COMPAT              (1)

#define MICROPY_PY_TIME                     (1)

#define MICROPY_PY_TIME_TIME_TIME_NS        (0)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)

#define MICROPY_PY_ARRAY                    (1)
#define MICROPY_PY_COLLECTIONS              (1)
#define MICROPY_PY_BUILTINS_STR_COUNT       (1)
#define MICROPY_PY_BUILTINS_SLICE           (1)
#define MICROPY_PY_BUILTINS_ENUMERATE       (1)
#define MICROPY_PY_BUILTINS_REVERSED        (1)
#define MICROPY_PY_BUILTINS_SET             (1)

#define MICROPY_ENABLE_EXTERNAL_IMPORT      (0)

#define MICROPY_PY_IO                       (0)
#define MICROPY_PY_SYS_STDFILES             (0)
#define MICROPY_PY_BUILTINS_HELP            (0)
#define MICROPY_PY_BUILTINS_INPUT           (0)

#define MICROPY_GC_ALLOC_THRESHOLD          (1)

#define MICROPY_ENABLE_SCHEDULER            (1)
#define MICROPY_KBD_EXCEPTION               (1)

void mp_hal_set_interrupt_char(int c);
