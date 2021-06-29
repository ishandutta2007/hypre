#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "_hypre_utilities.h"
//#include "_hypre_utilities.hpp"
#include "HYPRE.h"
#include "HYPRE_parcsr_mv.h"

#include "HYPRE_IJ_mv.h"
#include "_hypre_IJ_mv.h"
#include "HYPRE_parcsr_ls.h"
#include "_hypre_parcsr_mv.h"
#include "HYPRE_krylov.h"

#if defined(HYPRE_USING_GPU)
#include "_hypre_utilities.hpp"
#endif

#if defined(HYPRE_USING_UMPIRE)
#include "umpire/interface/umpire.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if !defined GMRES_DEBUG
#define GMRES_DEBUG 0
#endif

   extern
HYPRE_Int apply_GMRES_poly(hypre_ParCSRMatrix *A,
                            HYPRE_Real *coefs_real,
                            HYPRE_Real *coefs_imag,
                            HYPRE_Int order,            /* polynomial order */
                            hypre_ParVector *x,
                            hypre_ParVector *tmp,
                            hypre_ParVector *prod,
                            hypre_ParVector *p);

extern HYPRE_Int hypre_ParCSRRelax_GMRES_Setup(hypre_ParCSRMatrix *A, /* matrix to relax with */
    HYPRE_Int degree,
    HYPRE_Real **coefs_real_ptr,
    HYPRE_Real **coefs_imag_ptr
    );

HYPRE_Int
BuildParFromOneFile( HYPRE_Int                  argc,
                     char                *argv[],
                     HYPRE_Int                  arg_index,
                     HYPRE_Int                  num_functions,
                     HYPRE_ParCSRMatrix  *A_ptr     )
{
   char               *filename;

   HYPRE_CSRMatrix  A_CSR = NULL;
   HYPRE_BigInt       *row_part = NULL;
   HYPRE_BigInt       *col_part = NULL;

   HYPRE_Int          myid, numprocs;
   HYPRE_Int          i, rest, size, num_nodes, num_dofs;

   /*-----------------------------------------------------------
    * Initialize some stuff
    *-----------------------------------------------------------*/

   hypre_MPI_Comm_rank(hypre_MPI_COMM_WORLD, &myid );
   hypre_MPI_Comm_size(hypre_MPI_COMM_WORLD, &numprocs );

   /*-----------------------------------------------------------
    * Parse command line
    *-----------------------------------------------------------*/

   if (arg_index < argc)
   {
      filename = argv[arg_index];
   }
   else
   {
      hypre_printf("Error: No filename specified \n");
      exit(1);
   }

   /*-----------------------------------------------------------
    * Print driver parameters
    *-----------------------------------------------------------*/

   if (myid == 0)
   {
      hypre_printf("  FromFile: %s\n", filename);

      /*-----------------------------------------------------------
       * Generate the matrix
       *-----------------------------------------------------------*/

      A_CSR = HYPRE_CSRMatrixRead(filename);
   }

   if (myid == 0 && num_functions > 1)
   {
      HYPRE_CSRMatrixGetNumRows(A_CSR, &num_dofs);
      num_nodes = num_dofs/num_functions;
      if (num_dofs == num_functions*num_nodes)
      {
         row_part = hypre_CTAlloc(HYPRE_BigInt,  numprocs+1, HYPRE_MEMORY_HOST);

         row_part[0] = 0;
         size = num_nodes/numprocs;
         rest = num_nodes-size*numprocs;
         for (i = 0; i < rest; i++)
         {
            row_part[i+1] = row_part[i] + (size + 1)*num_functions;
         }
         for (i = rest; i < numprocs; i++)
         {
            row_part[i+1] = row_part[i]+size*num_functions;
         }

         col_part = row_part;
      }
   }

   HYPRE_CSRMatrixToParCSRMatrix(hypre_MPI_COMM_WORLD, A_CSR, row_part, col_part, A_ptr);

   if (myid == 0)
   {
      HYPRE_CSRMatrixDestroy(A_CSR);
   }

   return (0);
}

