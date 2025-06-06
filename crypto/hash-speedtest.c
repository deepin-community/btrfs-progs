/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <time.h>
#include <getopt.h>
#include <unistd.h>
#include <fnmatch.h>
#if HAVE_LINUX_PERF_EVENT_H == 1 && HAVE_LINUX_HW_BREAKPOINT_H == 1
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#define HAVE_PERF
#endif
#include "crypto/hash.h"
#include "common/messages.h"
#include "common/cpu-utils.h"

#ifdef __x86_64__
static const int cycles_supported = 1;
#else
static const int cycles_supported = 0;
#endif

enum {
	UNITS_CYCLES,
	UNITS_TIME,
	UNITS_PERF,
};

const int blocksize = 4096;
int iterations = 100000;

#ifdef __x86_64__
static __always_inline unsigned long long rdtsc(void)
{
	unsigned low, high;

        asm volatile("rdtsc" : "=a" (low), "=d" (high));

        return (low | ((u64)(high) << 32));
}

static inline u64 read_tsc(void)
{
       asm volatile("mfence");
       return rdtsc();
}

#define cpu_cycles()		read_tsc()

#else

#define cpu_cycles()		(0)

#endif

#ifdef HAVE_PERF

static int perf_fd = -1;
static int perf_init(void)
{
	static struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CPU_CYCLES
	};

	perf_fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
	return perf_fd;
}

static void perf_finish(void)
{
	close(perf_fd);
}

static long long perf_cycles(void)
{
	long long cycles;
	int ret;

	ret = read(perf_fd, &cycles, sizeof(cycles));
	if (ret != sizeof(cycles))
		return 0;
	return cycles;
}

#else
static int perf_init()
{
	errno = EOPNOTSUPP;
	return -1;
}
static void perf_finish() {}
static long long perf_cycles() {
	return 0;
}
#endif

