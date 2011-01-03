#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>

struct device;

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

static inline res_t *res_ptr(res_t r)
{
	return (res_t *)((unsigned long)r._p & ~7);
}

static inline enum resulttype res_type(res_t r)
{
	return (enum resulttype)((unsigned long)r._p & 7);
}

static inline res_t to_res(res_t *_p, enum resulttype t)
{
	return (res_t) { ._p = (res_t *)(((unsigned long)_p & ~7) | (t & 7)) };
}

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

		/* output */
		O_PRINT,
		O_PRINT_NS,
		O_PRINT_MBPS,

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

		/* end of list */
		O_MAX = O_REDUCE,
	} code;

	/* number of indirect results, if any */
	unsigned int num;

	/* command code specific value */
	long long val;

	/* output string for O_PRINT */
	const char *string;

	/* aggregation of results from children */
	enum {
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

struct syntax {
	enum opcode opcode;
	struct operation *(*function)(struct operation *op, struct device *dev,
			 off_t off, off_t max, size_t len);
	enum {
		P_NUM = 1,
		P_VAL = 2,
		P_STRING = 4,
		P_AGGREGATE = 8,
		P_ATOM = 16,
	} param;
};

static struct syntax syntax[];

static struct operation *call(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	if (!op)
		return NULL;

	if (op->code > O_MAX)
		return NULL;

	if (!(syntax[op->code].param & P_NUM) != !op->num)
		return NULL;
	if (!(syntax[op->code].param & P_VAL) != !op->val)
		return NULL;
	if (!(syntax[op->code].param & P_STRING) != !op->string)
		return NULL;
	if (!(syntax[op->code].param & P_AGGREGATE) != !op->aggregate)
		return NULL;

	if (op->num && !res_ptr(op->result)) {
		res_t *data = calloc(sizeof (res_t), op->num);
		if (!data)
			return NULL;

		op->result = to_res(data, R_NONE);
		op->r_type = R_ARRAY;
	}

	next = syntax[op->code].function(op, dev, off, max, len);

	return next;
}

static struct operation *call_propagate(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len, struct operation *this)
{
	struct operation *next;

	next = call(op, dev, off, max, len);

	this->result = op->result;
	this->size_x = op->size_x;
	this->size_y = op->size_y;
	this->r_type = op->r_type;

	op->result = to_res(NULL, R_NONE);
	op->size_x = op->size_y = 0;
	op->r_type = R_NONE;

	return next;
}

static struct operation *call_aggregate(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len, struct operation *this)
{
	struct operation *next;
	res_t *res = res_ptr(this->result);
	enum resulttype type = res_type(this->result);

	next = call(op, dev, off, max, len);

	res[this->size_x] = op->result;
	this->size_x++;

	if (type == R_NONE) {
		type = op->r_type;
		this->result = to_res(res, type);
	}

	if (type != op->r_type)
		return NULL;

	if (op->r_type == R_ARRAY) {
		if (this->size_y && this->size_y != op->size_x)
			return NULL;

		if (op->size_y)
			return NULL;

		this->size_y = op->size_x;

		op->size_x = op->size_y = 0;
		op->result = to_res(NULL, R_NONE);
	}

	return next;
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

	next = call_propagate(op+1, dev, off, max, len, op);

	if (op->size_x || op->size_y || op->r_type != R_NS)
		return NULL;

	printf("%lld", op->result.l);
	return next;
}

static struct operation *sequence(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next = op+1;

	for (i=0; i<op->num; i++)
		next = call_aggregate(next, dev, off, max, len, op);

	if (next->code != O_END)
		return NULL;

	return next+1;
}

static struct operation *len_pow2(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next;

	if (!len)
		len = 1;

	for (i = 0; i < op->num; i++)
		next = call_aggregate(op+1, dev, off, max, len * op->val << i, op);

	return next;
}

static struct operation *off_fixed(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return call_propagate(op+1, dev, off + op->val, max, len, op);
}

static struct operation *off_lin(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;
	unsigned int i;

	for (i = 0; i < op->num; i++)
		next = call_aggregate(op+1, dev, off + i * op->val, max, len, op);

	return next;
}

static struct operation *repeat(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;
	unsigned int i;

	for (i = 0; i < op->num; i++)
		next = call_aggregate(op+1, dev, off, max, len, op);

	return next;
}

static res_t do_reduce(int num, res_t *input, int aggregate)
{
	int i;
	res_t result = { .l = 0 };

	for (i = 0; i < num; i++) {
		switch (aggregate) {
		case A_MINIMUM:
			if (!result.l || result.l > input[i].l)
				result.l = input[i].l;
			break;
		case A_MAXIMUM:
			if (!result.l || result.l < input[i].l)
				result.l = input[i].l;
			break;
		case A_AVERAGE:
		case A_TOTAL:
			result.l += input[i].l;
			break;
		}
	}

	if (aggregate == A_AVERAGE)
		result.l /= num;

	return result;
}

static struct operation *reduce(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next, *child;
	unsigned int i;
	enum resulttype type;
	res_t *in;

	child = op+1;
	next = call(child, dev, off, max, len);

	/* single value */
	if (op->r_type != R_ARRAY || op->size_y == 0)
		return NULL;

	/* data does not fit */
	if (child->size_y > op->num)
		return NULL;

	/* one-dimensional array */
	if (op[1].size_y == 0) {
		op->result = do_reduce(child->size_x, res_ptr(child->result),
					op->aggregate);
		op->size_x = 0;
		op->size_y = 0;
		op->r_type = res_type(child->result);
		return next;
	}

	/* two-dimensional array */
	in = res_ptr(child->result);
	if (res_type(child->result) != R_ARRAY)
		return NULL;

	type = res_type(in[0]);
	for (i=0; i<child->size_y; i++) {
		if (res_type(in[i]) != type)
			return NULL;

		res_ptr(op->result)[i] = do_reduce(child->size_x, res_ptr(in[i]),
						   op->aggregate);
	}
	op->result = to_res(res_ptr(op->result), type);
	op->size_x = child->size_y;
	op->size_y = 0;
	op->r_type = R_ARRAY;

	return next;
}

static struct syntax syntax[] = {
	{ O_END,	nop,		0 },
	{ O_READ,	do_read,	P_ATOM },
	{ O_WRITE_ZERO,	nop,		P_ATOM },
	{ O_WRITE_ONE,	nop,		P_ATOM },
	{ O_WRITE_RAND,	nop,		P_ATOM },
	{ O_ERASE,	nop,		P_ATOM },

	{ O_PRINT,	print,		P_STRING },
	{ O_PRINT_NS,	print_ns,	0 },
	{ O_PRINT_MBPS,	nop,		0 },

	{ O_SEQUENCE,	sequence,	P_NUM },
	{ O_REPEAT,	repeat,		P_NUM },

	{ O_OFF_FIXED,	off_fixed,	P_VAL },
	{ O_OFF_POW2,	nop,		P_NUM | P_VAL },
	{ O_OFF_LIN,	off_lin,	P_NUM | P_VAL },
	{ O_OFF_RAND,	nop,		P_NUM | P_VAL },
	{ O_LEN_POW2,	len_pow2,	P_NUM | P_VAL },
	{ O_MAX_POW2,	nop,		P_NUM | P_VAL },
	{ O_MAX_LIN,	nop,		P_NUM | P_VAL },

	{ O_REDUCE,	reduce,		P_NUM },
};

struct operation program[] = {
	{ O_SEQUENCE, .num = 3 },
		{ O_PRINT, .string = "Hello, World!\n" },
		{ O_PRINT_NS },
			{ O_REDUCE, 1024 },
			{ O_LEN_POW2, 4, 4096 },
				{ O_OFF_LIN, 1024, 4096, .aggregate = A_MINIMUM },
					{ O_READ },
		{ O_PRINT, .string = "\n" },
		{ O_END },
	{ O_END },
};
