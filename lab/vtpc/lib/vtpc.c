#define _GNU_SOURCE
#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
  PAGE_SIZE = 4096,
  CACHE_PAGES = 64,
};

typedef struct CachePage {
  int os_fd;
  off_t page_offset;

  int valid;
  int dirty;

  void* data;

  struct CachePage* prev;
  struct CachePage* next;
} CachePage;

static CachePage cache[CACHE_PAGES];

static CachePage* lru_head = NULL;
static CachePage* lru_tail = NULL;

static int g_cache_inited = 0;

static void lru_remove(CachePage* p) {
  if (!p) {
    return;
  }
  if (p->prev) {
    p->prev->next = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  }
  if (lru_head == p) {
    lru_head = p->next;
  }
  if (lru_tail == p) {
    lru_tail = p->prev;
  }
  p->prev = NULL;
  p->next = NULL;
}

static void lru_insert_head(CachePage* p) {
  if (!p) {
    return;
  }
  p->prev = NULL;
  p->next = lru_head;
  if (lru_head) {
    lru_head->prev = p;
  }
  lru_head = p;
  if (!lru_tail) {
    lru_tail = p;
  }
}

static void lru_touch(CachePage* p) {
  if (!p) {
    return;
  }
  if (lru_head == p) {
    return;
  }
  if (p->prev || p->next || lru_head == p) {
    lru_remove(p);
  }
  lru_insert_head(p);
}