int main(int argc, char** argv) {
   hypre_MPI_Init(&argc, &argv);

   HYPRE_ParCSRMatrix A = NULL;
   HYPRE_Int           num_procs, myid;
   HYPRE_Int           ierr;
   HYPRE_Int           i;
   HYPRE_ExecutionPolicy default_exec_policy = HYPRE_EXEC_DEVICE;
   HYPRE_MemoryLocation  memory_location     = HYPRE_MEMORY_DEVICE;



   hypre_MPI_Comm_size(hypre_MPI_COMM_WORLD, &num_procs );
   hypre_MPI_Comm_rank(hypre_MPI_COMM_WORLD, &myid );

   BuildParFromOneFile(argc, argv, 1, 0, &A);

   MPI_Comm            comm = hypre_MPI_COMM_WORLD;
   HYPRE_BigInt        M, N, big_i;
   HYPRE_Int           local_num_rows, local_num_cols;
   HYPRE_BigInt        first_local_row, last_local_row;
   HYPRE_BigInt        first_local_col, last_local_col;
   HYPRE_IJMatrix      ij_A = NULL;
   HYPRE_IJVector      ij_b = NULL;
   HYPRE_IJVector      ij_x = NULL;
   HYPRE_IJVector      *ij_rbm;


   ierr = HYPRE_ParCSRMatrixGetLocalRange( A,
                                           &first_local_row, &last_local_row ,
                                           &first_local_col, &last_local_col );

   local_num_rows = (HYPRE_Int)(last_local_row - first_local_row + 1);
   local_num_cols = (HYPRE_Int)(last_local_col - first_local_col + 1);



   if (myid == 0)
   {
      hypre_printf("  RHS vector has unit components\n");
      hypre_printf("  Initial guess is 0\n");
   }

   void* object;
   HYPRE_Real *values_h = hypre_CTAlloc(HYPRE_Real, local_num_rows, HYPRE_MEMORY_HOST);
   for (i = 0; i < local_num_rows; i++)
   {
      values_h[i] = 1.0;
   }

   /* RHS */
   HYPRE_ParVector b; 
   HYPRE_ParVector x; 

   HYPRE_IJVectorCreate(hypre_MPI_COMM_WORLD, first_local_row, last_local_row, &ij_b);
   HYPRE_IJVectorSetObjectType(ij_b, HYPRE_PARCSR);
   HYPRE_IJVectorInitialize_v2(ij_b, memory_location);
   HYPRE_IJVectorSetValues(ij_b, local_num_rows, NULL, values_h);
   HYPRE_IJVectorAssemble(ij_b);
   ierr = HYPRE_IJVectorGetObject( ij_b, &object );
   b = (HYPRE_ParVector) object;

   /* Initial guess */
   HYPRE_IJVectorCreate(hypre_MPI_COMM_WORLD, first_local_col, last_local_col, &ij_x);
   HYPRE_IJVectorSetObjectType(ij_x, HYPRE_PARCSR);
   HYPRE_IJVectorInitialize_v2(ij_x, memory_location);
   HYPRE_IJVectorSetValues(ij_x, local_num_cols, NULL, values_h);
   HYPRE_IJVectorAssemble(ij_x);
   ierr = HYPRE_IJVectorGetObject( ij_x, &object );
   x = (HYPRE_ParVector) object;

   hypre_TFree(values_h, HYPRE_MEMORY_HOST);


   hypre_ParVector* prod = hypre_ParVectorCreate(hypre_ParCSRMatrixComm(A),
                                 hypre_ParCSRMatrixGlobalNumRows(A),
                                 hypre_ParCSRMatrixRowStarts(A));
   hypre_ParVectorInitialize_v2(prod, memory_location);
   hypre_ParVectorSetPartitioningOwner(prod, 0);

   hypre_ParVector* tmp = hypre_ParVectorCreate(hypre_ParCSRMatrixComm(A),
                                 hypre_ParCSRMatrixGlobalNumRows(A),
                                 hypre_ParCSRMatrixRowStarts(A));
   hypre_ParVectorInitialize_v2(tmp, memory_location);
   hypre_ParVectorSetPartitioningOwner(tmp, 0);

   hypre_ParVector* p = hypre_ParVectorCreate(hypre_ParCSRMatrixComm(A),
                                 hypre_ParCSRMatrixGlobalNumRows(A),
                                 hypre_ParCSRMatrixRowStarts(A));
   hypre_ParVectorInitialize_v2(p, memory_location);
   hypre_ParVectorSetPartitioningOwner(p, 0);

   HYPRE_Real* coefs_real_ptr = NULL;
   HYPRE_Real* coefs_imag_ptr = NULL;
   HYPRE_Int order = 2;

   HYPRE_Int num_rows = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(A));
   printf("Num rows: %i\n", num_rows);

   hypre_ParCSRRelax_GMRES_Setup(A, order, &coefs_real_ptr, &coefs_imag_ptr);
   printf("B\n");
   HYPRE_ParVectorSetConstantValues(b, 1);
   HYPRE_Real norm;
   HYPRE_ParVectorInnerProd(b,b,&norm);
   norm = 1./sqrt(norm);
   ierr = HYPRE_ParVectorScale(norm, b);
#if GMRES_DEBUG
   PrintVector(b, num_rows);
#endif
   apply_GMRES_poly(A, coefs_real_ptr, coefs_imag_ptr, order, b, tmp, prod, p);
   printf("p\n");
   PrintVector(p, num_rows);
}




#ifdef __cplusplus
}
#endif