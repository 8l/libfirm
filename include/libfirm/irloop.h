/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @brief    Loop datastructure and access functions.
 * @author   Goetz Lindenmaier
 * @date     7.2002
 * @brief
 *  Computes backedges in the control and data flow.
 *
 * @note
 *  Only Block and Phi nodes can have incoming backedges.
 *  Constructs loops data structure: indicates loop nesting.
 */
#ifndef FIRM_ANA_IRLOOP_H
#define FIRM_ANA_IRLOOP_H

#include "firm_types.h"
#include "firm_common.h"
#include "begin.h"

/** @ingroup irana
 * @defgroup ir_loop Loops
 * @{
 */

/** Returns non-zero if the predecessor pos is a backedge. */
FIRM_API int is_backedge(const ir_node *n, int pos);
/** Marks edge pos as a backedge. */
FIRM_API void set_backedge(ir_node *n, int pos);
/** Marks edge pos as a non-backedge. */
FIRM_API void set_not_backedge(ir_node *n, int pos);
/** Returns non-zero if n has backedges. */
FIRM_API int has_backedges(const ir_node *n);
/** Clears all backedge information. */
FIRM_API void clear_backedges(ir_node *n);

/** Loop elements: loop nodes and ir nodes */
typedef union {
	firm_kind *kind; /**< is either k_ir_node or k_ir_loop */
	ir_node  *node;  /**< Pointer to an ir_node element */
	ir_loop  *son;   /**< Pointer to an ir_loop element */
	ir_graph *irg;   /**< Pointer to an ir_graph element (only callgraph loop trees) */
} loop_element;

/** Tests whether a given pointer points to a loop.
 * @note only works reliably if @p thing points to something with a #firm_kind
 * header */
FIRM_API int is_ir_loop(const void *thing);

/** Sets the outermost loop in ir graph as basic access to loop tree. */
FIRM_API void set_irg_loop(ir_graph *irg, ir_loop *l);

/** Returns the root loop info (if exists) for an irg. */
FIRM_API ir_loop *get_irg_loop(const ir_graph *irg);

/** Returns the loop n is contained in.  NULL if node is in no loop. */
FIRM_API ir_loop *get_irn_loop(const ir_node *n);

/** Returns outer loop, itself if outermost. */
FIRM_API ir_loop *get_loop_outer_loop(const ir_loop *loop);
/** Returns nesting depth of this loop */
FIRM_API unsigned get_loop_depth(const ir_loop *loop);

/** Returns the number of elements contained in loop.  */
FIRM_API size_t get_loop_n_elements(const ir_loop *loop);

/** Returns a loop element. A loop element can be interpreted as a
 * kind pointer, an ir_node* or an ir_loop*. */
FIRM_API loop_element get_loop_element(const ir_loop *loop, size_t pos);

/** Returns a unique node number for the loop node to make output
readable. If libfirm_debug is not set it returns the loop cast to
int. */
FIRM_API long get_loop_loop_nr(const ir_loop *loop);

/** A field to connect additional information to a loop. */
FIRM_API void set_loop_link(ir_loop *loop, void *link);
/** Returns field with additional loop information.
 * @see set_loop_link() */
FIRM_API void *get_loop_link(const ir_loop *loop);

/** Constructs backedge information and loop tree for a graph.
 *
 *  The algorithm views the program representation as a pure graph.
 *  It assumes that only block and phi nodes may be loop headers.
 *  The resulting loop tree is a possible visiting order for dataflow
 *  analysis.
 *
 *  This algorithm destoyes the link field of block nodes.
 *
 *  @returns Maximal depth of loop tree.
 *
 *  @remark
 *  One assumes, the Phi nodes in a block with a backedge have backedges
 *  at the same positions as the block.  This is not the case, as
 *  the scc algorithms does not respect the program semantics in this case.
 *  Take a swap in a loop (t = i; i = j; j = t;)  This results in two Phi
 *  nodes.  They form a cycle.  Once the scc algorithm deleted one of the
 *  edges, the cycle is removed.  The second Phi node does not get a
 *  backedge!
 */
FIRM_API int construct_backedges(ir_graph *irg);

/**
 * Construct Intra-procedural control flow loop tree for a IR-graph.
 *
 * This constructs loop information resembling the program structure.
 * It is useful for loop optimizations and analyses, as, e.g., finding
 * iteration variables or loop invariant code motion.
 *
 * This algorithm computes only back edge information for Block nodes, not
 * for Phi nodes.
 *
 * This algorithm destroyes the link field of block nodes.
 *
 * @param irg  the graph
 *
 * @returns Maximal depth of loop tree.
 */
FIRM_API int construct_cf_backedges(ir_graph *irg);

/**
 * Computes Intra-procedural control flow loop tree on demand.
 *
 * @param irg  the graph
 */
FIRM_API void assure_loopinfo(ir_graph *irg);

/**
 * Removes all loop information.
 * Resets all backedges.  Works for any construction algorithm.
 */
FIRM_API void free_loop_information(ir_graph *irg);
/** Removes loop information from all graphs in the current program. */
FIRM_API void free_all_loop_information(void);

/** Tests whether a value is loop invariant.
 *
 * @param n      The node to be tested.
 * @param block  A block node.
 *
 * Returns non-zero, if the node n is not changed in the loop block
 * belongs to or in inner loops of this block. */
FIRM_API int is_loop_invariant(const ir_node *n, const ir_node *block);

/** @} */

#include "end.h"

#endif
