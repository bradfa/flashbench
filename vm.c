#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>

typedef long long ns_t;
struct device;

struct operation {
	enum opcode {
		/* end of program marker */
		END = 0,

		/* basic operations */
		READ,
		WRITE_ZERO,
		WRITE_ONE,
		WRITE_RAND,
		ERASE,

		/* output */
		PRINT,
		PRINT_NS,
		PRINT_MBPS,

		/* group */
		SEQUENCE,
		REPEAT,

		/* series */
		OFF_FIXED,
		OFF_POW2,
		OFF_LIN,
		OFF_RAND,
		LEN_POW2,
		MAX_POW2,
		MAX_LIN,

		/* reduce dimension */
		REDUCE,

		/* invalid */
		MAX_OPCODE,
	} code;

	unsigned int num;

	long long val;
	const char *string;

	unsigned int dim;
	union {
		ns_t r1;
		ns_t *r2;
		ns_t **r3;
	} result;

	enum {
		MINIMUM,
		MAXIMUM,
		AVERAGE,
		TOTAL,
		IGNORE,
	} aggregate;
};

struct syntax {
	enum opcode opcode;
	struct operation *(*function)(struct operation *op, struct device *dev,
			 off_t off, off_t max, size_t len);
	enum {
		NUM = 1,
		VAL = 2,
		STRING = 4,
		AGGREGATE = 8,
		ATOM = 16,
	} args;
};

static struct syntax syntax[];

static struct operation *call(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	if (!op)
		return NULL;

	if (op->code >= MAX_OPCODE)
		return NULL;

	if (!(syntax[op->code].args & NUM) != !op->num)
		return NULL;
	if (!(syntax[op->code].args & VAL) != !op->val)
		return NULL;
	if (!(syntax[op->code].args & STRING) != !op->string)
		return NULL;
	if (!(syntax[op->code].args & AGGREGATE) != !op->aggregate)
		return NULL;

	return syntax[op->code].function(op, dev, off, max, len);
}

static struct operation *nop(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return NULL;
}

static struct operation *do_read(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return op+1;
}

static struct operation *print(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	printf("%s", op->string);
	return op+1;
}

static struct operation *print_ns(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	op++;
	next = call(op, dev, off, max, len);
	if (op->dim > 1)
		return NULL;

	printf("%lld", op->result.r1);
	return next;
}

static struct operation *sequence(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int num = op->num;

	op++;
	while (num--)
		op = call(op, dev, off, max, len);

	if (op->code != END)
		return NULL;

	return op++;
}

static struct operation *len_pow2(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next;

	if (!len)
		len = 1;

	for (i = 0; i < op->num; i++)
		next = call(op+1, dev, off, max, len * op->val << i);

	return next;
}

static struct operation *off_fixed(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return call(op+1, dev, off + op->val, max, len);
}

static struct operation *off_lin(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;
	unsigned int i;

	for (i = 0; i < op->num; i++)
		next = call(op+1, dev, off + i * op->val, max, len);

	return next;
}

static struct operation *repeat(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;
	unsigned int i;

	for (i = 0; i < op->num; i++)
		next = call(op+1, dev, off, max, len);

	return next;
}

static ns_t do_reduce1(int num, ns_t *input, int aggregate)
{
	int i;
	ns_t result = 0;

	for (i = 0; i < num; i++) {
		switch (aggregate) {
		case MINIMUM:
			if (!result || result > input[i])
				result = input[i];
			break;
		case MAXIMUM:
			if (!result || result < input[i])
				result = input[i];
			break;
		case AVERAGE:
		case TOTAL:
			result += input[i];
			break;
		}
	}

	if (aggregate == AVERAGE)
		result /= num;

	return result;
}

static void do_reduce2(int num_out, ns_t *output,
			int num_in, ns_t **input,
			int aggregate)
{
	int i, j;
	ns_t result = 0;
	
	for (j = 0; j < num_out; j++) {
		output[j] = 0;
		for (i = 0; i < num_in; i++) {
			switch (aggregate) {
			case MINIMUM:
				if (!output[j] || result > input[i][j])
					output[j] = input[i][j];
				break;
			case MAXIMUM:
				if (!output[j] || result < input[i][j])
					output[j] = input[i][j];
				break;
			case AVERAGE:
			case TOTAL:
				output[j] += input[i][j];
				break;
			}

		if (aggregate == AVERAGE)
			output[j] /= num_in;
		}
	}
}

static struct operation *reduce(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	next = call(op+1, dev, off, max, len);

	switch (op->dim) {
	case 1:
		op->result.r1 = do_reduce1(op[1].num, op[1].result.r2,
						op->aggregate);
		break;
	case 2:
		do_reduce2(op->num, op->result.r2, op[1].num, op[1].result.r3,
				 op->aggregate);
		break;
	default:
		return NULL;
	}

	return next;
}

static struct syntax syntax[] = {
	{ END,		nop,		0 },
	{ READ,		do_read,	ATOM },
	{ WRITE_ZERO,	nop,		ATOM },
	{ WRITE_ONE,	nop,		ATOM },
	{ WRITE_RAND,	nop,		ATOM },
	{ ERASE,	nop,		ATOM },

	{ PRINT,	print,		STRING },
	{ PRINT_NS,	print_ns,	0 },
	{ PRINT_MBPS,	nop,		0 },

	{ SEQUENCE,	sequence,	NUM },
	{ REPEAT,	repeat,		NUM },

	{ OFF_FIXED,	off_fixed,	VAL },
	{ OFF_POW2,	nop,		NUM | VAL },
	{ OFF_LIN,	off_lin,	NUM | VAL },
	{ OFF_RAND,	nop,		NUM | VAL },
	{ LEN_POW2,	len_pow2,	NUM | VAL },
	{ MAX_POW2,	nop,		NUM | VAL },
	{ MAX_LIN,	nop,		NUM | VAL },

	{ REDUCE,	reduce,		NUM },
};

struct operation program[] = {
	{ SEQUENCE, .num = 3 },
		{ PRINT, .string = "Hello, World!\n" },
		{ PRINT_NS },
			{ REDUCE, 1024, .dim = 2 },
			{ LEN_POW2, 4, 4096 },
				{ OFF_LIN, 1024, 4096, .aggregate = MINIMUM },
					{ READ },
		{ PRINT, .string = "\n" },
		{ END },
	{ END },
};
