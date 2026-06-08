#include "ccdeseq2/numeric_backend.hpp"

#include "ccdeseq2/optimize.hpp"

namespace ccdeseq2 {

std::map<std::string, std::string> numeric_backend_metadata() {
  std::map<std::string, std::string> metadata{
      {"linear_algebra_backend",
#ifdef FLASHDEG_HAVE_EIGEN
#ifdef FLASHDEG_HAVE_OPENBLAS
       "Eigen+OpenBLAS"
#elif defined(FLASHDEG_HAVE_BLAS_LAPACK)
       "Eigen+BLAS/LAPACK"
#else
       "Eigen"
#endif
#else
       "local-cholesky"
#endif
      },
      {"special_function_backend",
#ifdef FLASHDEG_HAVE_BOOST_MATH
       "Boost.Math"
#else
       "local-special-wrappers"
#endif
      },
      {"optimizer_backend", optimizer_backend_name()},
      {"blas_lapack_backend",
#ifdef FLASHDEG_HAVE_OPENBLAS
       "OpenBLAS"
#elif defined(FLASHDEG_HAVE_BLAS_LAPACK)
       "BLAS/LAPACK"
#else
       "none"
#endif
      },
  };
#ifdef FLASHDEG_HAVE_EIGEN
  metadata["eigen_available"] = "true";
#else
  metadata["eigen_available"] = "false";
#endif
#ifdef FLASHDEG_HAVE_BOOST_MATH
  metadata["boost_math_available"] = "true";
#else
  metadata["boost_math_available"] = "false";
#endif
#ifdef FLASHDEG_HAVE_BLAS_LAPACK
  metadata["blas_lapack_available"] = "true";
#else
  metadata["blas_lapack_available"] = "false";
#endif
#ifdef FLASHDEG_HAVE_OPENBLAS
  metadata["openblas_available"] = "true";
#else
  metadata["openblas_available"] = "false";
#endif
  return metadata;
}

}  // namespace ccdeseq2
