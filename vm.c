#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "vm.h"

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

int verbose = 0;

struct operation *call(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;

	if (!op)
		return_err("internal error: NULL operation\n");

	pr_debug("call %s %lld %lld %ld\n", syntax[op->code].name,
		(long long)off, (long long)max, (long)len);

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

	if (op->num) {
		res_t *data;

		if (res_ptr(op->result))
			return_err("%s already has result\n", syntax[op->code].name);

		data = calloc(sizeof (res_t), op->num);
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

	if (this->size_x >= this->num)
		return_err("array too small for %d entries\n", this->size_x);

	res[this->size_x] = op->result;

	/* no result */
	if (op->r_type == R_NONE)
		return next;

	this->size_x++;
	/* first data in this aggregation: set type */
	if (type == R_NONE) {
		type = op->r_type;
		this->result = to_res(res, type);
	}

	if (type != op->r_type) {
		return_err("cannot aggregate return type %d with %d\n",
				type, op->r_type);
	}

	if (op->r_type == R_ARRAY) {
		if (this->size_y && this->size_y != op->size_x)
			return_err("cannot aggregate different size arrays (%d, %d)\n",
					this->size_y, op->size_x);

		if (op->size_y)
			return_err("cannot aggregate three-dimensional array\n");

		this->size_y = op->size_x;

		op->size_x = op->size_y = 0;
	}

	op->r_type = R_NONE;
	op->result = res_null;

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
	op->result.l = time_read(dev, off, len);
	op->r_type = R_NS;
	return op+1;
}

static struct operation *do_write_zero(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	op->result.l = time_write(dev, off, len, WBUF_ZERO);
	op->r_type = R_NS;
	return op+1;
}

static struct operation *do_write_one(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	op->result.l = time_write(dev, off, len, WBUF_ONE);
	op->r_type = R_NS;
	return op+1;
}

static struct operation *do_write_rand(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	op->result.l = time_write(dev, off, len, WBUF_RAND);
	op->r_type = R_NS;
	return op+1;
}

static struct operation *do_erase(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	op->result.l = time_erase(dev, off, len);
	op->r_type = R_NS;
	return op+1;
}

static struct operation *length_or_offs(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	op->result.l = (op->code == O_LENGTH) ? (long long)len : off;
	op->r_type = R_BYTE;
	return op+1;
}

static res_t format_value(res_t val, enum resulttype type,
			unsigned int size_x, unsigned int size_y)
{
	long long l = val.l;
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

		return out;

	case R_BYTE:
		if (l < 1024)
			snprintf(out.s, 8, "%0lldB", l);
		else if (l < 1024 * 1024)
			snprintf(out.s, 8, "%0.3gKiB", l / 1024.0);
		else if (l < 1024 * 1024 * 1024)
			snprintf(out.s, 8, "%0.3gMiB", l / (1024.0 * 1024.0));
		else
			snprintf(out.s, 8, "%0.4gGiB", l / (1024.0 * 1024.0 * 1024.0));
		break;

	case R_BPS:
		if (l < 1000)
			snprintf(out.s, 8, "%0lldB/s", l);
		else if (l < 1000 * 1000)
			snprintf(out.s, 8, "%.03gK/s", l / 1000.0);
		else if (l < 1000 * 1000 * 1000)
			snprintf(out.s, 8, "%.03gM/s", l / (1000.0 * 1000.0));
		else
			snprintf(out.s, 8, "%.04gG/s", l / (1000.0 * 1000.0 * 1000.0));
		break;
		

	case R_NS:
		if (l < 1000)
			snprintf(out.s, 8, "%lldns", l);
		else if (l < 1000 * 1000)
			snprintf(out.s, 8, "%.3gµs", l / 1000.0);
		else if (l < 1000 * 1000 * 1000)
			snprintf(out.s, 8, "%.3gms", l / 1000000.0);
		else
			snprintf(out.s, 8, "%.4gs", l / 1000000000.0);
		break;
	default:
		return res_null;
	}

	for (x = strlen(out.s); x<7; x++)
		out.s[x] = ' ';
	out.s[7] = '\0';

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

static struct operation *print_string(struct operation *op, struct device *dev,
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
	case R_BPS:
		printf("%lld ", val.l);
		break;
	case R_STRING:
		printf("%s ", val.s);
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

static struct operation *newline(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	printf("\n");
	return op+1;
}

static res_t bytespersec_one(res_t res, size_t bytes, enum resulttype type,
			unsigned int size_x, unsigned int size_y)
{
	if (type == R_NS)
		res.l = 1000000000ll * bytes / res.l;
	else if (type == R_ARRAY) {
		res_t *array = res_ptr(res);
		type = res_type(res);
		unsigned int x;

		for (x = 0; x < size_x; x++)
			array[x] = bytespersec_one(array[x], bytes,
					type, size_y, 0);

		if (type == R_NS)
			res = to_res(array, R_BPS);
	} else {
		res = res_null;
	}

	return res;
}

static struct operation *bytespersec(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next;
	next = call_propagate(op+1, dev, off, max, len, op);

	op->result = bytespersec_one(op->result, len, op->r_type,
				 op->size_x, op->size_y);

	if (op->result.l == res_null.l)
		return_err("invalid data, type %d\n", op->r_type);

	if (op->r_type == R_NS)
		op->r_type = R_BPS;

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

static struct operation *len_fixed(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	return call_propagate(op+1, dev, off, max, op->val, op);
}

static struct operation *len_pow2(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	unsigned int i;
	struct operation *next = op+1;

	if (!len)
		len = 1;

	if (op->val > 0) {
		for (i = 0; i < op->num && next; i++)
			next = call_aggregate(op+1, dev, off, max,
					 len * op->val << i, op);
	} else {
		for (i = op->num; i>0 && next; i--)
			next = call_aggregate(op+1, dev, off, max,
					 len * (-op->val/2) << i, op);

	}

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
	unsigned int num, val;

	if (op->val == -1) {
		if (len == 0 || max < (off_t)len)
			return_err("cannot fill %lld bytes with %ld byte chunks\n",
					(long long)max, (long)len);

		num = max/len;
		val = max/num;
	} else {
		val = op->val;
		num = op->num;
	}

	for (i = 0; i < num && next; i++)
		next = call_aggregate(op+1, dev, off + i * val, max, len, op);

	return next;
}

/*
 * Linear feedback shift register
 *
 * We use this to randomize the block positions for random-access
 * tests. Unlike real random data, we know that within 2^bits
 * accesses, every possible value up to 2^bits will be seen
 * exactly once, with the exception of zero, for which we have
 * a special treatment.
 */
static int lfsr(unsigned short v, unsigned int bits)
{
	unsigned short bit;

	if (v >= (1 << bits)) {
		fprintf(stderr, "lfsr: internal error\n");
		exit(-1);
	}

	if (v == (((1 << bits) - 1) & 0xace1))
		return 0;

	if (v == 0)
		v = ((1 << bits) - 1) & 0xace1;

	switch (bits) {
	case 8: /* x^8 + x^6 + x^5 + x^4 + 1 */
		bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 4)) & 1;
		break;
	case 9: /* x9 + x5 + 1 */
		bit = ((v >> 0) ^ (v >> 4)) & 1;
		break;
	case 10: /* x10 + x7 + 1 */
		bit = ((v >> 0) ^ (v >> 3)) & 1;
		break;
	case 11: /* x11 + x9 + 1 */
		bit = ((v >> 0) ^ (v >> 2)) & 1;
		break;
	case 12:
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 8)) & 1;
		break;
	case 13: /* x^13 + x^12 + x^11 + x^8 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 5)) & 1;
		break;
	case 14: /* x^14 + x^13 + x^12 + x^2 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 12)) & 1;
		break;
	case 15: /* x^15 + x^14 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ) & 1;
		break;
	case 16: /* x^16 + x^14 + x^13 + x^11 + 1 */
		bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 5) ) & 1;
		break;
	default:
		fprintf(stderr, "lfsr: internal error\n");
		exit(-1);
	}

	return v >> 1 | bit << (bits - 1);
}

