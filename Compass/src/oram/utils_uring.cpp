#include "utils_uring.h"

using namespace std;

struct io_data {
    int read;
    off_t first_offset, offset;
    size_t first_len;
    struct iovec iov;
};

int setup_context(unsigned entries, struct io_uring *ring) {
    int ret;

    ret = io_uring_queue_init(entries, ring, 0);
    if( ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return -1;
    }

    return 0;
}

int get_file_size(int fd, off_t *size) {
    struct stat st;

    if (fstat(fd, &st) < 0 )
        return -1;
    if(S_ISREG(st.st_mode)) {
        *size = st.st_size;
        return 0;
    } else if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;

        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;

        *size = bytes;
        return 0;
    }
    return -1;
}

void queue_prepped_fd(struct io_uring *ring, int fd,  struct io_data *data) {
    struct io_uring_sqe *sqe;

    sqe = io_uring_get_sqe(ring);
    assert(sqe);

    if (data->read)
        io_uring_prep_readv(sqe, fd, &data->iov, 1, data->offset);
    else
        io_uring_prep_writev(sqe, fd, &data->iov, 1, data->offset);

    io_uring_sqe_set_data(sqe, data);
}


int io(struct io_uring *ring, off_t insize, int fd, unsigned char* buf, bool read){

    struct io_uring_cqe *cqe;
    unsigned long to_do, has_done;
    to_do = insize;
    has_done = 0;

    off_t offset = 0;
    int q_size = 0;
    int ret;

    while(to_do || has_done != insize){

        bool sumbit = false;

        while(to_do) {
            off_t this_size = to_do;

            if (q_size >= QD)
                break;
            if (this_size > BS)
                this_size = BS;
            else if (!this_size)
                break;

            struct io_data *data;
            data = (io_data *)malloc(sizeof(*data));

            data->read = read ? 1 : 0;
            data->offset = data->first_offset = offset;
            // ATTENTION on this
            data->iov.iov_base = buf + offset;
            data->iov.iov_len = this_size;
            data->first_len = this_size;

            queue_prepped_fd(ring, fd,  data);

            to_do -= this_size;
            offset += this_size;
            q_size++;
            sumbit = true;
        }

        if (sumbit) {
            ret = io_uring_submit(ring);
            if (ret < 0) {
                fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
                break;
            }
        }

        /* Queue is full at this point. Let's find at least one completion */
        int got_comp = 0;
        while (has_done != insize){
            struct io_data *data;

            if (!got_comp) {
                ret = io_uring_wait_cqe(ring, &cqe);
                got_comp = 1;
            } else {
                ret = io_uring_peek_cqe(ring, &cqe);
                if (ret == -EAGAIN) {
                    cqe = NULL;
                    ret = 0;
                }
            }
            if (ret < 0) {
                fprintf(stderr, "io_uring_peek_cqe: %s\n",
                        strerror(-ret));
                return 1;
            }
            if (!cqe)
                break;

            data = (io_data *)io_uring_cqe_get_data(cqe);
            if (cqe->res < 0) {
                if (cqe->res == -EAGAIN) {
                    queue_prepped_fd(ring, fd, data);
                    io_uring_cqe_seen(ring, cqe);
                    continue;
                }
                fprintf(stderr, "cqe failed: %s\n",
                        strerror(-cqe->res));
                return 1;
            } else if (cqe->res != data->iov.iov_len) {
                /* short read/write; adjust and requeue */
                data->iov.iov_base += cqe->res;
                data->iov.iov_len -= cqe->res;
                queue_prepped_fd(ring, fd, data);
                io_uring_cqe_seen(ring, cqe);
                continue;
            }

            /*
             * All done. 
             * */
            free(data);
            has_done += data->first_len;
            q_size--;
            io_uring_cqe_seen(ring, cqe);
        }
    }

    return 0;
}


