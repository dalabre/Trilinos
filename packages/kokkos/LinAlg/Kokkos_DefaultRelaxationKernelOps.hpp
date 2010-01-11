//@HEADER
// ************************************************************************
// 
//          Kokkos: Node API and Parallel Node Kernels
//              Copyright (2009) Sandia Corporation
// 
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
// 
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//  
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//  
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
// Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
// 
// ************************************************************************
//@HEADER

#ifndef KOKKOS_DEFAULTRELAXATION_KERNELOPS_HPP
#define KOKKOS_DEFAULTRELAXATION_KERNELOPS_HPP

#include <Teuchos_ArrayRCP.hpp>
#include <Teuchos_DataAccess.hpp>
#include <Teuchos_TestForException.hpp>
#include <Teuchos_TypeNameTraits.hpp>
#include <Teuchos_ScalarTraits.hpp>
#include <stdexcept>

#include "Kokkos_ConfigDefs.hpp"
#include "Kokkos_CrsMatrix.hpp" 
#include "Kokkos_CrsGraph.hpp" 
#include "Kokkos_MultiVector.hpp"
#include "Kokkos_NodeHelpers.hpp"
#include "Kokkos_DefaultArithmetic.hpp"

#include <stdio.h>

#ifndef KERNEL_PREFIX
  #define KERNEL_PREFIX
#endif

#ifdef __CUDACC__
#include <Teuchos_ScalarTraitsCUDA.hpp>
#else
#include <Teuchos_ScalarTraits.hpp>
#endif



namespace Kokkos {

  // Extract Matrix Diagonal for Type 1 storage
  template <class Scalar, class Ordinal>
  struct ExtractDiagonalOp1 {

    // mat data
    const size_t  *offsets;
    const Ordinal *inds;
    const Scalar  *vals;
    Scalar * diag;
    size_t numRows;


    inline KERNEL_PREFIX void execute(size_t row) {
      for (size_t c=offsets[row];c<offsets[row+1];c++) {
	if(row==(size_t)inds[c]) {
	  diag[row]=vals[c];
	  break;
	}
      }
    }
  };

  // Extract Matrix Diagonal for Type 2 storage
  template <class Scalar, class Ordinal>
  struct ExtractDiagonalOp2 {

    // mat data
    const Ordinal * const * inds_beg;
    const Scalar  * const * vals_beg;
    const size_t  *         numEntries;
    Scalar * diag;
    size_t numRows;

    inline KERNEL_PREFIX void execute(size_t row) {
      const Scalar  *curval = vals_beg[row];
      const Ordinal *curind = inds_beg[row];
      for (size_t j=0;j<numEntries[row];j++) {
	if(row==(size_t)curind[j]){
	  diag[row]=curval[j];
	  break;
	}
      }
    }
  };


  /************************************************************************************/
  /********************************* Jacobi Kernels ***********************************/
  /************************************************************************************/
  // Jacobi for Type 1 storage.
  template <class Scalar, class Ordinal>
  struct DefaultJacobiOp1 {
    const size_t  *offsets;
    const Ordinal *inds;
    const Scalar  *vals;
    const Scalar  *diag;
    size_t numRows;
    // vector data (including multiple rhs)
    Scalar       *x;
    const Scalar *x0;
    const Scalar *b;
    Scalar damping_factor;
    size_t xstride, bstride;

    inline KERNEL_PREFIX void execute(size_t i) {
      const size_t row  = i % numRows;
      const size_t rhs  = (i - row) / numRows;
      Scalar       *xj  = x + rhs * xstride;
      const Scalar *x0j = x0 + rhs * xstride;
      const Scalar *bj  = b + rhs * bstride;

      Scalar tmp = bj[row];
      for (size_t c=offsets[row];c<offsets[row+1];c++) {
	tmp -= vals[c] * x0j[inds[c]];
      }
      xj[row]=x0j[row]+damping_factor*tmp/diag[row];
    }
  };


  // Jacobi for Type 2 storage.
  template <class Scalar, class Ordinal>
  struct DefaultJacobiOp2 {
    // mat data
    const Ordinal * const * inds_beg;
    const Scalar  * const * vals_beg;
    const size_t  *         numEntries;
    const Scalar  *diag;
    size_t numRows;
    // vector data (including multiple rhs)    
    Scalar        *x;
    const Scalar *x0;
    const Scalar  *b;
    Scalar damping_factor;
    size_t xstride, bstride;

    inline KERNEL_PREFIX void execute(size_t i) {
      const size_t row = i % numRows;
      const size_t rhs = (i - row) / numRows;
      Scalar       *xj = x + rhs * xstride;
      const Scalar *x0j = x0 + rhs * xstride;
      const Scalar *bj = b + rhs * bstride;
      Scalar tmp = bj[row];
      const Scalar  *curval = vals_beg[row];
      const Ordinal *curind = inds_beg[row];
      for (size_t j=0; j != numEntries[row]; ++j) {
        tmp -= (curval[j]) * x0j[curind[j]];
      }
      xj[row]=x0j[row]+damping_factor*tmp/diag[row];
    }
  };

  /************************************************************************************/
  /************************ Fine-Grain Gauss-Seidel Kernels ***************************/
  /************************************************************************************/

  // Fine-grain "hybrid" Gauss-Seidel for Type 1 storage.
  // Note: This is actually real Gauss-Seidel for a serial node, and hybrid for almost any other kind of node.
  template <class Scalar, class Ordinal>
  struct DefaultFineGrainHybridGaussSeidelOp1 {
    const size_t  *offsets;
    const Ordinal *inds;
    const Scalar  *vals;
    const Scalar  *diag;
    size_t numRows;
    // vector data (including multiple rhs)
    Scalar       *x;
    const Scalar *b;
    Scalar damping_factor;
    size_t xstride, bstride;

    inline KERNEL_PREFIX void execute(size_t i) {
      const size_t row = i % numRows;
      const size_t rhs = (i - row) / numRows;
      Scalar       *xj = x + rhs * xstride;
      const Scalar *bj = b + rhs * bstride;
      Scalar tmp = bj[row];
      for (size_t c=offsets[row];c<offsets[row+1];c++) {
	tmp -= vals[c] * xj[inds[c]];
      }
      xj[row]+=damping_factor*tmp/diag[row];
    }
  };


  // Fine-grain "hybrid" Gauss-Seidel for Type 2 storage.
  // Note: This is actually real Gauss-Seidel for a serial node, and hybrid for almost any other kind of node.
  template <class Scalar, class Ordinal>
  struct DefaultFineGrainHybridGaussSeidelOp2 {
    // mat data
    const Ordinal * const * inds_beg;
    const Scalar  * const * vals_beg;
    const size_t  *         numEntries;
    const Scalar  *diag;
    size_t numRows;
    // vector data (including multiple rhs)    
    Scalar        *x;
    const Scalar  *b;
    Scalar damping_factor;
    size_t xstride, bstride;

    inline KERNEL_PREFIX void execute(size_t i) {
      const size_t row = i % numRows;
      const size_t rhs = (i - row) / numRows;
      Scalar       *xj = x + rhs * xstride;
      const Scalar *bj = b + rhs * bstride;
      Scalar tmp = bj[row];
      const Scalar  *curval = vals_beg[row];
      const Ordinal *curind = inds_beg[row];
      for (size_t j=0; j != numEntries[row]; ++j) {
        tmp -= (curval[j]) * xj[curind[j]];
      }
      xj[row]+=damping_factor*tmp/diag[row];
    }
  };
}// namespace Kokkos

#endif /* KOKKOS_DEFAULTRELAXATION_KERNELOPS_HPP */
