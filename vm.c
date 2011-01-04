#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

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

static const res_t res_null = { };

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
		O_PRINTF,
		O_PRINT_MBPS,
		O_FORMAT,

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
	const char *name;
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
static int verbose = 0;
#define pr_debug(...) do { if (verbose) printf(__VA_ARGS__); } while(0)
#define return_err(...) do { printf(__VA_ARGS__); return NULL; } while(0)

static struct operation *call(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	if (!op)
		return_err("internal error: NULL operation\n");

	pr_debug("call %s %ld %ld %ld\n", syntax[op->code].name, off, max, len);

	if (op->code > O_MAX)
		return_err("illegal command code %d\n", op->code);

	if (!(syntax[op->code].param & P_NUM) != !op->num)
		return_err("need .num= argument\n");
	if (!(syntax[op->code].param & P_VAL) != !op->val)
		return_err("need .param= argument\n");
	if (!(syntax[op->code].param & P_STRING) != !op->string)
		return_err("need .string= argument\n");
	if (!(syntax[op->code].param & P_AGGREGATE) != !op->aggregate)
		return_err("need .aggregate= argument\n");

	if (op->num && res_ptr(op->result))
		return_err("%s already has result\n", syntax[op->code].name);

	if (op->num && !res_ptr(op->result)) {
		res_t *data = calloc(sizeof (res_t), op->num);
		if (!data)
			return_err("out of memory");

		op->result = to_res(data, R_NONE);
		op->r_type = R_ARRAY;
	}

	next = syntax[op->code].function(op, dev, off, max, len);
	if (!next)
		return_err("from %s\n", syntax[op->code].name);

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

	op->result = res_null;
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
	if (!next)
		return NULL;

	res[this->size_x] = op->result;

	if (op->r_type == R_NONE)
		return next;

	this->size_x++;

	if (type == R_NONE) {
		type = op->r_type;
		this->result = to_res(res, type);
	}

	if (type != op->r_type)
		return_err("cannot aggregate return type %d with %d\n",
				type, op->r_type);

	if (op->r_type == R_ARRAY) {
		if (this->size_y && this->size_y != op->size_x)
			return_err("cannot aggregate different size arrays (%d, %d)\n",
					this->size_y, op->size_x);

		if (op->size_y)
			return_err("cannot aggregate three-dimensional array\n");

		this->size_y = op->size_x;

		op->size_x = op->size_y = 0;
		op->result = res_null;
	}

	return next;
}

static struct operation *nop(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return_err("command not implemented\n");
}

static struct operation *do_read(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	/* FIXME */
	pr_debug("read %ld %ld %ld\n", off, max, len);
	op->result.l = 0;
	op->r_type = R_NS;
	return op+1;
}

static res_t format_value(res_t val, enum resulttype type,
			unsigned int size_x, unsigned int size_y)
{
	long long ns = val.l;
	unsigned int x;
	res_t *res;
	res_t out;

	switch (type) {
	case R_ARRAY:
		res = res_ptr(val);
		for (x = 0; x < size_x; x++) {
			res[x] = format_value(res[x], res_type(val), size_y, 0);
			if (res[x].s == res_null.s)
				return res_null;
		}
		if (res_type(val) == R_ARRAY)
			out = val;
		else
			out = to_res(res_ptr(val), R_STRING);
		break;
	case R_NS:
		if (ns < 1000)
			snprintf(out.s, 8, "%lldns", ns);
		else if (ns < 1000 * 1000)
			snprintf(out.s, 8, "%.3gµs", ns / 1000.0);
		else if (ns < 1000 * 1000 * 1000)
			snprintf(out.s, 8, "%.3gms", ns / 1000000.0);
		else
			snprintf(out.s, 8, "%.4gs", ns / 1000000000.0);
		break;
	default:
		out = res_null;
	}

	return out;
}

static struct operation *format(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	next = call_propagate(op+1, dev, off, max, len, op);
	op->result = format_value(op->result, op->r_type, op->size_x, op->size_y);

	if (op->result.s == res_null.s)
		return NULL;

	if (op->r_type != R_ARRAY)
		op->r_type = R_STRING;

	return next;
}

static struct operation *print(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	printf("%s", op->string);
	return op+1;
}

static void *print_value(res_t val, enum resulttype type,
			unsigned int size_x, unsigned int size_y)
{
	unsigned int x;
	res_t *res;

	switch (type) {
	case R_ARRAY:
		res = res_ptr(val);
		for (x=0; x < size_x; x++) {
			if (!print_value(res[x], res_type(val), size_y, 0))
				return_err("cannot print array of type %d\n",
					res_type(val));
			printf(size_y ? "\n" : " ");
		}


		break;
	case R_BYTE:
	case R_NS:
		printf("%lld", val.l);
		break;
	case R_STRING:
		printf("%s", val.s);
		break;
	default:
		return NULL;
	}
	return (void *)1;
}

static struct operation *print_val(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	next = call_propagate(op+1, dev, off, max, len, op);
	if (!next)
		return NULL;

	if (!print_value(op->result, op->r_type, op->size_x, op->size_y))
		return_err("cannot print value of type %d\n", op->r_type);

	return next;
}

