#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>    // for open(), O_DSYNC
#include <string.h>   // for memset()
#include <stdint.h>   // for uint64_t
#include <limits.h>   // for UINT_MAX
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <liburing.h> // for io_uring
#include <time.h>     // for random offsets
#include <x86intrin.h> // for __rdtsc();

// Define target file size in bytes (1 GB sparse file)
#define TARGET_FILE_SIZE (1024 * 1024 * 1024)

// Global variables for loop count, batch size, and sync flag
int loop_count = 1;
int batch_size = 64;
int sync_flag = 0;  // Sync flag to determine if O_DSYNC should be used

// Function prototypes
static __inline__ uint64_t rdtsc(void);
extern off_t get_random_offset(void);
extern void run_write(const char *filename, size_t size, int sync);
extern void run_write_uring(const char *filename, size_t size, int sync);
extern void parse_arguments(int argc, char *argv[], const char **filename, size_t *size, int *sync);

// Function to read the time-stamp counter
static __inline__ uint64_t rdtsc(void) {
	// TODO: add ARM support here
#if 0
	// assembly version of RDTSC
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "rdtsc"
        : "=a" (lo), "=d" (hi)
    );
    return ((uint64_t)hi << 32) | lo;
#else
	// use intrinsic; supported on gcc/clang/msvcc/icc
	return __rdtsc();
#endif
}

// Function to generate a random offset within the target file size
off_t get_random_offset(void) {
    return (off_t)(random() % TARGET_FILE_SIZE); // hopefully the compiler rewrites this to be a mask instead of a division/remainder
}

// Function to write <size> bytes to a file using synchronous write()
void run_write(const char *filename, size_t size, int sync) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (sync) flags |= O_DSYNC;
    int fd = open(filename, flags, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        return;
    }

    char *buffer = malloc(size);
    memset(buffer, 'A', size); // Fill buffer with 'A'

    // Start timing for the write loop
    uint64_t start_cycles = rdtsc();

    for (int i = 0; i < loop_count; i++) {
        off_t offset = get_random_offset();
        ssize_t ret = pwrite(fd, buffer, size, offset); // Write data at random offset
        if (ret < 0 || (size_t)ret != size) perror("write failed"); // Ensure correct comparison
    }

    // End timing for the write loop
    uint64_t end_cycles = rdtsc();
    uint64_t total_cycles = end_cycles - start_cycles;

    // Display the timing results
    printf("Total cycles for write (%s): %lu\n", sync ? "O_DSYNC" : "normal", total_cycles);
    printf("Cycles per byte (%s): %.2f\n", sync ? "O_DSYNC" : "normal", (double)total_cycles / ((double)size * (double)loop_count));

    close(fd);
    free(buffer);
}

void run_write_uring(const char *filename, size_t size, int sync) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (sync) flags |= O_DSYNC;
    int fd = open(filename, flags, 0644);
    if (fd < 0) {
        perror("Failed to open file");
        return;
    }

    char *buffer = malloc(size);
    memset(buffer, 'A', size); // Fill buffer with 'A'

    struct io_uring ring;
    struct io_uring_cqe *cqe;

    // Initialize io_uring with a queue size that matches the loop count
    unsigned int queue_size = (unsigned int)(batch_size > loop_count ? batch_size : loop_count);
    if (io_uring_queue_init(queue_size, &ring, 0) < 0) {  // Use adjusted queue size
        perror("Failed on io_uring_queue_init");
        free(buffer);
        close(fd);
        return;
    }

    // Start timing for the io_uring write loop
    uint64_t start_cycles = rdtsc();

    // Submit all I/O requests first
    for (int i = 0; i < loop_count; i++) {
        off_t offset = get_random_offset();

        // Get submission queue entry
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "ERROR: Failed to get SQE\n");
            return;
        }

        // Prepare a pwrite request
        io_uring_prep_write(sqe, fd, buffer, (unsigned int)size, (__u64)offset);
    }

    // Submit all requests in one go
    int ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "ERROR: io_uring_submit failed: %s\n", strerror(-ret));
        io_uring_queue_exit(&ring);
        free(buffer);
        close(fd);
        return;
    }

    // Batched version using io_uring_wait_cqes and batch_size
    for (int i = 0; i < loop_count; i += batch_size) {
        // Wait for multiple completions at once
        ret = io_uring_wait_cqes(&ring, &cqe, (uint32_t)batch_size, NULL, NULL);
        if (ret < 0) {
            fprintf(stderr, "ERROR: io_uring_wait_cqes failed: %s\n", strerror(-ret));
            return;
        }

        // Process completions
        for (int j = 0; j < batch_size && i + j < loop_count; j++) {
            if (cqe[j].res < 0) fprintf(stderr, "write (io_uring) failed: %s\n", strerror(-cqe[j].res));

            // Mark CQE as seen
            io_uring_cqe_seen(&ring, &cqe[j]);
        }
    }

    // End timing for the io_uring write loop
    uint64_t end_cycles = rdtsc();
    uint64_t total_cycles = end_cycles - start_cycles;

    // Display the timing results
    printf("Total cycles for write (io_uring, %s): %lu\n", sync ? "O_DSYNC" : "normal", total_cycles);
    printf("Cycles per byte (io_uring, %s): %.2f\n", sync ? "O_DSYNC" : "normal", (double)total_cycles / ((double)size * (double)loop_count));

    // Clean up
    io_uring_queue_exit(&ring);
    close(fd);
    free(buffer);
}

// Function to parse command-line arguments
void parse_arguments(int argc, char *argv[], const char **filename, size_t *size, int *sync) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <filename> <size> [loop_count] [batch_size] [sync]\n", argv[0]);
        exit(1);
    }
    *filename = argv[1];
    *size = (size_t)atoi(argv[2]);  // Explicitly cast atoi result to size_t
    if (argc > 3) loop_count = atoi(argv[3]);
    if (argc > 4) batch_size = atoi(argv[4]);
    if (argc > 5) *sync = atoi(argv[5]);  // Set sync flag if provided (1 for O_DSYNC)
    if (*size <= 0 || loop_count <= 0 || batch_size <= 0) {
        fprintf(stderr, "Invalid arguments. All sizes, loop_count, and batch_size must be positive integers.\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    const char *filename;
    size_t size;

    // Seed the random number generator
    srandom((unsigned int)time(NULL));

    // Parse command-line arguments
    parse_arguments(argc, argv, &filename, &size, &sync_flag);

    // Run and time write
    printf("Running write...\n");
    run_write(filename, size, sync_flag);

    // Run and time write with io_uring
    printf("Running write with io_uring...\n");
    run_write_uring(filename, size, sync_flag);

    return 0;
}

