/* The Win32 stub layer, compiled for i386.
 *
 * Reuses winstubs.h wholesale: it is written with explicit-width types, and the
 * pointer-sized Windows types (SIZE_T, UINT_PTR) are spelled size_t/uintptr_t
 * so they narrow correctly here. Only two things change:
 *
 *   MS   -- the Win32 calling convention is stdcall on i386, not the single
 *           x86-64 convention the 64-bit loader can assume
 *   GUI  -- the window and DirectWrite layers are x86-64 only for now, so this
 *           build is audio-only
 */
#ifndef PELOAD_WINSTUBS32_H
#define PELOAD_WINSTUBS32_H

#define MS WINAPI_
/* GUI layer enabled: see how far it is from building at 32-bit */

/* winstubs.h refers to the loader's TEB through g_teb->slots[], which pe32.c
 * provides with the same shape. */
#include "winstubs.h"

#endif /* PELOAD_WINSTUBS32_H */
