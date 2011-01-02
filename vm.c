#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

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

		/* series */
		LEN_POW2,
		OFF_FIXED,
		OFF_POW2,
		OFF_LIN,
		OFF_RAND,
		MAX_POW2,
		MAX_LIN,

		/* invalid */
		MAX_OPCODE,
	} code;

	unsigned int num;
	long long val;
	const char *string;

	ns_t *result;

	enum {
		NOP = 0,
		MINIMUM,
		MAXIMUM,
		AVERAGE,
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
	printf("%lld", op->result[0]);
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

static struct syntax syntax[] = {
	{ END,		nop,		0 },
	{ READ,		do_read,	ATOM },
	{ WRITE_ZERO,	nop,		ATOM },
	{ WRITE_ONE,	nop,		ATOM },
	{ WRITE_RAND,	nop,		ATOM },
	{ ERASE,	nop,		ATOM },
	{ PRINT,	print,		STRING },
	{ PRINT_NS,	print_ns,	AGGREGATE },
	{ PRINT_MBPS,	nop,		AGGREGATE },
	{ SEQUENCE,	sequence,	NUM },

	{ LEN_POW2,	len_pow2,	NUM | VAL },
	{ OFF_FIXED,	off_fixed,	VAL },
	{ OFF_POW2,	nop,		NUM | VAL },
	{ OFF_LIN,	off_lin,	NUM | VAL },
	{ OFF_RAND,	nop,		NUM | VAL },
	{ MAX_POW2,	nop,		NUM | VAL },
	{ MAX_LIN,	nop,		NUM | VAL },
};

struct operation program[] = {
	{ SEQUENCE, .num = 3 },
		{ PRINT, .string = "Hello, World!\n" },
		{ PRINT_NS },
			{ LEN_POW2, 1, 4096 },
				{ OFF_LIN, 1024, 4096 },
					{ READ },
		{ PRINT, .string = "\n" },
		{ END },
	{ END },
};
