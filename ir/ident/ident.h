/* Declarations for ident.
   Copyright (C) 1995, 1996 Markus Armbruster */

/* Copyright (C) 1998 - 2000 by Universitaet Karlsruhe
* All rights reserved.
*
* Authors: Martin Trapp, Christian Schaefer
*/

/* $Id$ */

# ifndef _IDENT_H_
# define _IDENT_H_

# include <stdio.h>
# include <assert.h>
# include "firm_common.h"

/**
 *
 *   - identifiers in the firm library
 *  Identifiers are used in the firm library. This is the interface to it.
 */

/* Identifiers */
/**
 *
 *  the abstract data type ident
 */
typedef const struct set_entry ident;

/**
 *
 *  store a string and create an ident
 *  Stores a string in the ident module and returns a handle for the string.
 *  Copies the string.
 *  @param str - the string (or whatever) which shall be stored
 *  @param len - the length of the data in bytes
 *  @return id - a handle for the generated ident
 * @see id_to_str, id_to_strlen
 * @see
 */
INLINE ident      *id_from_str (const char *str, int len);

/**
 *
 *  return a string represented by an ident
 *  Returns the string cp represented by id. This string cp is not
 *  Null terminated!  The string may not be changed.
 *  @param id - the ident
 *  @return cp - a string
 * @see id_from_str, id_to_strlen
 * @see
 */
INLINE const char *id_to_str   (ident *id);

/**
 *
 *  return the length of a string represented by an ident
 *  Returns the length of string represented by id.
 *  @param id - the ident
 *  @return len - the length of the string
 * @see id_from_str, id_to_str
 * @see
 */
INLINE int  id_to_strlen(ident *id);

/**
 *
 *
 *  Returns true if prefix is prefix of id.
 *  @param prefix - the prefix
 *  @param id - the ident
 * @see id_from_str, id_to_str, id_is_prefix
 * @see
 */
/*  */
int id_is_prefix (ident *prefix, ident *id);

/**
 *
 *
 *  Returns true if suffix is suffix of id.
 *  @param suffix - the suffix
 *  @param id - the ident
 * @see id_from_str, id_to_str, id_is_prefix
 * @see
 */
/*  */
int id_is_suffix (ident *suffix, ident *id);

/**
 *
 *
 *  Prints the ident to stdout.
 *  @param The ident to print.
 * @see id_from_str, id_to_str, id_is_prefix, fprint_id
 * @see
 */
/*  */
int print_id (ident *id);

/**
 *
 *
 *  Prints the ident to the file passed.
 *  @param The ident to print and the file.
 * @see id_from_str, id_to_str, id_is_prefix, print_id
 * @see
 */
/*  */
int fprint_id (FILE *F, ident *id);


# endif /* _IDENT_H_ */
