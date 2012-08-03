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
 * @brief       This file implements the ia32 node emitter.
 * @author      Christian Wuerdig, Matthias Braun
 */
#ifndef FIRM_BE_IA32_IA32_EMITTER_H
#define FIRM_BE_IA32_IA32_EMITTER_H

#include "irnode.h"

#include "bearch.h"

#include "bearch_ia32_t.h"

/**
 * fmt  parameter               output
 * ---- ----------------------  ---------------------------------------------
 * %%                           %
 * %AF  <node>                  address mode or x87 register
 * %AM  <node>                  address mode of the node
 * %AR  arch_register_t const*  address mode of the node or register
 * %ASx <node>                  address mode of the node or source register x
 * %B   <node>                  operands for binary operation
 * %Dx  <node>                  destination register x
 * %Fx  <node>                  x87 register x
 * %FM  <node>                  x87 mode suffix
 * %FX  <node>                  SSE mode suffix
 * %I   <node>                  immediate of the node
 * %L   <node>                  control flow target of the node
 * %M   <node>                  mode suffix of the node
 * %Px  <node>                  condition code
 * %PX  int                     condition code
 * %R   arch_register_t const*  register
 * %Sx  <node>                  source register x
 * %s   char const*             string
 * %u   unsigned int            unsigned int
 * %d   signed int              signed int
 *
 * x starts at 0
 * # modifier for %ASx, %D, %R, and %S uses ls mode of node to alter register width
 * # modifier for %M for extend suffix
 * * modifier does not prefix immediates with $, but AM with *
 * l modifier for %lu and %ld
 * > modifier to output high 8bit register (ah, bh)
 * < modifier to output low 8bit register (al, bl)
 */
void ia32_emitf(ir_node const *node, char const *fmt, ...);

void ia32_gen_routine(ir_graph *irg);
void ia32_gen_binary_routine(ir_graph *irg);

/** Initializes the Emitter. */
void ia32_init_emitter(void);
#endif
