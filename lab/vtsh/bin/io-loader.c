#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PERMISSIONS 0666
#define O_DIRECT_FLAG "on"
#define SEQUENCE_ACCESS "sequence"
#define RANDOM_ACCESS "random"
enum { ARGS_COUNT = 8, TEN = 10, FIVE = 5, SIX = 6, SEVEN = 7, SIZE = 256 };

void print_usage(void) {
  printf(
      "Usage: io_loader [operation_mode] [block_size] [block_count] [file] "
      "[range] [direct] [type]\n"
  );
  printf("  operation_mode: read or write\n");
  printf("  block_size: size of each block in bytes\n");
  printf("  block_count: number of blocks to read/write\n");
  printf("  file: path to the file\n");
  printf("  range: range in the format 'start-end' (use 0-0 for full file)\n");
  printf("  direct: on/off to enable O_DIRECT\n");
  printf("  type: sequence or random access\n");
}

int parse_range(const char* range, int* start_offset, int* end_offset) {
  if (strcmp(range, "0-0") != 0) {
    char* endptr1 = NULL;

    long start_offset_long = strtol(range, &endptr1, TEN);
    if (*endptr1 != '-' || *(endptr1 + 1) == '\0') {
      (void)fprintf(stderr, "Invalid range format: %s\n", range);
      return -1;
    }

    if (start_offset_long < INT_MIN || start_offset_long > INT_MAX) {
      (void)fprintf(
          stderr,
          "start_offset is out of range for int: %ld\n",
          start_offset_long
      );
      return -1;
    }
    *start_offset = (int)start_offset_long;

    long end_offset_long = strtol(endptr1 + 1, &endptr1, TEN);
    if (*endptr1 != '\0') {
      (void)fprintf(stderr, "Invalid range format: %s\n", range);
      return -1;
    }

    if (end_offset_long < INT_MIN || end_offset_long > INT_MAX) {
      (void)fprintf(
          stderr, "end_offset is out of range for int: %ld\n", end_offset_long
      );
      return -1;
    }
    *end_offset = (int)end_offset_long;

    if (*start_offset < 0 || *end_offset < *start_offset) {
      (void)fprintf(stderr, "Invalid range format: %s\n", range);
      return -1;
    }
  }
  return 0;
}

int parse_block_size_and_count(
    char* block_size_str2,
    int* block_size2,
    char* block_count_str2,
    int* block_count2
) {
  char* endptr = NULL;

  const long block_size_long = strtol(block_size_str2, &endptr, TEN);
  if (*endptr != '\0' || block_size_long <= 0 || block_size_long > INT32_MAX) {
    (void)fprintf(stderr, "Invalid block_size: %s\n", block_size_str2);
    return -1;
  }
  *block_size2 = (int)block_size_long;

  const long block_count_long = strtol(block_count_str2, &endptr, TEN);
  if (*endptr != '\0' || block_count_long <= 0 ||
      block_count_long > INT32_MAX) {
    (void)fprintf(stderr, "Invalid block_count: %s\n", block_count_str2);
    return -1;
  }
  *block_count2 = (int)block_count_long;

  return 0;
}

int open_file(const char* file_path, int iii, const char* direct_flag1) {
  iii += 1;
  unsigned int flags = O_RDWR | O_CREAT | O_LARGEFILE;
  if (strcmp(direct_flag1, O_DIRECT_FLAG) == 0) {
    flags |= O_DIRECT;
  }

  int filed = open(file_path, (int)flags, DEFAULT_PERMISSIONS);
  if (filed == -1) {
    perror("Failed to open file");
    return -1;
  }

  return filed;
}
void perform_read_write(
    const int filed2,
    void* buffer1,
    int block_size1,
    const int block_count1,
    const char* operation_mode
) {
  for (int i = 0; i < block_count1; i++) {
    if (strcmp(operation_mode, "write") == 0) {
      memset(buffer1, i % SIZE, (size_t)block_size1);
      const ssize_t bytes_written = write(filed2, buffer1, (size_t)block_size1);
      if (bytes_written != block_size1) {
        perror("Write failed");
        return;
      }
    } else if (strcmp(operation_mode, "read") == 0) {
      const ssize_t bytes_read = read(filed2, buffer1, (size_t)block_size1);
      if (bytes_read != block_size1) {
        if (bytes_read == -1) {
          perror("Read failed");
        } else if (bytes_read < block_size1) {
          (void)fprintf(stderr, "Warning: Read less data than expected\n");
        }
        return;
      }
    } else {
      (void)fprintf(stderr, "Invalid operation_mode: %s\n", operation_mode);
      return;
    }
  }
}

int perform_random_access(
    int filed1, const int start_offset1, int end_offset1
) {
  if (start_offset1 == 0 && end_offset1 == 0) {
    end_offset1 = INT_MAX;
  }

  if (end_offset1 <= start_offset1) {
    (void)fprintf(
        stderr,
        "Invalid range for random access: %d-%d\n",
        start_offset1,
        end_offset1
    );
    return -1;
  }

  long random_offset =
      (random() % (end_offset1 - start_offset1)) + start_offset1;
  if (lseek(filed1, (off_t)random_offset, SEEK_SET) == (off_t)-1) {
    perror("Failed to seek to a random position");
    return -1;
  }

  return 0;
}

int main(const int argc, char* argv[]) {
  if (argc != ARGS_COUNT) {
    print_usage();
    return EXIT_FAILURE;
  }

  // Parse command-line arguments
  const char* operation_mode = argv[1];
  int block_size = 0;
  int block_count = 0;
  if (parse_block_size_and_count(argv[2], &block_size, argv[3], &block_count) !=
      0) {
    return EXIT_FAILURE;
  }

  const char* file_path = argv[4];
  const char* range = argv[FIVE];
  const char* direct_flag = argv[SIX];
  const char* access_type = argv[SEVEN];

  int start_offset = 0;
  int end_offset = 0;
  if (parse_range(range, &start_offset, &end_offset) != 0) {
    return EXIT_FAILURE;
  }

  // Open file with appropriate flags
  const int filed = open_file(file_path, TEN, direct_flag);
  if (filed == -1) {
    return EXIT_FAILURE;
  }

  void* buffer = malloc((size_t)block_size);
  if (buffer == NULL) {
    perror("Failed to allocate memory");
    close(filed);
    return EXIT_FAILURE;
  }

  unsigned int seed = 0;
  if (getrandom(&seed, sizeof(seed), 0) == -1) {
    perror("Failed to get random seed");
    return EXIT_FAILURE;
  }
  srand(seed);

  const clock_t start_time = clock();

  perform_read_write(filed, buffer, block_size, block_count, operation_mode);

  if (strcmp(access_type, RANDOM_ACCESS) == 0) {
    if (perform_random_access(filed, start_offset, end_offset) != 0) {
      free(buffer);
      close(filed);
      return EXIT_FAILURE;
    }
  }

  const clock_t end_time = clock();
  const double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

  printf("Total time taken: %.3f seconds\n", elapsed_time);
  free(buffer);
  close(filed);
  return EXIT_SUCCESS;
}