static int init_cache_once(void) {
  if (g_cache_inited) {
    return 0;
  }
  for (size_t i = 0; i < CACHE_PAGES; i++) {
    cache[i].os_fd = -1;
    cache[i].page_offset = 0;
    cache[i].valid = 0;
    cache[i].dirty = 0;
    cache[i].prev = NULL;
    cache[i].next = NULL;
    cache[i].data = NULL;

    void* ptr = NULL;
    const int rc = posix_memalign(&ptr, (size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
    if (rc != 0) {
      return -1;
    }
    cache[i].data = ptr;
    memset(cache[i].data, 0, (size_t)PAGE_SIZE);
  }
  lru_head = NULL;
  lru_tail = NULL;
  g_cache_inited = 1;
  return 0;
}

static ssize_t flush_page(CachePage* p) {
  if (!p || !p->valid || !p->dirty) {
    return 0;
  }
  const ssize_t wr =
      pwrite(p->os_fd, p->data, (size_t)PAGE_SIZE, p->page_offset);
  if (wr == (ssize_t)PAGE_SIZE) {
    p->dirty = 0;
    return wr;
  }
  return -1;
}

static CachePage* find_page(const int os_fd, const off_t page_offset) {
  for (size_t i = 0; i < CACHE_PAGES; i++) {
    if (cache[i].valid && cache[i].os_fd == os_fd &&
        cache[i].page_offset == page_offset) {
      return &cache[i];
    }
  }
  return NULL;
}

static CachePage* get_free_or_evict_page(void) {
  for (size_t i = 0; i < CACHE_PAGES; i++) {
    if (!cache[i].valid) {
      return &cache[i];
    }
  }

  CachePage* victim = lru_tail;
  if (!victim) {
    return &cache[0];
  }

  if (victim->dirty) {
    if (flush_page(victim) < 0) {
      return NULL;
    }
  }

  lru_remove(victim);
  victim->valid = 0;
  victim->dirty = 0;
  victim->os_fd = -1;
  victim->page_offset = 0;
  memset(victim->data, 0, (size_t)PAGE_SIZE);
  return victim;
}

static CachePage* load_page(const int os_fd, const off_t page_offset) {
  CachePage* p = find_page(os_fd, page_offset);
  if (p) {
    lru_touch(p);
    return p;
  }

  p = get_free_or_evict_page();
  if (!p) {
    return NULL;
  }

  const ssize_t rd = pread(os_fd, p->data, (size_t)PAGE_SIZE, page_offset);
  if (rd < 0) {
    return NULL;
  }
  if (rd < (ssize_t)PAGE_SIZE) {
    memset((char*)p->data + rd, 0, (size_t)PAGE_SIZE - (size_t)rd);
  }

  p->os_fd = os_fd;
  p->page_offset = page_offset;
  p->valid = 1;
  p->dirty = 0;
  p->prev = NULL;
  p->next = NULL;
  lru_insert_head(p);
  return p;
}

static off_t get_file_size_best_effort(const int fd) {
  struct stat st;
  if (fstat(fd, &st) == 0) {
    return st.st_size;
  }
  return -1;
}

static int is_at_or_after_eof(const off_t cur, const off_t file_size) {
  return file_size >= 0 && cur >= file_size;
}

static int prepare_read_chunk(
    const off_t cur,
    const size_t count,
    const size_t total,
    const off_t file_size,
    off_t* page_offset,
    size_t* in_page,
    size_t* take
) {
  if (is_at_or_after_eof(cur, file_size)) {
    return 0;
  }
    *page_offset = (cur / (off_t)PAGE_SIZE) * (off_t)PAGE_SIZE;
  *in_page = (size_t)(cur - *page_offset);

  const size_t can_take = (size_t)PAGE_SIZE - *in_page;
  const size_t need = count - total;
  *take = (need < can_take) ? need : can_take;

  if (file_size >= 0) {
    const off_t remaining = file_size - cur;
    if (remaining <= 0) {
      return 0;
    }
    if ((off_t)*take > remaining) {
      *take = (size_t)remaining;
    }
  }

  return 1;
}

static ssize_t return_partial_or_error(const size_t total) {
  if (errno == 0) {
    errno = EIO;
  }
  return (total > 0) ? (ssize_t)total : -1;
}

int vtpc_open(const char* path, int mode, const int access) {
  if (init_cache_once() != 0) {
    errno = ENOMEM;
    return -1;
  }
#ifdef O_DIRECT
  const int open_flags = (int)((unsigned int)mode | (unsigned int)O_DIRECT);
  int fd = open(path, open_flags, access);
  if (fd >= 0) {
    return fd;
  }

  if (errno == EINVAL || errno == EOPNOTSUPP) {
    return open(path, mode, access);
  }

  return -1;
#else
  return open(path, mode, access);
#endif
}

int vtpc_close(const int fd) {
  if (init_cache_once() != 0) {
    (void)fsync(fd);
    return close(fd);
  }

  for (size_t i = 0; i < CACHE_PAGES; i++) {
    CachePage* p = &cache[i];
    if (p->valid && p->os_fd == fd) {
      if (p->dirty) {
        if (flush_page(p) < 0) {
        }
      }
      lru_remove(p);
      p->valid = 0;
      p->dirty = 0;
      p->os_fd = -1;
      p->page_offset = 0;
      memset(p->data, 0, (size_t)PAGE_SIZE);
    }
  }

  (void)fsync(fd);
  return close(fd);
}

ssize_t vtpc_read(const int fd, void* buf, const size_t count) {
  if (init_cache_once() != 0) {
    errno = ENOMEM;
    return -1;
  }

  if (count == 0) {
    return 0;
  }
  if (!buf) {
    errno = EINVAL;
    return -1;
  }

  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) {
    return -1;
  }

  const off_t file_size = get_file_size_best_effort(fd);

  if (is_at_or_after_eof(cur, file_size)) {
    return 0;
  }

  size_t total = 0;
  char* out = (char*)buf;

  while (total < count) {
    off_t page_offset = 0;
    size_t in_page = 0;
    size_t take = 0;
    if (!prepare_read_chunk(
            cur, count, total, file_size, &page_offset, &in_page, &take
        )) {
      break;
    }

    CachePage* p = load_page(fd, page_offset);
    if (!p) {
      return return_partial_or_error(total);
    }

    memcpy(out + total, (const char*)p->data + in_page, take);

    total += take;
    cur += (off_t)take;
  }

  if (lseek(fd, cur, SEEK_SET) < 0) {
    return (total > 0) ? (ssize_t)total : -1;
  }

  return (ssize_t)total;
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
  if (init_cache_once() != 0) {
    errno = ENOMEM;
    return -1;
  }

  if (count == 0) {
    return 0;
  }
  if (!buf) {
    errno = EINVAL;
    return -1;
  }

  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) {
    return -1;
  }

  size_t total = 0;
  const char* in = (const char*)buf;

  while (total < count) {
    const off_t page_offset = (cur / (off_t)PAGE_SIZE) * (off_t)PAGE_SIZE;
    const size_t in_page = (size_t)(cur - page_offset);
    const size_t can_take = (size_t)PAGE_SIZE - in_page;
    const size_t need = count - total;
    const size_t take = (need < can_take) ? need : can_take;

    CachePage* p = load_page(fd, page_offset);
    if (!p) {
      if (errno == 0) {
        errno = EIO;
      }
      return (total > 0) ? (ssize_t)total : -1;
    }

    memcpy((char*)p->data + in_page, in + total, take);
    p->dirty = 1;
    lru_touch(p);

    total += take;
    cur += (off_t)take;
  }

  if (lseek(fd, cur, SEEK_SET) < 0) {
    return (total > 0) ? (ssize_t)total : -1;
  }

  return (ssize_t)total;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  return lseek(fd, offset, whence);
}

int vtpc_fsync(int fd) {
  if (init_cache_once() != 0) {
    errno = ENOMEM;
    return -1;
  }

  for (size_t i = 0; i < CACHE_PAGES; i++) {
    CachePage* p = &cache[i];
    if (p->valid && p->os_fd == fd && p->dirty) {
      if (flush_page(p) < 0) {
        if (errno == 0) {
          errno = EIO;
        }
        return -1;
      }
    }
  }

  return fsync(fd);
}
