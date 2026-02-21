
#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define EMA_MAGIC 0x454D4131U
#define EMA_VERSION 1U
#define DEFAULT_VALUE_MAX 1000U
#define DEFAULT_BIAS_PERCENT 50U
#define MAX_PATH_LEN 4096
#define HEADER_RESERVED 0U

#define MAX_BIAS_PERCENT 100U
#define DEFAULT_BIAS_PERCENT 50U
#define PERCENT_MODULO 100U
#define STACK_CAPACITY_DEFAULT 1024U
#define BITS_IN_BYTE 8U
#define OUTPATH_MAX_LEN 256U
#define FILEPATH_MAX_LEN 256U
#define DECIMAL_BASE 10

struct ema_header {
  uint32_t magic;
  uint32_t version;
  uint64_t node_count;
  uint32_t k_neighbors;
  uint32_t reserved;
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static inline void check_alloc(const void* ptr) {
  if (ptr == NULL) {
    (void)fprintf(stderr, "Memory allocation failed\n");
    exit(EXIT_FAILURE);  // NOLINT
  }
}

static void print_usage(const char* prog) {
  (void)fprintf(
      stderr,
      "ema-traverse-graph -- генерировать или обходить k-регулярный граф в "
      "файле\n\n"
      "Генерация:\n"
      "  %s --generate --nodes N --k K --outfile path [--seed S] "
      "[--value-max "
      "V] [--bias B]\n\n"
      "Траверс/модификация:\n"
      "  %s --traverse --file path --start INDEX --find VALUE --replace "
      "NEWVALUE [--max-depth D] [--max-modify M]\n\n",
      prog,
      prog
  );
}

static int open_file_for_write(const char* path) {
  int filed = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);  // NOLINT
  if (filed < 0) {
    perror("open outfile");
  }
  return filed;
}

static int open_file_for_rw(const char* path) {
  int filed = open(path, O_RDWR);
  if (filed < 0) {
    perror("open file for rw");
  }
  return filed;
}

static size_t node_size_bytes(uint32_t k_neighbors) {
  return sizeof(uint32_t) + (size_t)k_neighbors * sizeof(uint32_t);
}

static bool write_header(
    int filed, uint64_t node_count, uint32_t k_neighbors  // NOLINT
) {
  struct ema_header header;
  header.magic = EMA_MAGIC;
  header.version = EMA_VERSION;
  header.node_count = node_count;
  header.k_neighbors = k_neighbors;
  header.reserved = HEADER_RESERVED;

  ssize_t write = pwrite(filed, &header, sizeof(header), 0);
  if (write != (ssize_t)sizeof(header)) {
    perror("pwrite header");
    return false;
  }
  return true;
}

static bool read_header(int filed, struct ema_header* out_header) {
  ssize_t read = pread(filed, out_header, sizeof(*out_header), 0);
  if (read != (ssize_t)sizeof(*out_header)) {
    perror("pread header");
    return false;
  }
  if (out_header->magic != EMA_MAGIC) {
    (void)fprintf(stderr, "Bad magic (not EMA file)\n");
    return false;
  }
  if (out_header->version != EMA_VERSION) {
    (void)fprintf(stderr, "Unsupported version: %u\n", out_header->version);
    return false;
  }
  return true;
}

static bool write_node(
    int fd,  // NOLINT
    uint64_t node_index,
    uint32_t value,
    uint32_t* neighbors,  // NOLINT
    uint32_t k_neighbors
) {
  struct ema_header header;
  ssize_t read = pread(fd, &header, sizeof(header), 0);
  if (read != (ssize_t)sizeof(header)) {
    perror("pread header before write_node");
    return false;
  }
  size_t node_bytes = node_size_bytes(k_neighbors);
  off_t offset = (off_t)sizeof(header) + (off_t)node_index * (off_t)node_bytes;
  size_t bufsize = node_bytes;
  uint8_t* buf = malloc(bufsize);
  check_alloc(buf);
  uint32_t* buf_as_u32 = (uint32_t*)buf;
  buf_as_u32[0] = value;
  for (uint32_t i = 0; i < k_neighbors; ++i) {
    buf_as_u32[1 + i] = neighbors[i];
  }
  ssize_t write = pwrite(fd, buf, bufsize, offset);
  free(buf);
  if (write != (ssize_t)bufsize) {
    perror("pwrite node");
    return false;
  }
  return true;
}

