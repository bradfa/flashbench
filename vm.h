#ifndef FLASHBENCH_VM_H
#define FLASHBENCH_VM_H

#include <sys/types.h>

typedef union result res_t;

enum resulttype {
	R_NONE,
	R_ARRAY,
	R_NS,
	R_BYTE,
	R_STRING,
};

union result {
	res_t *_p;
	long long l;
	char s[8];
} __attribute__((aligned(8)));

struct device;

struct operation {
	enum opcode {
		/* end of program marker */
		O_END = 0,

		/* basic operations */
		O_READ,
		O_WRITE_ZERO,
		O_WRITE_ONE,
		O_WRITE_RAND,
		O_ERASE,
		O_LENGTH,
		O_OFFSET,

		/* output */
		O_PRINT,
		O_PRINTF,
		O_FORMAT,
		O_NEWLINE,

		/* group */
		O_SEQUENCE,
		O_REPEAT,

		/* series */
		O_OFF_FIXED,
		O_OFF_POW2,
		O_OFF_LIN,
		O_OFF_RAND,
		O_LEN_POW2,
		O_MAX_POW2,
		O_MAX_LIN,

		/* reduce dimension */
		O_REDUCE,

		/* ignore result */
		O_DROP,

		/* end of list */
		O_MAX = O_DROP,
	} code;

	/* number of indirect results, if any */
	unsigned int num;

	/* command code specific value */
	long long val;

	/* output string for O_PRINT */
	const char *string;

	/* aggregation of results from children */
	enum {
		A_NONE,
		A_MINIMUM,
		A_MAXIMUM,
		A_AVERAGE,
		A_TOTAL,
		A_IGNORE,
	} aggregate;

	/* dynamic result contents */
	res_t		result;
	unsigned int	size_x;
	unsigned int	size_y;
	enum resulttype r_type;
};

extern struct operation *call(struct operation *program, struct device *dev,
		 off_t off, off_t max, size_t len);

extern int verbose;
#define pr_debug(...) do { if (verbose) printf(__VA_ARGS__); } while(0)
#define return_err(...) do { printf(__VA_ARGS__); return NULL; } while(0)

#endif /* FLASHBENCH_VM_H */
