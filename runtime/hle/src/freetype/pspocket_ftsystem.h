/* pspocket's system layer for FreeType 2.2.1 (D-525): what FreeType's ftsystem.c does elsewhere. */
#ifndef PSPOCKET_FTSYSTEM_H
#define PSPOCKET_FTSYSTEM_H

#include <ft2build.h>
#include FT_SYSTEM_H
#include FT_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* FT_Stream_Open's work: set by pspocket's cell_FreeType2, which opens the guest's files through cellFs as Sony's
   libfreetype does. Without it FT_Stream_Open answers FT_Err_Cannot_Open_Resource. */
extern FT_Error (*pspocket_ft_stream_open)(FT_Stream stream, const char* filepathname);

#ifdef __cplusplus
}
#endif

#endif
