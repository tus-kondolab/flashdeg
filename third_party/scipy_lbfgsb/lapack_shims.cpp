#include <algorithm>
#include <cmath>

namespace {

[[nodiscard]] bool same_char(char value, char upper, char lower) {
  return value == upper || value == lower;
}

}  // namespace

extern "C" {

void dpotrf_(char* uplo, int* n, double* a, int* lda, int* info) {
  *info = 0;
  const int size = *n;
  const int stride = *lda;
  if (size < 0) {
    *info = -2;
    return;
  }
  if (stride < std::max(1, size)) {
    *info = -4;
    return;
  }

  const bool upper = same_char(*uplo, 'U', 'u');
  const bool lower = same_char(*uplo, 'L', 'l');
  if (!upper && !lower) {
    *info = -1;
    return;
  }

  if (upper) {
    for (int j = 0; j < size; ++j) {
      double diag = a[j + j * stride];
      for (int k = 0; k < j; ++k) {
        const double value = a[k + j * stride];
        diag -= value * value;
      }
      if (!(diag > 0.0) || !std::isfinite(diag)) {
        *info = j + 1;
        return;
      }
      a[j + j * stride] = std::sqrt(diag);
      for (int col = j + 1; col < size; ++col) {
        double value = a[j + col * stride];
        for (int k = 0; k < j; ++k) {
          value -= a[k + j * stride] * a[k + col * stride];
        }
        a[j + col * stride] = value / a[j + j * stride];
      }
    }
    return;
  }

  for (int j = 0; j < size; ++j) {
    double diag = a[j + j * stride];
    for (int k = 0; k < j; ++k) {
      const double value = a[j + k * stride];
      diag -= value * value;
    }
    if (!(diag > 0.0) || !std::isfinite(diag)) {
      *info = j + 1;
      return;
    }
    a[j + j * stride] = std::sqrt(diag);
    for (int row = j + 1; row < size; ++row) {
      double value = a[row + j * stride];
      for (int k = 0; k < j; ++k) {
        value -= a[row + k * stride] * a[j + k * stride];
      }
      a[row + j * stride] = value / a[j + j * stride];
    }
  }
}

void dtrtrs_(char* uplo, char* trans, char* diag, int* n, int* nrhs, double* a,
             int* lda, double* b, int* ldb, int* info) {
  *info = 0;
  const int size = *n;
  const int rhs_count = *nrhs;
  const int a_stride = *lda;
  const int b_stride = *ldb;
  if (size < 0) {
    *info = -4;
    return;
  }
  if (rhs_count < 0) {
    *info = -5;
    return;
  }
  if (a_stride < std::max(1, size)) {
    *info = -7;
    return;
  }
  if (b_stride < std::max(1, size)) {
    *info = -9;
    return;
  }

  const bool upper = same_char(*uplo, 'U', 'u');
  const bool lower = same_char(*uplo, 'L', 'l');
  const bool transpose = same_char(*trans, 'T', 't') || same_char(*trans, 'C', 'c');
  const bool unit_diag = same_char(*diag, 'U', 'u');
  if (!upper && !lower) {
    *info = -1;
    return;
  }

  for (int rhs = 0; rhs < rhs_count; ++rhs) {
    double* x = b + rhs * b_stride;
    if (!transpose && upper) {
      for (int i = size - 1; i >= 0; --i) {
        double sum = x[i];
        for (int j = i + 1; j < size; ++j) {
          sum -= a[i + j * a_stride] * x[j];
        }
        if (!unit_diag) {
          const double diagonal = a[i + i * a_stride];
          if (diagonal == 0.0) {
            *info = i + 1;
            return;
          }
          sum /= diagonal;
        }
        x[i] = sum;
      }
    } else if (!transpose && lower) {
      for (int i = 0; i < size; ++i) {
        double sum = x[i];
        for (int j = 0; j < i; ++j) {
          sum -= a[i + j * a_stride] * x[j];
        }
        if (!unit_diag) {
          const double diagonal = a[i + i * a_stride];
          if (diagonal == 0.0) {
            *info = i + 1;
            return;
          }
          sum /= diagonal;
        }
        x[i] = sum;
      }
    } else if (transpose && upper) {
      for (int i = 0; i < size; ++i) {
        double sum = x[i];
        for (int j = 0; j < i; ++j) {
          sum -= a[j + i * a_stride] * x[j];
        }
        if (!unit_diag) {
          const double diagonal = a[i + i * a_stride];
          if (diagonal == 0.0) {
            *info = i + 1;
            return;
          }
          sum /= diagonal;
        }
        x[i] = sum;
      }
    } else {
      for (int i = size - 1; i >= 0; --i) {
        double sum = x[i];
        for (int j = i + 1; j < size; ++j) {
          sum -= a[j + i * a_stride] * x[j];
        }
        if (!unit_diag) {
          const double diagonal = a[i + i * a_stride];
          if (diagonal == 0.0) {
            *info = i + 1;
            return;
          }
          sum /= diagonal;
        }
        x[i] = sum;
      }
    }
  }
}

}  // extern "C"
