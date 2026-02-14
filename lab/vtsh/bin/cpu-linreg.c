#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { FIVE = 5, TEN = 10, BUFFER_SIZE = 256 };

const double TEN_D = 10.0;
const double Y_COFF = 2.5;

double random_double(unsigned int* seed, double min, double max) {
  return min + ((rand_r(seed) / (double)RAND_MAX) * (max - min));
}

void linear_regression(
    const double* xxx,
    const double* yyy,
    const int n,
    double* slope,
    double* intercept
) {
  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_xy = 0.0;
  double sum_xx = 0.0;

  // метод наименьших квадратов
  for (int i = 0; i < n; i++) {
    sum_x += xxx[i];
    sum_y += yyy[i];
    sum_xy += xxx[i] * yyy[i];
    sum_xx += xxx[i] * xxx[i];
  }

  double denominator = (n * sum_xx - sum_x * sum_x);
  if (denominator == 0) {
    *slope = 0.0;
    *intercept = 0.0;
    (void
    )fprintf(stderr, "Ошибка: деление на ноль в расчетах линейной регрессии\n");
    return;
  }

  *slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
  *intercept = (sum_y - (*slope) * sum_x) / n;
}

void measure_performance(
    const int num_points,
    const int iterations,
    double* slope,
    double* intercept,
    double range_min,
    double range_max
) {
  double* xxx = malloc(num_points * sizeof(double));
  double* yyy = malloc(num_points * sizeof(double));

  if (xxx == NULL || yyy == NULL) {
    (void)fprintf(stderr, "Ошибка выделения памяти!\n");
    free(xxx);
    free(yyy);
    return;
  }

  unsigned int seed = (unsigned int)time(NULL);

  const clock_t start_time = clock();

  for (int i = 0; i < iterations; i++) {
    for (int j = 0; j < num_points; j++) {
      xxx[j] = random_double(&seed, range_min, range_max);
      yyy[j] = Y_COFF * xxx[j] + TEN + random_double(&seed, -TEN_D, TEN_D);
    }

    linear_regression(xxx, yyy, num_points, slope, intercept);
  }

  const clock_t end_time = clock();
  const double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
  printf(
      "Время выполнения для %d точек и %d итераций: %.6f секунд\n",
      num_points,
      iterations,
      elapsed_time
  );

  free(xxx);
  free(yyy);
}

int main(const int argc, char* argv[]) {
  if (argc != FIVE) {
    (void)fprintf(
        stderr,
        "Usage: %s <num_points> <iterations> <range_min> <range_max>\n",
        argv[0]
    );
    return 1;
  }

  char* endptr = NULL;
  const long num_points_long = strtol(argv[1], &endptr, 10);
  if (*endptr != '\0' || num_points_long > INT_MAX ||
      num_points_long < INT_MIN) {
    (void)fprintf(stderr, "Ошибка: неверный ввод для num_points\n");
    return 1;
  }
  const int num_points = (int)num_points_long;

  const long iterations_long = strtol(argv[2], &endptr, 10);
  if (*endptr != '\0' || iterations_long > INT_MAX ||
      iterations_long < INT_MIN) {
    (void)fprintf(stderr, "Ошибка: неверный ввод для iterations\n");
    return 1;
  }
  const int iterations = (int)iterations_long;

  char* range_endptr = NULL;
  double range_min = strtod(argv[3], &range_endptr);
  double range_max = strtod(argv[4], &range_endptr);
  if (*range_endptr != '\0') {
    (void
    )fprintf(stderr, "Ошибка: неверный ввод для range_min или range_max\n");
    return 1;
  }

  double slope = 0.0;
  double intercept = 0.0;

  measure_performance(
      num_points, iterations, &slope, &intercept, range_min, range_max
  );

  printf("Коэффициенты линейной регрессии:\n");
  printf("Угловой коэффициент (slope): %.6f\n", slope);
  printf("Свободный член (intercept): %.6f\n", intercept);

  return 0;
}