#include <cassert>

extern "C" {
double ddot_(int* n, double* x, int* incx, double* y, int* incy);
void daxpy_(int* n, double* alpha, double* x, int* incx, double* y,
            int* incy);
}

int main() {
  int n = 3;
  int inc = 1;
  double x[] = {1.0, 2.0, 3.0};
  double y[] = {4.0, 5.0, 6.0};

  const double dot = ddot_(&n, x, &inc, y, &inc);
  assert(dot == 32.0);

  double alpha = 2.0;
  daxpy_(&n, &alpha, x, &inc, y, &inc);
  assert(y[0] == 6.0);
  assert(y[1] == 9.0);
  assert(y[2] == 12.0);
  return 0;
}
