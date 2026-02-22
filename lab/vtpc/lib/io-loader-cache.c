#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vtpc.h"

#define DEFAULT_PERMISSIONS 0666
#define O_DIRECT_FLAG "on"
#define SEQUENCE_ACCESS "sequence"
#define RANDOM_ACCESS "random"

enum { ARGS_COUNT = 8, TEN = 10, FIVE = 5, SIX = 6, SEVEN = 7, SIZE = 256 };

void print_usage(void) {
  printf(
      "Usage: io_loader_cache [operation_mode] [block_size] [block_count] "
      "[file] "
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

int parse_range(const char* range, int64_t* start_offset, int64_t* end_offset) {
  if (strcmp(range, "0-0") != 0) {
    char* endptr1 = NULL;
    errno = 0;

    long long start_offset_long = strtoll(range, &endptr1, TEN);
    if (*endptr1 != '-' || *(endptr1 + 1) == '\0') {
      (void)fprintf(stderr, "Invalid range format: %s\n", range);
      return -1;
    }

    if (errno == ERANGE) {
      (void)fprintf(
          stderr, "start_offset is out of range: %lld\n", start_offset_long
      );
      return -1;
    }
    *start_offset = (int64_t)start_offset_long;

    errno = 0;
    long long end_offset_long = strtoll(endptr1 + 1, &endptr1, TEN);
    if (*endptr1 != '\0') {
      (void)fprintf(stderr, "Invalid range format: %s\n", range);
      return -1;
    }

    if (errno == ERANGE) {
      (void
      )fprintf(stderr, "end_offset is out of range: %lld\n", end_offset_long);
      return -1;
    }
    *end_offset = (int64_t)end_offset_long;

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
  (void)iii;
  (void)direct_flag1;

  const int flags = O_RDWR | O_CREAT | O_LARGEFILE;
  const int filed = vtpc_open(file_path, flags, DEFAULT_PERMISSIONS);
  if (filed == -1) {
    perror("Failed to open file");
    return -1;
  }

  return filed;
}

static void resolve_range_end(
    const int fd,
    const int64_t start_offset,
    const int64_t end_offset,
    int64_t* range_start,
    int64_t* range_end
) {
  *range_start = start_offset;
  *range_end = end_offset;
  if (!(*range_start == 0 && *range_end == 0)) {
    return;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_size > 0) {
    *range_end = (int64_t)st.st_size;
  } else {
    *range_end = 0;
  }
}

static int seek_to_start_if_needed(
    const int fd, const int64_t start_offset, const int64_t end_offset
) {
  if (start_offset == 0 && end_offset == 0) {
    return 0;
  }
  if (vtpc_lseek(fd, (off_t)start_offset, SEEK_SET) == (off_t)-1) {
    perror("Failed to seek to start offset");
    return -1;
  }
  return 0;
}

static int maybe_seek_random_block(
    const int fd,
    const int is_random,
    const int64_t range_start,
    const int64_t effective_len,
    const int block_size
) {
  if (!is_random || effective_len <= 0) {
    return 0;
  }

  const uint64_t span_blocks = (uint64_t)(effective_len / (int64_t)block_size);
  if (span_blocks == 0) {
    return 0;
  }

  const uint64_t res = ((uint64_t)(unsigned long)random() << 33U) ^
                       ((uint64_t)(unsigned long)random() << 2U) ^
                       ((uint64_t)(unsigned long)random() & 0x3U);
  const uint64_t block_index = res % span_blocks;
  const int64_t random_offset =
      range_start + (int64_t)(block_index * (uint64_t)block_size);
  if (vtpc_lseek(fd, (off_t)random_offset, SEEK_SET) == (off_t)-1) {
    perror("Failed to seek to a random position");
    return -1;
  }
  return 0;
}

static int do_write_block(
    const int fd, void* buffer, const int block_size, const int iteration
) {
  memset(buffer, iteration % SIZE, (size_t)block_size);
  const ssize_t bytes_written = vtpc_write(fd, buffer, (size_t)block_size);
  if (bytes_written != block_size) {
    perror("Write failed");
    return -1;
  }
  return 0;
}

static int do_read_block(const int fd, void* buffer, const int block_size) {
  const ssize_t bytes_read = vtpc_read(fd, buffer, (size_t)block_size);
  if (bytes_read == block_size) {
    return 0;
  }
  if (bytes_read == -1) {
    perror("Read failed");
  } else {
    (void)fprintf(stderr, "Warning: Read less data than expected\n");
  }
  return -1;
}
static int do_single_operation(
    const int fd,
    void* buffer,
    const int block_size,
    const int iteration,
    const char* operation_mode
) {
  if (strcmp(operation_mode, "write") == 0) {
    return do_write_block(fd, buffer, block_size, iteration);
  }
  if (strcmp(operation_mode, "read") == 0) {
    return do_read_block(fd, buffer, block_size);
  }

  (void)fprintf(stderr, "Invalid operation_mode: %s\n", operation_mode);
  return -1;
}

void perform_read_write(
    const int filed2,
    void* buffer1,
    int block_size1,
    const int block_count1,
    const char* operation_mode,
    const char* access_type,
    const int64_t start_offset,
    const int64_t end_offset
) {
  int64_t range_start = 0;
  int64_t range_end = 0;
  resolve_range_end(filed2, start_offset, end_offset, &range_start, &range_end);
  if (seek_to_start_if_needed(filed2, start_offset, end_offset) != 0) {
    return;
  }

  const int is_random = (strcmp(access_type, RANDOM_ACCESS) == 0);

  int64_t effective_len = 0;
  if (is_random && range_end > range_start) {
    effective_len = range_end - range_start;
  }

  for (int i = 0; i < block_count1; i++) {
    if (maybe_seek_random_block(
            filed2, is_random, range_start, effective_len, block_size1
        ) != 0) {
      return;
    }

    if (do_single_operation(filed2, buffer1, block_size1, i, operation_mode) !=
        0) {
      return;
    }
  }

  // For write mode, flush cached dirty pages so results are comparable.
  if (strcmp(operation_mode, "write") == 0) {
    if (vtpc_fsync(filed2) != 0) {
      perror("fsync failed");
    }
  }
}

int perform_random_access(
    int filed1, const int64_t start_offset1, int64_t end_offset1
) {
  (void)filed1;
  (void)start_offset1;
  (void)end_offset1;

  return 0;
}

int main(const int argc, char* argv[]) {
  if (argc != ARGS_COUNT) {
    print_usage();
    return EXIT_FAILURE;
  }

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

  int64_t start_offset = 0;
  int64_t end_offset = 0;
  if (parse_range(range, &start_offset, &end_offset) != 0) {
    return EXIT_FAILURE;
  }

  const int filed = open_file(file_path, TEN, direct_flag);
  if (filed == -1) {
    return EXIT_FAILURE;
  }

  void* buffer = malloc((size_t)block_size);
  if (buffer == NULL) {
    perror("Failed to allocate memory");
    vtpc_close(filed);
    return EXIT_FAILURE;
  }

  unsigned int seed = 0;
  if (getrandom(&seed, sizeof(seed), 0) == -1) {
    perror("Failed to get random seed");
    return EXIT_FAILURE;
  }
  srand(seed);

  const clock_t start_time = clock();

  perform_read_write(
      filed,
      buffer,
      block_size,
      block_count,
      operation_mode,
      access_type,
      start_offset,
      end_offset
  );
  const clock_t end_time = clock();
  const double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

  printf("Total time taken: %.3f seconds\n", elapsed_time);
  free(buffer);
  vtpc_close(filed);
  return EXIT_SUCCESS;
}