static bool read_node(
    int fd,  // NOLINT
    uint64_t node_index,
    uint32_t k_neighbors,
    uint32_t* out_value,  // NOLINT
    uint32_t* out_neighbors
) {
  struct ema_header header;
  ssize_t read = pread(fd, &header, sizeof(header), 0);
  if (read != (ssize_t)sizeof(header)) {
    perror("pread header before read_node");
    return false;
  }
  size_t node_bytes = node_size_bytes(k_neighbors);
  off_t offset = (off_t)sizeof(header) + (off_t)node_index * (off_t)node_bytes;
  uint8_t* buf = malloc(node_bytes);
  check_alloc(buf);
  ssize_t read2 = pread(fd, buf, node_bytes, offset);
  if (read2 != (ssize_t)node_bytes) {
    perror("pread node");
    free(buf);
    return false;
  }
  uint32_t* buf_as_u32 = (uint32_t*)buf;
  *out_value = buf_as_u32[0];
  for (uint32_t i = 0; i < k_neighbors; ++i) {
    out_neighbors[i] = buf_as_u32[1 + i];
  }
  free(buf);
  return true;
}

static bool write_node_value(
    int fd,  // NOLINT
    uint64_t node_index,
    uint32_t k_neighbors,  // NOLINT
    uint32_t new_value
) {
  struct ema_header header;
  ssize_t read = pread(fd, &header, sizeof(header), 0);
  if (read != (ssize_t)sizeof(header)) {
    perror("pread header before write_node_value");
    return false;
  }
  off_t offset = (off_t)sizeof(header) +
                 (off_t)node_index * (off_t)node_size_bytes(k_neighbors);
  uint32_t value_le = new_value;
  ssize_t write = pwrite(fd, &value_le, sizeof(value_le), offset);
  if (write != (ssize_t)sizeof(value_le)) {
    perror("pwrite node value");
    return false;
  }
  return true;
}

static int command_generate(
    const char* outpath,
    uint64_t node_count,
    uint32_t k_neighbors,  // NOLINT
    unsigned int seed,
    uint32_t value_max,
    uint32_t bias_percent
) {
  if (k_neighbors == 0) {
    (void)fprintf(stderr, "k must be > 0\n");
    return 1;
  }
  if (node_count == 0) {
    (void)fprintf(stderr, "nodes must be > 0\n");
    return 1;
  }
  if (bias_percent > MAX_BIAS_PERCENT) {
    bias_percent = DEFAULT_BIAS_PERCENT;
  }

  int filed = open_file_for_write(outpath);
  if (filed < 0) {
    return 1;
  }

  if (!write_header(filed, node_count, k_neighbors)) {
    close(filed);
    return 1;
  }

  uint32_t* neighbors_buf = malloc(sizeof(uint32_t) * k_neighbors);
  check_alloc(neighbors_buf);

  unsigned int rng_state = seed;

  for (uint64_t idx = 0; idx < node_count; ++idx) {
    uint32_t value = (rand_r(&rng_state) % value_max);
    for (uint32_t ni = 0; ni < k_neighbors; ++ni) {
      uint32_t pick = 0;
      unsigned int roll = (rand_r(&rng_state) % PERCENT_MODULO);
      if (roll < bias_percent && idx + 1 < node_count) {
        uint64_t low = idx + 1;
        uint64_t high = node_count - 1;
        uint64_t range = high - low + 1;
        uint32_t neighbor_index =
            (uint32_t)(low + (rand_r(&rng_state) % range));
        pick = neighbor_index;
      } else {
        pick = (uint32_t)(rand_r(&rng_state) % node_count);
      }
      neighbors_buf[ni] = pick;
    }
    if (!write_node(filed, idx, value, neighbors_buf, k_neighbors)) {
      free(neighbors_buf);
      close(filed);
      return 1;
    }
  }

  free(neighbors_buf);
  close(filed);
  printf(
      "Generated graph file %s: nodes=%" PRIu64 " k=%u\n",
      outpath,
      node_count,
      k_neighbors
  );
  return 0;
}

