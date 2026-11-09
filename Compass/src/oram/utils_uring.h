#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <liburing.h>
#include <vector>

#include <chrono>
#include <iostream>

#define QD  2
#define BS (16 * 1024)
#define NUM_THREADS 32

int setup_context(unsigned entries, struct io_uring *ring);

int get_file_size(int fd, off_t *size);

void queue_prepped_fd(struct io_uring *ring, int fd,  struct io_data *data);

int io(struct io_uring *ring, off_t insize, int fd, unsigned char* buf, bool read);

int buckets_io(struct io_uring *ring, std::vector<int> bucket_ids, off_t base_offset, off_t bucket_size, int fd, unsigned char* buf, bool read);

int buckets_io_wrapper(std::vector<int> bucket_ids, off_t bucket_size, int fd, unsigned char* buf, bool read);

int buckets_io_wrapper_mt(std::vector<int> bucket_ids, off_t bucket_size, int fd, unsigned char* buf, bool read);