// Original Cell declaration of the Windows event-token ABI, for MinGW SDKs
// that omit this Windows Runtime header. Prefer the platform header when present.
#pragma once
#include <stdint.h>
typedef struct EventRegistrationToken { int64_t value; } EventRegistrationToken;
#ifdef __cplusplus
static_assert(sizeof(EventRegistrationToken)==8);
#endif
