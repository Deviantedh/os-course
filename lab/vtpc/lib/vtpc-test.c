#define _GNU_SOURCE
#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
  PAGE_SIZE = 1048576,
  CACHE_PAGES = 1024,
  CPU_NUM = 64,
  DIRECT_FAST_PATH_ENABLED = 1,
};

typedef struct CachePage {
  int os_fd;
  off_t page_offset;

  int valid;
  int dirty;
  off_t dirty_end;

  void* data;

  struct CachePage* prev;
  struct CachePage* next;
} CachePage;

static CachePage cache[CACHE_PAGES];

static CachePage* lru_head = NULL;
static CachePage* lru_tail = NULL;

static int g_cache_inited = 0;

static int ensure_page_buffer(CachePage* p) {
  if (!p) {
    errno = EINVAL;
    return -1;
  }
  if (p->data) {
    return 0;
  }

  void* ptr = NULL;
  const int rc = posix_memalign(&ptr, (size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
  if (rc != 0) {
    errno = rc;
    return -1;
  }

  p->data = ptr;
  return 0;
}

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
    cache[i].dirty_end = 0;
    cache[i].prev = NULL;
    cache[i].next = NULL;
    cache[i].data = NULL;
  }
  lru_head = NULL;
  lru_tail = NULL;
  g_cache_inited = 1;
  return 0;
}