static int command_traverse(  // NOLINT
    const char* filepath,
    uint64_t start_index,  // NOLINT
    uint32_t find_value,
    uint32_t replace_value,  // NOLINT
    uint64_t max_depth,
    uint64_t max_modify
) {
  int filed = open_file_for_rw(filepath);
  if (filed < 0) {
    return 1;
  }

  struct ema_header header;
  if (!read_header(filed, &header)) {
    close(filed);
    return 1;
  }

  const uint64_t node_count = header.node_count;
  const uint32_t k_neighbors = header.k_neighbors;

  if (start_index >= node_count) {
    (void)fprintf(stderr, "Start index out of range\n");
    close(filed);
    return 1;
  }

  const size_t visited_bytes =
      (size_t)((node_count + BITS_IN_BYTE - 1ULL) / BITS_IN_BYTE);
  uint8_t* visited = calloc(visited_bytes, 1);
  check_alloc(visited);

  size_t stack_capacity = STACK_CAPACITY_DEFAULT;
  uint64_t* stack_nodes = malloc(stack_capacity * sizeof(*stack_nodes));
  uint64_t* stack_depths = malloc(stack_capacity * sizeof(*stack_depths));
  check_alloc(stack_nodes);
  check_alloc(stack_depths);
  size_t stack_size = 0;

  uint32_t* neighbors_buf = malloc(sizeof(uint32_t) * k_neighbors);
  check_alloc(neighbors_buf);

  stack_nodes[stack_size] = start_index;
  stack_depths[stack_size] = 0ULL;
  stack_size++;

  uint64_t modifications_done = 0ULL;

  while (stack_size > 0 && modifications_done < max_modify) {
    stack_size--;
    const uint64_t node_index = stack_nodes[stack_size];
    const uint64_t depth = stack_depths[stack_size];

    const uint64_t byte_index = node_index / BITS_IN_BYTE;
    const uint8_t bit_mask = (uint8_t)(1U << (node_index % BITS_IN_BYTE));
    if (visited[byte_index] & bit_mask) {
      continue;
    }
    visited[byte_index] |= bit_mask;

    uint32_t node_value = 0U;
    if (!read_node(
            filed, node_index, k_neighbors, &node_value, neighbors_buf
        )) {
      (void)fprintf(stderr, "Failed to read node %" PRIu64 "\n", node_index);
      continue;
    }

    if (node_value == find_value) {
      if (!write_node_value(filed, node_index, k_neighbors, replace_value)) {
        (void)fprintf(
            stderr, "Failed to write node value at %" PRIu64 "\n", node_index
        );
        continue;
      }
      modifications_done++;
      printf(
          "Modified node %" PRIu64 " value %" PRIu32 " -> %" PRIu32 "\n",
          node_index,
          find_value,
          replace_value
      );
    }

    if (max_depth == 0ULL || depth < max_depth) {
      if (stack_size + k_neighbors > stack_capacity) {
        size_t new_capacity = stack_capacity * 2ULL;
        while (stack_size + k_neighbors > new_capacity) {
          new_capacity *= 2ULL;
        }
        uint64_t* new_stack_nodes =
            realloc(stack_nodes, new_capacity * sizeof(*stack_nodes));
        if (new_stack_nodes == NULL) {
          free(stack_nodes);
          (void)fprintf(
              stderr, "Memory allocation failed in realloc for stack_nodes\n"
          );
          return 1;
        }
        stack_nodes = new_stack_nodes;

        uint64_t* new_stack_depths =
            realloc(stack_depths, new_capacity * sizeof(*stack_depths));
        if (new_stack_depths == NULL) {
          free(stack_depths);
          (void)fprintf(
              stderr, "Memory allocation failed in realloc for stack_depths\n"
          );
          return 1;
        }
        stack_depths = new_stack_depths;
        check_alloc(stack_nodes);
        check_alloc(stack_depths);
        stack_capacity = new_capacity;
      }
      for (uint32_t ni = 0; ni < k_neighbors; ++ni) {
        const uint64_t neighbor_index = (uint64_t)neighbors_buf[ni];
        if (neighbor_index < node_count) {
          stack_nodes[stack_size] = neighbor_index;
          stack_depths[stack_size] = depth + 1ULL;
          stack_size++;
        }
      }
    }
  }

  free(neighbors_buf);
  free(stack_nodes);
  free(stack_depths);
  free(visited);
  close(filed);

  printf("Traversal done, modifications=%" PRIu64 "\n", modifications_done);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  static struct option long_options[] = {
      {  "generate",       no_argument, 0, 'g'},
      {  "traverse",       no_argument, 0, 't'},
      {     "nodes", required_argument, 0, 'n'},
      {         "k", required_argument, 0, 'k'},
      {   "outfile", required_argument, 0, 'o'},
      {      "seed", required_argument, 0, 's'},
      { "value-max", required_argument, 0, 'v'},
      {      "bias", required_argument, 0, 'b'},
      {      "file", required_argument, 0, 'f'},
      {     "start", required_argument, 0, 'S'},
      {      "find", required_argument, 0, 'F'},
      {   "replace", required_argument, 0, 'R'},
      { "max-depth", required_argument, 0, 'D'},
      {"max-modify", required_argument, 0, 'M'},
      {           0,                 0, 0,   0}
  };

  bool do_generate = false;
  bool do_traverse = false;
  uint64_t nodes_count = 0ULL;
  uint32_t k_neighbors = 0U;
  char outpath[MAX_PATH_LEN] = {0};
  unsigned int seed_val = (unsigned int)time(NULL);
  uint32_t value_max = DEFAULT_VALUE_MAX;
  uint32_t bias_percent = DEFAULT_BIAS_PERCENT;

  char filepath[MAX_PATH_LEN] = {0};
  uint64_t start_index = 0ULL;
  uint32_t find_value = 0U;
  uint32_t replace_value = 0U;
  uint64_t max_depth = 0ULL;
  uint64_t max_modify = 1ULL;

  int opt_index = 0;
  int opt = 0;
  optind = 1;
  while ((opt = getopt_long(  // NOLINT
              argc,
              argv,
              "gtn:k:o:s:v:b:f:S:F:R:D:M:",
              long_options,
              &opt_index
          )) != -1) {
    (void)opt;
    switch (opt) {
      case 'g':
        do_generate = true;
        break;
      case 't':
        do_traverse = true;
        break;
      case 'n':
        nodes_count = (uint64_t)strtoull(optarg, NULL, DECIMAL_BASE);
        break;
      case 'k':
        k_neighbors = (uint32_t)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'o':
        strncpy(outpath, optarg, sizeof(outpath) - 1);
        break;
      case 's':
        seed_val = (unsigned int)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'v':
        value_max = (uint32_t)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'b':
        bias_percent = (uint32_t)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'f':
        strncpy(filepath, optarg, sizeof(filepath) - 1);
        break;
      case 'S':
        start_index = (uint64_t)strtoull(optarg, NULL, DECIMAL_BASE);
        break;
      case 'F':
        find_value = (uint32_t)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'R':
        replace_value = (uint32_t)strtoul(optarg, NULL, DECIMAL_BASE);
        break;
      case 'D':
        max_depth = (uint64_t)strtoull(optarg, NULL, DECIMAL_BASE);
        break;
      case 'M':
        max_modify = (uint64_t)strtoull(optarg, NULL, DECIMAL_BASE);
        break;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  if (do_generate) {
    if (nodes_count == 0ULL || k_neighbors == 0U || outpath[0] == '\0') {
      (void)fprintf(stderr, "generate: missing required parameters\n");
      print_usage(argv[0]);
      return 1;
    }
    return command_generate(
        outpath, nodes_count, k_neighbors, seed_val, value_max, bias_percent
    );
  }

  if (do_traverse) {
    if (filepath[0] == '\0') {
      (void)fprintf(stderr, "traverse: missing --file\n");
      print_usage(argv[0]);
      return 1;
    }
    return command_traverse(
        filepath, start_index, find_value, replace_value, max_depth, max_modify
    );
  }

  print_usage(argv[0]);
  return 1;
}
