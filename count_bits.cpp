#include <omp.h>
#include <cstdlib>
#include <iostream>
#include <fstream>

#include "boost/date_time/posix_time/posix_time.hpp" 
#include "boost/random/mersenne_twister.hpp"
#include "boost/random/uniform_int_distribution.hpp"

using namespace std;

typedef unsigned char uchar;

// A bit counting function is a function that takes a buffer
// and returns a count of the number of bits set.
typedef long bit_counting_function(const uchar *buffer, size_t bufsize);

// The various implementations of bit counting functions
bit_counting_function count_bits_naive; // Use simple C loop per bit
bit_counting_function count_bits_table; // Use simple C loop per byte, via a lookup table
bit_counting_function count_bits_kernighan; // Brian Kernighan's method
bit_counting_function count_bits_sidewaysaddition; // using magic binary numbers: http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetTable
bit_counting_function count_bits_intrinsic; // Use POPCNT intrinsic
bit_counting_function count_bits_asm; // Inline ASM loop with POPCNT

// Utility functions for implementations
long count_bits_asm_chunked(const uchar *buffer, size_t bufsize);
void init_lookup_table();
int num_threads();

// The SEE implementations work in long-sized chunks
typedef const unsigned long chunk_t;
typedef const unsigned long long double_chunk_t;
const static int chunk_size = sizeof(chunk_t);
const static int double_chunk_size = sizeof(double_chunk_t);
// A function to calculate the bits set for a single chunk
typedef long kernel_func(chunk_t _chunk);
typedef long kernel_func2(double_chunk_t _chunk);

#define B1 (~(chunk_t)0/3)
#define B2 (~(chunk_t)0/15*3)
#define B3 (~(chunk_t)0/255*15)
#define B4 (~(chunk_t)0/255)
#define S1 ((sizeof(chunk_t) - 1) * 8)
#define _CBITSa(val) \
                val = val - ((val >> 1) & B1);
#define _CBITSb(val) \
                val = (val & B2) + ((val >> 2) & B2);
#define _CBITSc(val) \
                val = (val + (val >> 4)) & B3;
#define _CBITS(val) \
        _CBITSa(val) _CBITSb(val) _CBITSc(val)

#define _CBITS2(val) (((chunk_t)(val * B4)) >> S1)



// How may trials to use for timing the slow and fast implementations
const int naive_iters = 10;
const int kernel_iters = 25;
const int fast_iters = 100;


// Iterate through the buffer one bit at a time
long count_bits_naive(const uchar *buffer, size_t bufsize)
{
    long bitcount = 0;
    for(size_t byte = 0; byte < bufsize; byte++)
        for(int bit = 0; bit < 8; bit++)
            if (buffer[byte] & (1 << bit))
                bitcount++;
    return bitcount;
}

// Count bits in a number of arbitrary size
template <class number_type>
long count_bits(number_type number)
{
    return count_bits_naive(reinterpret_cast<const uchar *>(&number), sizeof(number));
}

static int lookup_table[256];
void init_lookup_table()
{
    for (int i = 0; i < 256; i++)
        lookup_table[i] = count_bits(i);
}

// Count the bits by interating in word-sized chunks and
// using a kernel function that operates on words.
// Then, get the leftover bytes using the naive one-byte-at-a-time method.
template <kernel_func func>
long count_bits_kernel(const uchar *buffer, size_t bufsize)
{
    long total = 0;
    const long num_chunks = bufsize / chunk_size;
    const size_t chunked_bufsize = num_chunks * chunk_size;
    const int leftover = bufsize - chunked_bufsize;

#pragma omp parallel reduction (+:total)
    {
        long thread_total = 0;

#pragma omp for
        for (long i = 0; i < num_chunks; i++)
        {
            chunk_t chunk = *reinterpret_cast<chunk_t *>(buffer + i * chunk_size);
            thread_total += func(chunk);
        }

        total += thread_total;
    }

    total += count_bits_naive(buffer + chunked_bufsize, leftover);
    return total;
}