static struct operation *off_rand(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next = op+1;
	unsigned int i;
	unsigned int num, val;
	unsigned int pos = 0, bits = 0;

	if (op->val == -1) {
		if (len == 0 || max < (off_t)len)
			return_err("cannot fill %lld bytes with %ld byte chunks\n",
					(long long)max, (long)len);

		num = max/len;
		val = max/num;
	} else {
		val = op->val;
		num = op->num;
	}

	for (i = num; i > 0; i /= 2)
		bits++;

	if (bits < 8)
		bits = 8;

	for (i = 0; i < num && next; i++) {
		do {
			pos = lfsr(pos, bits);
		} while (pos >= num);
		next = call_aggregate(op+1, dev, off + pos * val, max, len, op);
	}

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
		if (res_type(child->result) != R_NS &&
		    res_type(child->result) != R_BPS)
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

		res_ptr(op->result)[i] = do_reduce_int(child->size_y, res_ptr(in[i]),
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

static struct operation *drop(struct operation *op, struct device *dev,
		 off_t off, off_t max, size_t len)
{
	struct operation *next, *child;

	child = op+1;
	next = call(child, dev, off, max, len);
	if (!next)
		return NULL;

	child->result = res_null;
	child->r_type = R_NONE;
	child->size_x = child->size_y = 0;

	return next;
}

static struct syntax syntax[] = {
	{ O_END,	"END",		nop,		},
	{ O_READ,	"READ",		do_read,	},
	{ O_WRITE_ZERO,	"WRITE_ZERO",	do_write_zero,	},
	{ O_WRITE_ONE,	"WRITE_ONE",	do_write_one,	},
	{ O_WRITE_RAND,	"WRITE_RAND",	do_write_rand,	},
	{ O_ERASE,	"ERASE",	do_erase,	},
	{ O_LENGTH,	"LENGTH",	length_or_offs	},
	{ O_OFFSET,	"OFFSET",	length_or_offs,	},

	{ O_PRINT,	"PRINT",	print_string,	P_STRING },
	{ O_PRINTF,	"PRINTF",	print_val,	},
	{ O_FORMAT,	"FORMAT",	format,		},
	{ O_NEWLINE,	"NEWLINE",	newline,	},
	{ O_BPS,	"BPS",		bytespersec,	},

	{ O_SEQUENCE,	"SEQUENCE",	sequence,	P_NUM },
	{ O_REPEAT,	"REPEAT",	repeat,		P_NUM },

	{ O_OFF_FIXED,	"OFF_FIXED",	off_fixed,	P_VAL },
	{ O_OFF_POW2,	"OFF_POW2",	nop,		P_NUM | P_VAL },
	{ O_OFF_LIN,	"OFF_LIN",	off_lin,	P_NUM | P_VAL },
	{ O_OFF_RAND,	"OFF_RAND",	off_rand,	P_NUM | P_VAL },
	{ O_LEN_FIXED,	"LEN_FIXED",	len_fixed,	P_VAL },
	{ O_LEN_POW2,	"LEN_POW2",	len_pow2,	P_NUM | P_VAL },
	{ O_MAX_POW2,	"MAX_POW2",	nop,		P_NUM | P_VAL },
	{ O_MAX_LIN,	"MAX_LIN",	nop,		P_NUM | P_VAL },

	{ O_REDUCE,	"REDUCE",	reduce,		P_AGGREGATE },
	{ O_DROP,	"DROP",		drop,		},
};