int buckets_io(struct io_uring *ring, std::vector<int> bucket_ids, off_t base_offset, off_t bucket_size, int fd, unsigned char* buf, bool read){

    int bucket_len = bucket_ids.size();

    struct io_uring_cqe *cqe;
    unsigned long to_do, has_done;
    to_do = bucket_len;
    has_done = 0;

    int q_size = 0;
    int ret;

    while(to_do || has_done != bucket_len){

        bool sumbit = false;

        while(to_do && q_size < QD) {
            
            off_t buf_id = bucket_len - to_do;
            off_t bkt_id = bucket_ids[buf_id];

            struct io_data *data;
            data = (io_data *)malloc(sizeof(*data));

            data->read = read ? 1 : 0;

            // ATTENTION on this
            // File offset
            data->offset = data->first_offset = base_offset + bkt_id * bucket_size;
            // Buf offset
            data->iov.iov_base = buf + buf_id * bucket_size;
            data->iov.iov_len = bucket_size;
            data->first_len = bucket_size;

            queue_prepped_fd(ring, fd,  data);

            to_do -= 1;
            q_size++;
            sumbit = true;
        }

        if (sumbit) {
            ret = io_uring_submit(ring);
            if (ret < 0) {
                fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
                break;
            }
        }

        /* Queue is full at this point. Let's find at least one completion */
        int got_comp = 0;
        while (has_done != bucket_len){
            struct io_data *data;

            if (!got_comp) {
                ret = io_uring_wait_cqe(ring, &cqe);
                got_comp = 1;
            } else {
                ret = io_uring_peek_cqe(ring, &cqe);
                if (ret == -EAGAIN) {
                    cqe = NULL;
                    ret = 0;
                }
            }
            if (ret < 0) {
                fprintf(stderr, "io_uring_peek_cqe: %s\n",
                        strerror(-ret));
                return 1;
            }
            if (!cqe)
                break;

            data = (io_data *)io_uring_cqe_get_data(cqe);
            if (cqe->res < 0) {
                if (cqe->res == -EAGAIN) {
                    queue_prepped_fd(ring, fd, data);
                    io_uring_cqe_seen(ring, cqe);
                    continue;
                }
                fprintf(stderr, "cqe failed: %s\n",
                        strerror(-cqe->res));
                return 1;
            } else if (cqe->res != data->iov.iov_len) {
                /* short read/write; adjust and requeue */
                data->iov.iov_base += cqe->res;
                data->iov.iov_len -= cqe->res;
                queue_prepped_fd(ring, fd, data);
                io_uring_cqe_seen(ring, cqe);
                continue;
            }

            /*
             * All done. 
             * */
            free(data);
            has_done += 1;
            q_size--;
            io_uring_cqe_seen(ring, cqe);
        }
    }

    return 0;
}

int buckets_io_wrapper(std::vector<int> bucket_ids, off_t bucket_size, int fd, unsigned char* buf, bool read){
    struct io_uring ring;
    int ret;

    if (setup_context(QD, &ring))
        perror("Error seting up context");
    // ----------- do sth ---------- // 
    off_t base_offset = sizeof(int) + sizeof(int) + sizeof(bool);
    ret = buckets_io(&ring, bucket_ids, base_offset, bucket_size, fd, buf, read);
    io_uring_queue_exit(&ring);
    return ret;
}

int buckets_io_wrapper_mt(std::vector<int> bucket_ids, off_t bucket_size, int fd, unsigned char* buf, bool read){

    int per_thread = bucket_ids.size() / NUM_THREADS;

    #pragma omp parallel for num_threads(NUM_THREADS)
    for(int i = 0; i < NUM_THREADS; i++){
        off_t start = i * per_thread;
        off_t end = (i+1) * per_thread;

        if(i == NUM_THREADS - 1)
            end = bucket_ids.size();
        vector<int> tmp(bucket_ids.begin() + start, bucket_ids.begin() + end);

        buckets_io_wrapper(
            tmp,
            bucket_size,
            fd,
            buf + start* bucket_size,
            read
        );
    }

    return 0;
}