// Count the bits by interating in word-sized chunks and
// using a kernel function that operates on words.
// Then, get the leftover bytes using the naive one-byte-at-a-time method.
template <kernel_func2 func>
long count_bits_kernel_double(const uchar *buffer, size_t bufsize)
{
    long total = 0;
    const long num_chunks = bufsize / double_chunk_size;
    const size_t chunked_bufsize = num_chunks * double_chunk_size;
    const int leftover = bufsize - chunked_bufsize;

#pragma omp parallel reduction (+:total)
    {
        long thread_total = 0;

#pragma omp for
        for (long i = 0; i < num_chunks; i++)
        {
            double_chunk_t chunk = *reinterpret_cast<double_chunk_t *>(buffer + i * double_chunk_size);
            thread_total += func(chunk);
        }

        total += thread_total;
    }

    total += count_bits_naive(buffer + chunked_bufsize, leftover);
    return total;
}


// Count the bits by interating in word-sized chunks and
// using a kernel function that operates on words.
// Then, get the leftover bytes using the naive one-byte-at-a-time method.
template <kernel_func func, kernel_func func2, int numFunc1, int numFunc2>
long count_bits_kernel2(const uchar *buffer, size_t bufsize)
{
    long total = 0;
    const long num_chunks = bufsize / chunk_size;
    const size_t chunked_bufsize = num_chunks * chunk_size;
    const int leftover = bufsize - chunked_bufsize;

#pragma omp parallel reduction (+:total)
    {
        long thread_total = 0;
        int thread_id = omp_get_thread_num();
        int num_threads = omp_get_num_threads();

        if (thread_id < num_threads / (numFunc1 + numFunc2)) {
#pragma omp for
          for (long i = 0; i < num_chunks; i++)
          {
            chunk_t chunk = *reinterpret_cast<chunk_t *>(buffer + i * chunk_size);
            thread_total += func(chunk);
          }
        } else {
#pragma omp for
          for (long i = 0; i < num_chunks; i++)
          {
            chunk_t chunk = *reinterpret_cast<chunk_t *>(buffer + i * chunk_size);
            thread_total += func2(chunk);
          }
        }

        total += thread_total;
    }

    total += count_bits_naive(buffer + chunked_bufsize, leftover);
    return total;
}


// Count the bits using static lookup table
inline long table_kernel(chunk_t chunk)
{
    const uchar *buffer = reinterpret_cast<const uchar *>(&chunk);
    long total = 0;
    for(size_t byte = 0; byte < sizeof(chunk); byte++)
        total += lookup_table[buffer[byte]];
    return total;
}

long count_bits_table(const uchar *buffer, size_t bufsize)
{
    return count_bits_kernel<table_kernel>(buffer, bufsize);
}

inline long kernighan_kernel(chunk_t _chunk)
{
    long chunk = static_cast<long>(_chunk);
    long total = 0;
    while (chunk)
    {
        total++;
        chunk &= chunk - 1;
    }
    return total;
}

long count_bits_kernighan(const uchar *buffer, size_t bufsize)
{
    return count_bits_kernel<kernighan_kernel>(buffer, bufsize);
}

inline long sidewaysaddition_kernel(chunk_t _chunk)
{
    long chunk = _chunk;
    _CBITS(chunk);
    return _CBITS2(chunk);
}

long count_bits_sidewaysaddition(const uchar *buffer, size_t buffsize)
{
    return count_bits_kernel<sidewaysaddition_kernel>(buffer, buffsize);
}

inline long intrinsic_kernel(chunk_t chunk)
{
    return __builtin_popcountl(chunk);
}

// Count the bits using POPCNT instrinsic
long count_bits_intrinsic(const uchar *buffer, size_t bufsize)
{
    return count_bits_kernel<intrinsic_kernel>(buffer, bufsize);
}


inline long intrinsic_kernel_double(double_chunk_t chunk)
{
    return __builtin_popcountll(chunk);
}