static inline u64 get_time(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static inline u64 get_cycles(int units)
{
	switch (units) {
	case UNITS_CYCLES: return cpu_cycles();
	case UNITS_TIME:   return get_time();
	case UNITS_PERF:   return perf_cycles();
	}
	return 0;
}

/* Read the input and copy last bytes as the hash */
static int hash_null_memcpy(const u8 *buf, size_t length, u8 *out)
{
       const u8 *end = buf + length;

       while (buf + CRYPTO_HASH_SIZE_MAX < end) {
               memcpy(out, buf, CRYPTO_HASH_SIZE_MAX);
               buf += CRYPTO_HASH_SIZE_MAX;
       }

       return 0;
}

/* Test overhead of the calls */
static int hash_null_nop(const u8 *buf, size_t length, u8 *out)
{
       memset(out, 0xFF, CRYPTO_HASH_SIZE_MAX);

       return 0;
}

static const char *units_to_desc(int units)
{
	switch (units) {
	case UNITS_CYCLES: return "CPU cycles";
	case UNITS_TIME:   return "time: ns";
	case UNITS_PERF:   return "perf event: CPU cycles";
	}
	return "unknown";
}

static const char *units_to_str(int units)
{
	switch (units) {
	case UNITS_CYCLES: return "cycles";
	case UNITS_TIME:   return "nsecs";
	case UNITS_PERF:   return "perf_c";
	}
	return "unknown";
}

int main(int argc, char **argv) {
	u8 buf[blocksize];
	u8 hash[32];
	int idx;
	int iter;
	char *filter = NULL;
	struct contestant {
		char name[16];
		int (*digest)(const u8 *buf, size_t length, u8 *out);
		int digest_size;
		u64 cycles;
		u64 time;
		unsigned long cpu_flag;
		int backend;
	} contestants[] = {
		{ .name = "NULL-NOP", .digest = hash_null_nop, .digest_size = 32 },
		{ .name = "NULL-MEMCPY", .digest = hash_null_memcpy, .digest_size = 32 },
		{ .name = "CRC32C-ref", .digest = hash_crc32c, .digest_size = 4,
		  .cpu_flag = CPU_FLAG_NONE },
		{ .name = "CRC32C-NI", .digest = hash_crc32c, .digest_size = 4,
		  .cpu_flag = CPU_FLAG_PCLMUL },
		{ .name = "XXHASH", .digest = hash_xxhash, .digest_size = 8 },
		{ .name = "SHA256-ref", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
		{ .name = "SHA256-gcrypt", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBGCRYPT + 1 },
		{ .name = "SHA256-sodium", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBSODIUM + 1 },
		{ .name = "SHA256-kcapi", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBKCAPI + 1 },
		{ .name = "SHA256-botan", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_BOTAN + 1 },
		{ .name = "SHA256-openssl", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_OPENSSL + 1 },
		{ .name = "SHA256-NI", .digest = hash_sha256, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_SHA, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
		{ .name = "BLAKE2-ref", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
		{ .name = "BLAKE2-gcrypt", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBGCRYPT + 1 },
		{ .name = "BLAKE2-sodium", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBSODIUM + 1 },
		{ .name = "BLAKE2-kcapi", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_LIBKCAPI + 1 },
		{ .name = "BLAKE2-botan", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_BOTAN + 1 },
		{ .name = "BLAKE2-openssl", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_NONE, .backend = CRYPTOPROVIDER_OPENSSL + 1 },
		{ .name = "BLAKE2-SSE2", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_SSE2, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
		{ .name = "BLAKE2-SSE41", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_SSE41, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
		{ .name = "BLAKE2-AVX2", .digest = hash_blake2b, .digest_size = 32,
		  .cpu_flag = CPU_FLAG_AVX2, .backend = CRYPTOPROVIDER_BUILTIN + 1 },
	};
	int units = UNITS_CYCLES;

	cpu_detect_flags();
	cpu_print_flags();
	hash_init_accel();

	optind = 0;
	while (1) {
		static const struct option long_options[] = {
			{ "cycles", no_argument, NULL, 'c' },
			{ "time", no_argument, NULL, 't' },
			{ "perf", no_argument, NULL, 'p' },
			{ "filter", required_argument, NULL, 'f' },
			{ NULL, 0, NULL, 0}
		};
		int c;

		c = getopt_long(argc, argv, "cf:tp", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'c':
			if (!cycles_supported) {
				error("cannot measure cycles on this arch, use --time");
				return 1;
			}
			units = UNITS_CYCLES;
			break;
		case 'f':
			free(filter);
			filter = strdup(optarg);
			printf("Use filter: %s\n", filter);
			break;
		case 't':
			units = UNITS_TIME;
			break;
		case 'p':
			if (perf_init() == -1) {
				error(
"cannot initialize perf, please check sysctl kernel.perf_event_paranoid: %m");
				return 1;
			}
			units = UNITS_PERF;
			break;
		default:
			error("unknown option");
			return 1;
		}
	}

	if (argc - optind >= 1) {
		iterations = atoi(argv[optind]);
		if (iterations < 1)
			iterations = 1;
	}

	memset(buf, 0, 4096);

	printf("Block size:     %d\n", blocksize);
	printf("Iterations:     %d\n", iterations);
	printf("Implementation: %s\n", CRYPTOPROVIDER);
	printf("Units:          %s\n", units_to_desc(units));
	printf("\n");

	for (idx = 0; idx < ARRAY_SIZE(contestants); idx++) {
		struct contestant *c = &contestants[idx];
		u64 start, end;
		u64 tstart, tend;
		u64 total = 0;

		if (c->cpu_flag != 0 && !cpu_has_feature(c->cpu_flag)) {
			printf("%14s: no CPU support\n", c->name);
			continue;
		}
		/* Backend not compiled in */
		if (c->backend == 1)
			continue;

		if (c->digest == hash_null_memcpy)
			/* Always run NULL-MEMCPY to warm up memory. */;
		else if (filter && fnmatch(filter, c->name, FNM_CASEFOLD) != 0)
			continue;

		printf("%14s: ", c->name);
		fflush(stdout);

		if (c->cpu_flag) {
			cpu_set_level(c->cpu_flag);
			hash_init_accel();
		}
		tstart = get_time();
		start = get_cycles(units);
		for (iter = 0; iter < iterations; iter++) {
			memset(buf, iter & 0xFF, blocksize);
			memset(hash, 0, 32);
			c->digest(buf, blocksize, hash);
		}
		end = get_cycles(units);
		tend = get_time();
		c->cycles = end - start;
		c->time = tend - tstart;
		cpu_reset_level();

		if (units == UNITS_CYCLES || units == UNITS_PERF)
			total = c->cycles;
		if (units == UNITS_TIME)
			total = c->time;

		printf("%s: %12llu, %s/i %8llu",
				units_to_str(units), total,
				units_to_str(units), total / iterations);
		if (idx > 0) {
			float t;
			float mb;

			t = (float)c->time / 1000 / 1000 / 1000;
			mb = blocksize * iterations / 1024 / 1024;
			printf(", %12.3f MiB/s", mb / t);
		}
		putchar('\n');
	}
	perf_finish();
	free(filter);

	return 0;
}
