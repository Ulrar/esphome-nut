/* Shim for upstream "nut_bool.h". */
#pragma once

#ifdef __cplusplus
/* bool/true/false are built in */
#else
# include <stdbool.h>
#endif
