/* pspocket's system layer for FreeType 2.2.1 (D-525), built in place of FreeType's src/base/ftsystem.c: the host's
   memory manager for FT_New_Memory, and FT_Stream_Open handed to pspocket_ft_stream_open, so that FreeType's files
   are the guest's, opened and read through cellFs as Sony's libfreetype opens them (its FT_Stream_Open calls cellFsOpen,
   cellFsLseek and cellFsRead). The interface is FreeType's (include/freetype/ftsystem.h). */

#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include FT_INTERNAL_DEBUG_H
#include FT_INTERNAL_STREAM_H
#include FT_SYSTEM_H
#include FT_ERRORS_H
#include FT_TYPES_H

#include <stdlib.h>

#include "pspocket_ftsystem.h"

FT_Error (*pspocket_ft_stream_open)(FT_Stream stream, const char* filepathname) = 0;

static void* pspocket_ft_alloc(FT_Memory memory, long size)
{
    (void)memory;
    return malloc((size_t)size);
}

static void* pspocket_ft_realloc(FT_Memory memory, long cur_size, long new_size, void* block)
{
    (void)memory;
    (void)cur_size;
    return realloc(block, (size_t)new_size);
}

static void pspocket_ft_free(FT_Memory memory, void* block)
{
    (void)memory;
    free(block);
}

FT_BASE_DEF(FT_Error)
FT_Stream_Open(FT_Stream stream, const char* filepathname)
{
    if (!stream) return FT_Err_Invalid_Stream_Handle;
    if (!pspocket_ft_stream_open) return FT_Err_Cannot_Open_Resource;
    return pspocket_ft_stream_open(stream, filepathname);
}

FT_BASE_DEF(FT_Memory)
FT_New_Memory(void)
{
    FT_Memory memory = (FT_Memory)malloc(sizeof(*memory));
    if (memory) {
        memory->user = 0;
        memory->alloc = pspocket_ft_alloc;
        memory->realloc = pspocket_ft_realloc;
        memory->free = pspocket_ft_free;
    }
    return memory;
}

FT_BASE_DEF(void)
FT_Done_Memory(FT_Memory memory)
{
    memory->free(memory, memory);
}