// Count the bits using POPCNT instrinsic
long count_bits_intrinsic_double(const uchar *buffer, size_t bufsize)
{
    return count_bits_kernel_double<intrinsic_kernel_double>(buffer, bufsize);
}

// Count the bits using both POPCNT instrinsic and sideways addition
long count_bits_optimized(const uchar *buffer, size_t bufsize)
{
    return count_bits_kernel2<intrinsic_kernel, sidewaysaddition_kernel, 1, 1>(buffer, bufsize);
}


// Count the bits using inline ASM with POPCNT
long count_bits_asm(const uchar *buffer, size_t bufsize)
{
    const int num_cores = num_threads();
    const size_t num_chunks = bufsize / chunk_size;
    const size_t chunks_per_core = num_chunks / num_cores;
    const size_t bufsize_per_core = chunks_per_core * chunk_size;
    const size_t chunked_bufsize = num_cores * bufsize_per_core;
    const size_t leftover = bufsize - chunked_bufsize;

    long total = 0;

#pragma omp parallel for reduction (+:total)
    for (int core = 0; core < num_cores; core++)
    {
        const uchar *mybuffer = buffer + core * bufsize_per_core;
        const long num_bits = count_bits_asm_chunked(mybuffer, bufsize_per_core);
        total += num_bits;
    }

    total += count_bits_naive(buffer + chunked_bufsize, leftover);

    return total;
}

// Count the bits using inline ASM with POPCNT for a buffer that is divisible by chunk_size
inline long count_bits_asm_chunked(const uchar *buffer, size_t bufsize)
{
    size_t iterations = bufsize / chunk_size;
    if (!iterations)
        return 0;
    // This is a dummy output variable for the bitcount
    // calculated in each iteration.
    // Which is really a temporary register that we are clobbering.
    long bitcount;
    long total;

    __asm__ (
        // do {
        "1:"
        //     bitcount = popcnt(*buffer);
        "popcnt (%[buffer]), %[bitcount];"
        //     total += bitcount;
        "add %[bitcount], %[total];"
        //     buffer += chunk_size;
        "add %[chunk_size], %[buffer];"
        // } while(--total);
        "loop 1b;"

        // Output values
        :   [total]         "=&r"       (total), 
            [bitcount]      "=&r"       (bitcount),
            // ecx and buffer are really clobbered rather than output,
            // but gcc seems to like it better if we list them here.
            [ecx]           "=&c"       (iterations), 
            [buffer]        "=&r"       (buffer)

        // Input values
        :   [chunk_size]    "i"         (chunk_size), 
                            "[buffer]"  (buffer), 
                            "[ecx]"     (iterations), 
                            "[total]"   (0)

        // Clobbered registers
        // We pretty much declared them all as outputs, so they don't
        // need to be listed again.
        :   "cc"
    );
    return total;
}

int num_threads()
{
    int n_threads;
#pragma omp parallel
    {
#pragma omp master
        {
            n_threads = omp_get_num_threads();
        }
    }
    return n_threads > 0 ? n_threads : -1;
}


// Time how fast a bit counting function is
void time_bit_counting(const char *description, bit_counting_function *func, const uchar *buffer, size_t bufsize, int iters = fast_iters)
{
    // How many iterations represent roughly 10% of the total.
    // Used because We print a dot after every 10%.
    int ten_percent = iters / 10;
    if (ten_percent < 10)
        //  Just print a dot after every one
        ten_percent = 1;

    cout << endl << description;
    const boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();
    for (int i = 0; i < iters; i++)
    {
        long num_bits = func(buffer, bufsize);
        if (i == 0)
            cout << " (" << num_bits << " bits are set) ";
        else if (! (i % ten_percent))
            cout << ".";
    }
    const boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();
    const double duration = (end-start).total_microseconds() / 1000000.0;
    cout << endl << ((double)duration / iters) << " seconds per iteration" << endl;
}

