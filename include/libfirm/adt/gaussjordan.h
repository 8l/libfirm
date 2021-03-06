/**
 * @file
 * @brief solves a system of linear equations
 */
#ifndef FIRM_ADT_GAUSSJORDAN_H
#define FIRM_ADT_GAUSSJORDAN_H

#include "../begin.h"

/**
 * @ingroup algorithms
 * @defgroup gassjordan  Gauss Jordan Elimination
 * Solves a system of linear equations
 * @{
 */

/**
 * solves a system of linear equations and returns 0 if successful
 *
 * @param A       the linear equations as matrix
 * @param b       the result vector, will contain the result if successful
 * @param nsize   the size of the equation system
 */
FIRM_API int firm_gaussjordansolve(double *A, double *b, int nsize);

/** @} */

#include "../end.h"

#endif
