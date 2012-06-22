/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Dwarf debug output support.
 * @author  Matthias Braun
 */
#ifndef FIRM_BE_BEDWARF_H
#define FIRM_BE_BEDWARF_H

#include "beabi.h"

typedef struct parameter_dbg_info_t {
	const ir_entity       *entity;
	const arch_register_t *reg;
} parameter_dbg_info_t;

/** initialize and open debug handle */
void be_dwarf_open(void);

/** close a debug handler. */
void be_dwarf_close(void);

/** start a compilation unit */
void be_dwarf_unit_begin(const char *filename);

/** end compilation unit */
void be_dwarf_unit_end(void);

/** output debug info necessary right before defining a method */
void be_dwarf_method_before(const ir_entity *ent,
                            const parameter_dbg_info_t *infos);

/** output debug info right before beginning to output assembly instructions */
void be_dwarf_method_begin(void);

/** debug for a method end */
void be_dwarf_method_end(void);

/** dump a variable in the global type */
void be_dwarf_variable(const ir_entity *ent);

/** Set "location" in the sourcefile corresponding to the following
 * assembly instructions */
void be_dwarf_location(dbg_info *dbgi);

/** set base register that points to callframe */
void be_dwarf_callframe_register(const arch_register_t *reg);

/** set offset from base register that points to the callframe.
 * Note: callframe is defined as in the dwarf documentation here which is the
 * stackpointer before the call has happened. (Which would be the beginning of
 * the between type in our backend) */
void be_dwarf_callframe_offset(int offset);

/**
 * Indicate at which offset (relative to the CFA) a caller saved register has
 * been saved.
 */
void be_dwarf_callframe_spilloffset(const arch_register_t *reg, int offset);

#endif
