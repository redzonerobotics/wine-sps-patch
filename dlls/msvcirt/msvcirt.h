/*
 * Copyright 2015 Iván Matellanes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "stdlib.h"
#include "windef.h"
#include "cxx.h"

typedef LONG streamoff;
typedef LONG streampos;

typedef enum {
    SEEKDIR_beg = 0,
    SEEKDIR_cur = 1,
    SEEKDIR_end = 2
} ios_seek_dir;

extern void* (__cdecl *MSVCRT_operator_new)(SIZE_T);
extern void (__cdecl *MSVCRT_operator_delete)(void*);

void init_exception(void*);