static ssize_t pread_retry(int fd, void* buf, size_t count, off_t offset) {
  size_t done = 0;
  char* out = (char*)buf;

  while (done < count) {
    const ssize_t rd =
        pread(fd, out + done, count - done, offset + (off_t)done);
    if (rd == 0) {
      break;
    }
    if (rd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    done += (size_t)rd;
  }

  return (ssize_t)done;
}

static ssize_t pwrite_retry(
    int fd, const void* buf, size_t count, off_t offset
) {
  size_t done = 0;
  const char* in = (const char*)buf;

  while (done < count) {
    const ssize_t wr =
        pwrite(fd, in + done, count - done, offset + (off_t)done);
    if (wr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (wr == 0) {
      errno = EIO;
      return -1;
    }
    done += (size_t)wr;
  }

  return (ssize_t)done;
}

static ssize_t pwrite_buffered_fallback(
    int fd, const void* buf, size_t count, off_t offset
) {
  char proc_path[CPU_NUM];
  const int n = snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
  if (n <= 0 || (size_t)n >= sizeof(proc_path)) {
    errno = EIO;
    return -1;
  }

  const int buffered_fd = open(proc_path, O_WRONLY);
  if (buffered_fd < 0) {
    return -1;
  }

  const ssize_t wr = pwrite_retry(buffered_fd, buf, count, offset);
  const int close_rc = close(buffered_fd);
  if (wr < 0) {
    return -1;
  }
  if (close_rc < 0) {
    return -1;
  }
  return wr;
}

static ssize_t flush_page(CachePage* p) {
  if (!p || !p->valid || !p->dirty) {
    return 0;
  }
  if (p->dirty_end <= p->page_offset) {
    p->dirty = 0;
    p->dirty_end = 0;
    return 0;
  }

  const size_t flush_len = (size_t)(p->dirty_end - p->page_offset);
  ssize_t wr = 0;
  if ((flush_len % (size_t)PAGE_SIZE) == 0) {
    wr = pwrite_retry(p->os_fd, p->data, flush_len, p->page_offset);
  } else {
    wr = pwrite_buffered_fallback(p->os_fd, p->data, flush_len, p->page_offset);
  }
  if (wr == (ssize_t)flush_len) {
    p->dirty = 0;
    p->dirty_end = 0;
    return wr;
  }
  if (wr >= 0) {
    errno = EIO;
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

static void invalidate_cached_range(
    const int os_fd, const off_t start_offset, const off_t end_offset
) {
  for (size_t i = 0; i < CACHE_PAGES; i++) {
    CachePage* p = &cache[i];
    if (!p->valid || p->os_fd != os_fd) {
      continue;
    }

    const off_t page_start = p->page_offset;
    const off_t page_end = p->page_offset + (off_t)PAGE_SIZE;
    if (page_end <= start_offset || page_start >= end_offset) {
      continue;
    }

    lru_remove(p);
    p->valid = 0;
    p->dirty = 0;
    p->dirty_end = 0;
    p->os_fd = -1;
    p->page_offset = 0;
  }
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
  victim->dirty_end = 0;
  victim->os_fd = -1;
  victim->page_offset = 0;
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

  if (ensure_page_buffer(p) != 0) {
    return NULL;
  }

  const ssize_t rd =
      pread_retry(os_fd, p->data, (size_t)PAGE_SIZE, page_offset);
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
  p->dirty_end = 0;
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

static off_t get_cached_logical_eof(const int fd, const off_t file_size) {
  off_t eof = file_size;
  for (size_t i = 0; i < CACHE_PAGES; i++) {
    const CachePage* p = &cache[i];
    if (!p->valid || p->os_fd != fd || !p->dirty) {
      continue;
    }
    if (p->dirty_end > eof) {
      eof = p->dirty_end;
    }
  }
  return eof;
}

static int is_at_or_after_eof(const off_t cur, const off_t file_size) {
  return file_size >= 0 && cur >= file_size;
}

static int is_o_direct_fd(const int fd) {
  const int fd_flags = fcntl(fd, F_GETFL);
  if (fd_flags < 0) {
    return 0;
  }

  const unsigned int fd_flags_u = (unsigned int)fd_flags;
  return (fd_flags_u & (unsigned int)O_DIRECT) != 0U;
}

static int can_use_direct_write_fast_path(
    const int fd, const void* buf, const off_t cur, const size_t count
) {
  const int is_direct = is_o_direct_fd(fd);
  if (is_direct && (((uintptr_t)buf % (uintptr_t)PAGE_SIZE) != 0U)) {
    return 0;
  }

  return (cur % (off_t)PAGE_SIZE) == 0 && (count % (size_t)PAGE_SIZE) == 0;
}

static int can_use_direct_read_fast_path(
    const int fd, const void* buf, const off_t cur, const size_t count
) {
  const int is_direct = is_o_direct_fd(fd);
  if (is_direct && (((uintptr_t)buf % (uintptr_t)PAGE_SIZE) != 0U)) {
    return 0;
  }

  return (cur % (off_t)PAGE_SIZE) == 0 && (count % (size_t)PAGE_SIZE) == 0;
}

static ssize_t write_direct_fast_path(
    const int fd, const void* buf, const size_t count, off_t* cur
) {
  const off_t end = *cur + (off_t)count;
  invalidate_cached_range(fd, *cur, end);

  const ssize_t wr = pwrite_retry(fd, buf, count, *cur);
  if (wr < 0) {
    return -1;
  }

  *cur += wr;
  if (lseek(fd, *cur, SEEK_SET) < 0) {
    return -1;
  }

  return wr;
}

static ssize_t write_via_page_cache(
    const int fd, const void* buf, const size_t count, off_t* cur
) {
  size_t total = 0;
  const char* in = (const char*)buf;

  while (total < count) {
    const off_t page_offset = (*cur / (off_t)PAGE_SIZE) * (off_t)PAGE_SIZE;
    const size_t in_page = (size_t)(*cur - page_offset);
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
    const off_t write_end = *cur + (off_t)take;
    if (write_end > p->dirty_end) {
      p->dirty_end = write_end;
    }
    lru_touch(p);

    total += take;
    *cur += (off_t)take;
  }

  if (lseek(fd, *cur, SEEK_SET) < 0) {
    return (total > 0) ? (ssize_t)total : -1;
  }

  return (ssize_t)total;
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

static ssize_t read_direct_fast_path(
    const int fd, void* buf, const size_t count, off_t* cur
) {
  const off_t file_size = get_file_size_best_effort(fd);
  const off_t logical_eof = get_cached_logical_eof(fd, file_size);
  if (is_at_or_after_eof(*cur, logical_eof)) {
    return 0;
  }

  size_t to_read = count;
  if (logical_eof >= 0) {
    const off_t remaining = logical_eof - *cur;
    if (remaining <= 0) {
      return 0;
    }
    if ((off_t)to_read > remaining) {
      to_read = (size_t)remaining;
    }
    to_read -= (to_read % (size_t)PAGE_SIZE);
    if (to_read == 0) {
      return 0;
    }
  }

  const ssize_t rd = pread_retry(fd, buf, to_read, *cur);
  if (rd < 0) {
    return -1;
  }

  *cur += rd;
  if (lseek(fd, *cur, SEEK_SET) < 0) {
    return (rd > 0) ? rd : -1;
  }

  return rd;
}

static ssize_t read_via_page_cache(
    const int fd, void* buf, const size_t count, off_t* cur
) {
  const off_t file_size = get_file_size_best_effort(fd);
  const off_t logical_eof = get_cached_logical_eof(fd, file_size);

  if (is_at_or_after_eof(*cur, logical_eof)) {
    return 0;
  }

  size_t total = 0;
  char* out = (char*)buf;

  while (total < count) {
    off_t page_offset = 0;
    size_t in_page = 0;
    size_t take = 0;
    if (!prepare_read_chunk(
            *cur, count, total, logical_eof, &page_offset, &in_page, &take
        )) {
      break;
    }

    CachePage* p = load_page(fd, page_offset);
    if (!p) {
      return return_partial_or_error(total);
    }

    memcpy(out + total, (const char*)p->data + in_page, take);

    total += take;
    *cur += (off_t)take;
  }

  if (lseek(fd, *cur, SEEK_SET) < 0) {
    return (total > 0) ? (ssize_t)total : -1;
  }

  return (ssize_t)total;
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
    const int fsync_rc = fsync(fd);
    const int close_rc = close(fd);
    if (fsync_rc < 0 || close_rc < 0) {
      return -1;
    }
    return 0;
  }

  int saved_errno = 0;

  for (size_t i = 0; i < CACHE_PAGES; i++) {
    CachePage* p = &cache[i];
    if (p->valid && p->os_fd == fd) {
      if (p->dirty && flush_page(p) < 0 && saved_errno == 0) {
        saved_errno = errno ? errno : EIO;
      }
      lru_remove(p);
      p->valid = 0;
      p->dirty = 0;
      p->dirty_end = 0;
      p->os_fd = -1;
      p->page_offset = 0;
    }
  }

  if (fsync(fd) < 0 && saved_errno == 0) {
    saved_errno = errno;
  }

  if (close(fd) < 0 && saved_errno == 0) {
    saved_errno = errno;
  }

  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }

  return 0;
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

  if (DIRECT_FAST_PATH_ENABLED &&
      can_use_direct_read_fast_path(fd, buf, cur, count)) {
    const ssize_t direct_rd = read_direct_fast_path(fd, buf, count, &cur);
    if (direct_rd < 0) {
      return -1;
    }
    if ((size_t)direct_rd == count) {
      return direct_rd;
    }

    const ssize_t tail_rd = read_via_page_cache(
        fd, (char*)buf + direct_rd, count - (size_t)direct_rd, &cur
    );
    if (tail_rd < 0) {
      return (direct_rd > 0) ? direct_rd : -1;
    }
    return direct_rd + tail_rd;
  }

  return read_via_page_cache(fd, buf, count, &cur);
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

  if (DIRECT_FAST_PATH_ENABLED &&
      can_use_direct_write_fast_path(fd, buf, cur, count)) {
    return write_direct_fast_path(fd, buf, count, &cur);
  }

  return write_via_page_cache(fd, buf, count, &cur);
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
