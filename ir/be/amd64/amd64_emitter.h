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
 * @brief    declarations for emit functions
 */
#ifndef FIRM_BE_amd64_amd64_EMITTER_H
#define FIRM_BE_amd64_amd64_EMITTER_H

#include "irargs_t.h"
#include "irnode.h"
#include "debug.h"

#include "bearch.h"
#include "beemitter.h"

#include "bearch_amd64_t.h"

/**
 * fmt  parameter               output
 * ---- ----------------------  ---------------------------------------------
 * %%                           %
 * %C   <node>                  immediate value
 * %Dx  <node>                  destination register x
 * %E   ir_entity const*        entity
 * %L   <node>                  control flow target
 * %O   <node>                  offset
 * %R   arch_register_t const*  register
 * %Sx  <node>                  source register x
 * %S*  <node>, int             source register
 * %d   signed int              signed int
 * %s   char const*             string
 * %u   unsigned int            unsigned int
 *
 * x starts at 0
 */
void amd64_emitf(ir_node const *node, char const *fmt, ...);

void amd64_gen_routine(ir_graph *irg);

#endif