static struct operation *sequence(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next = op+1;

	for (i=0; i<op->num; i++) {
		next = call_aggregate(next, dev, off, max, len, op);
		if (!next)
			return NULL;
	}

	/* immediately fold sequences with a single result */
	if (op->size_x == 1) {
		op->r_type = res_type(op->result);
		op->result = res_ptr(op->result)[0];
		op->size_x = op->size_y;
		op->size_y = 0;
	}

	if (next && next->code != O_END)
		return_err("sequence needs to end with END command\n");

	return next+1;
}

static struct operation *len_pow2(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next = op+1;

	if (!len)
		len = 1;

	for (i = 0; i < op->num && next; i++)
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
	struct operation *next = op+1;
	unsigned int i;

	for (i = 0; i < op->num && next; i++)
		next = call_aggregate(op+1, dev, off + i * op->val, max, len, op);

	return next;
}

static struct operation *repeat(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next = op+1;
	unsigned int i;

	for (i = 0; i < op->num && next; i++)
		next = call_aggregate(op+1, dev, off, max, len, op);

	return next;
}

static res_t do_reduce_int(int num, res_t *input, int aggregate)
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
	if (!next)
		return NULL;

	/* single value */
	if (child->r_type != R_ARRAY || child->size_x == 0)
		return_err("cannot reduce scalar further, type %d, size %d\n",
				child->r_type, child->size_y);

	/* data does not fit */
	if (child->size_y > op->num)
		return_err("target array too short\n"); /* FIXME: is this necessary? */

	/* one-dimensional array */
	if (child->size_y == 0) {
		if (res_type(child->result) != R_NS)
			return_err("cannot reduce type %d\n", res_type(child->result));

		op->result = do_reduce_int(child->size_x, res_ptr(child->result),
					op->aggregate);
		op->size_x = op->size_y = 0;
		op->r_type = res_type(child->result);
		goto clear_child;
	}

	/* two-dimensional array */
	in = res_ptr(child->result);
	if (res_type(child->result) != R_ARRAY)
		return_err("inconsistent array contents\n");

	type = res_type(in[0]);
	for (i=0; i<child->size_x; i++) {
		if (res_type(in[i]) != type)
			return_err("cannot combine type %d and %d\n",
				 res_type(in[i]), type);

		res_ptr(op->result)[i] = do_reduce_int(child->size_x, res_ptr(in[i]),
						   op->aggregate);
	}
	op->result = to_res(res_ptr(op->result), type);
	op->size_x = child->size_y;
	op->size_y = 0;
	op->r_type = R_ARRAY;

clear_child:
	child->result = res_null;
	child->size_x = child->size_y = 0;
	child->r_type = R_NONE;

	return next;
}

static struct syntax syntax[] = {
	{ O_END,	"END",		nop,		0 },
	{ O_READ,	"READ",		do_read,	P_ATOM },
	{ O_WRITE_ZERO,	"WRITE_ZERO",	nop,		P_ATOM },
	{ O_WRITE_ONE,	"WRITE_ONE",	nop,		P_ATOM },
	{ O_WRITE_RAND,	"WRITE_RAND",	nop,		P_ATOM },
	{ O_ERASE,	"ERASE",	nop,		P_ATOM },

	{ O_PRINT,	"PRINT",	print,		P_STRING },
	{ O_PRINTF,	"PRINTF",	print_val,	0 },
	{ O_PRINT_MBPS,	"PRINT_MBPS",	nop,		0 },
	{ O_FORMAT,	"FORMAT",	format,		0 },

	{ O_SEQUENCE,	"SEQUENCE",	sequence,	P_NUM },
	{ O_REPEAT,	"REPEAT",	repeat,		P_NUM },

	{ O_OFF_FIXED,	"OFF_FIXED",	off_fixed,	P_VAL },
	{ O_OFF_POW2,	"OFF_POW2",	nop,		P_NUM | P_VAL },
	{ O_OFF_LIN,	"OFF_LIN",	off_lin,	P_NUM | P_VAL },
	{ O_OFF_RAND,	"OFF_RAND",	nop,		P_NUM | P_VAL },
	{ O_LEN_POW2,	"LEN_POW2",	len_pow2,	P_NUM | P_VAL },
	{ O_MAX_POW2,	"MAX_POW2",	nop,		P_NUM | P_VAL },
	{ O_MAX_LIN,	"MAX_LIN",	nop,		P_NUM | P_VAL },

	{ O_REDUCE,	"REDUCE",	reduce,		P_NUM },
};

struct operation program[] = {
	{ O_REPEAT, 4 },
	{ O_SEQUENCE, .num = 3 },
	    { O_PRINT, .string = "Hello, World!\n" },
	    { O_REPEAT, 1 },
		{ O_PRINTF },
			    { O_FORMAT },
		    { O_REDUCE, 8 },
			{ O_REDUCE, 8 },
			    { O_LEN_POW2, 4, 4096 },
				{ O_OFF_LIN, 8, 4096 },//, .aggregate = A_MINIMUM },
				    { O_READ },
		{ O_PRINT, .string = "\n" },
		{ O_END },
	{ O_END },
};

int main(void)
{
	call(program, NULL, 0, 4096 * 1024, 512);

	return 0;
}