uchar * init_buffer(long bufsize) {

    uchar *buffer = new unsigned char[bufsize];

    // Use /dev/urandom intead of /dev/random because
    // the latter may block if we try to read too much
    cout << "Generating random input... ";
#pragma omp parallel
    {
        int num_threads = omp_get_num_threads();
        int thread_id = omp_get_thread_num();

        long myBuffSize = bufsize / num_threads;
        ifstream infile("/dev/urandom", ios::binary);
        long myseed;
        infile.read((char*) &myseed, sizeof(myseed));
        infile.close();
        boost::random::mt11213b rng(myseed);
        boost::random::uniform_int_distribution<long> dist;

	long *myBuf = (long*) (buffer + myBuffSize * thread_id);
	long *myEnd = (long*) (buffer + myBuffSize * (thread_id+1));
        if (thread_id == num_threads-1) {
            myEnd = (long*) (buffer + bufsize);
        }
        while(myBuf != myEnd) {
            *myBuf++ = dist(rng);
        }
    }
    cout << "done." << endl;

    return buffer;

}


int main(int argc, char **argv)
{
    // Unbuffered stdout
    cout.setf(ios::unitbuf);

    // Figure out how much data we want
    size_t megs_of_data = 100; 
    if (argc > 1)
    {
        megs_of_data = atol(argv[1]);
    }
    if (!megs_of_data)
    {
        cerr << "Usage: " << argv[0] << " <megs of data>" << endl;
        return -1;
    }
    cout << "Using " << megs_of_data << " megs of data. wordsize: " << chunk_size << " double wordsize: " << double_chunk_size << endl;
    size_t bufsize = megs_of_data * 1024 * 1024;


    uchar *original_buffer = init_buffer(bufsize);

    // Let's make the data unaligned so it's even harder for SSE
    // who sometimes cares about such things
    uchar *buffer = original_buffer;
//    buffer += 1;
//    bufsize -= 1;

    init_lookup_table();

    time_bit_counting("Naive implementation",
                      count_bits_naive, buffer, bufsize, naive_iters);

    // Turn off parallelism
    int original_n_threads = num_threads();
    omp_set_num_threads(1);
    if (original_n_threads < 4) {

        time_bit_counting("Brian Kernighan's method (serial)",
                          count_bits_kernighan, buffer, bufsize, kernel_iters);
        time_bit_counting("Lookup table implementation (serial)",
                          count_bits_table, buffer, bufsize, kernel_iters);
    }
    time_bit_counting("Intrinsic implementation (serial)",
                      count_bits_intrinsic, buffer, bufsize);
    time_bit_counting("Intrinsic implementation double (serial)",
                      count_bits_intrinsic_double, buffer, bufsize);
    time_bit_counting("ASM implementation (serial)",
                      count_bits_asm, buffer, bufsize);
    time_bit_counting("Sideways Addition (serial)",
                      count_bits_sidewaysaddition, buffer, bufsize);
    

    if (original_n_threads > 1)
    {
        // Turn on parallelism
        omp_set_num_threads(original_n_threads);

        time_bit_counting("Brian Kernighan's method (parallel)",
                          count_bits_kernighan, buffer, bufsize, kernel_iters);
        time_bit_counting("Lookup table implementation (parallel)",
                          count_bits_table, buffer, bufsize, kernel_iters);
        time_bit_counting("Intrinsic implementation (parallel)",
                          count_bits_intrinsic, buffer, bufsize);
        time_bit_counting("Intrinsic implementation double (parallel)",
                          count_bits_intrinsic_double, buffer, bufsize);
        time_bit_counting("ASM implementation (parallel)",
                          count_bits_asm, buffer, bufsize);
        time_bit_counting("Sideways Addition (parallel)",
                          count_bits_sidewaysaddition, buffer, bufsize);
        time_bit_counting("Optimized hyperthread (parallel)",
                          count_bits_optimized, buffer, bufsize);
    }

    delete [] original_buffer;
    return 0;
}
