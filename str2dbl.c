/* str2dbl.c
 *
 * Copyright (C) 2025 Yukimasa Morimi
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * ---------------------------------------------------------------------------
 *
 * Locale-independent conversion from string to double.
 * 
 * The strtod function from the C standard library is not suitable
 * because it is locale-dependent.
 *
 * # Convertion Methods
 *
 * Method 1 (fast):
 *
 * If it's an integer with 18 digits or less, parse it as an int64_t
 * and convert it to a double. The compiler should emit the correct
 * conversion code.
 *
 * Method 2 (fast): 
 * 
 * When the following conditions are met:
 *
 *   * Mantissa is 15 digits or less
 *   * The absolute value is less than 1e37 and greater than or equal
 *     to 1e-8
 *     - (when the number of digits is 15, 1e-22 when the number of
 *        digits is 1).
 *   * Double arithmetic conforms to IEEE 745.
 *
 * Parse the mantissa as an integer, convert it to a double (exactly),
 * and multiply or divide it by an exact power of 10.
 * The result is the correct value under "correctly rounding".
 *
 * Method 3 (slow):
 *
 * Using multiple-precision decimal arithmetic. 
 *
 * # Limitations
 * 
 *  * Exponents over 100,000,000 are not supported.
 *  * "nan", "inf", etc. are not supported.
 *  * Hexadecimal represention is not supported.
 *
 */
#if defined(_MSC_VER)
#pragma float_control(precise, on) /* Disable "/fp:fast" */
#endif
#if defined(__FAST_MATH__)
#error NEVER compile with the -ffast-math flag!
#endif

#include "str2dbl.h"

#define STR2DBL_DEBUG 0

#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
#include <stdio.h>
#if defined(_MSC_VER) && _MSC_VER < 1900
#define PRId64 "I64d"
#define PRId32 "d"
#else
#include <inttypes.h>
#endif
#endif

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1900
#include <fenv.h>
#include <stdint.h>
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#define isinf(x) ((_fpclass(x) & (_FPCLASS_NINF|_FPCLASS_PINF)) != 0)
#endif

/* Using the current rounding mode. */
#define STR2DBL_RESPECT_FP_ROUND 1

#ifdef INFINITY
#define STR2DBL_DOUBLE_INFINITY ((double)INFINITY)
#else
#define STR2DBL_DOUBLE_INFINITY (ldexp(1.0, 1024)) /* should be overflow */
#endif

/* It seems that older msvc compilers don't handle the literal "-0.0" correctly. */
#if defined(_MSC_VER) && _MSC_VER < 1400 /* Is this version correct? */
static double STR2DBL_DOUBLE_NEGATIVE_ZERO() { static volatile double ZERO = 0.0; return -ZERO; }
#define STR2DBL_DOUBLE_NEGATIVE_ZERO (STR2DBL_DOUBLE_NEGATIVE_ZERO())
#else
#define STR2DBL_DOUBLE_NEGATIVE_ZERO (-0.0)
#endif

#define STR2DBL_EXPONENT_LIMIT               100000000


#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\f' || (c) == '\n' || (c) == '\r' || (c) == '\t' || (c) == '\v')
#define IS_DIGITS(c) ((c) >= '0' && (c) <= '9')

struct ParseFloatNumber {
	const char* i_ptr; /* pointer to integer part */
	const char* f_ptr; /* pointer to fraction part */
	size_t i_len;      /* length of integer part */
	size_t f_len;      /* length of fraction part */
	int exponent;
	int negative;      /* 0: positive, 1: negative */
};

/* parse float number string
 * 
 * parse  [out]     parse result.
 * str    [in]      string pointer.
 * end    [in, opt] string end pointer.
 * no_exponent  [in]    1: do not take exponent part.
 * 
 * return comsume string length.
 */
static size_t parse_float_number(struct ParseFloatNumber* parse, const char* str, const char* end, int no_exponent)
{
	const char* ptr = str;

	parse->i_ptr = parse->f_ptr = NULL;
	parse->i_len = parse->f_len = 0;
	parse->exponent = 0;
	parse->negative = 0;

	/* Ignore leading whitespaces */
	while (ptr != end && IS_WHITESPACE(*ptr))
		ptr++;

	/* Invalid: An empty or blank string */
	if (ptr == end)
		return 0;

	if (*ptr == '+' || *ptr == '-')
	{
		parse->negative = (*ptr == '-');
		ptr++;
		/* Invalid: Sing only */
		if (ptr == end)
			return 0;
	}

	/* integer part */
	if (IS_DIGITS(*ptr))
	{
		parse->i_ptr = ptr;
		while (ptr != end && IS_DIGITS(*ptr))
		{
			ptr++;
			parse->i_len++;
		}
		/* end of string */
		if (ptr == end)
			return (size_t)(ptr - str);
	}

	/* fracton part */
	if (*ptr == '.')
	{
		ptr++;
		parse->f_ptr = ptr;
		while (ptr != end && IS_DIGITS(*ptr))
		{
			ptr++;
			parse->f_len++;
		}
		/* end of string */
		if (ptr == end)
			return (size_t)(ptr - str);
	}

	/* Invalid: missing mantissa */
	if (parse->i_len == 0 && parse->f_len == 0)
		return 0;

	/* exponent part */
	if (no_exponent == 0 && (*ptr == 'E' || *ptr == 'e'))
	{
		const char* e_ptr = ptr;
		int e_neg = 0;
		int exponent = 0;
		ptr++;
		if (ptr == end)
			return (size_t)(e_ptr - str);
		if (*ptr == '+' || *ptr == '-')
		{
			e_neg = (*ptr == '-');
			ptr++;
			if (ptr == end)
				return (size_t)(e_ptr - str);
		}
		if (!IS_DIGITS(*ptr))
			return (size_t)(e_ptr - str);
		while (ptr != end && IS_DIGITS(*ptr))
		{
			if (exponent < STR2DBL_EXPONENT_LIMIT)
				exponent = exponent * 10 + (*ptr - '0');
			ptr++;
		}
		parse->exponent = e_neg ? -exponent : exponent;
	}

	return (size_t)(ptr - str);
}


/* BigDecimal */

/* Number of digits required for BD_MAX:
 * The conversion to double where the most significant digits are needed
 * seems to be distinguishing between the smallest normalized number (2^-1022)
 * and the largest denormalized number ((1 - 2^-52) * 2^-1022).
 * The midpoint ((1 - 2^-53) * 2^-1022) requires 768 decimal digits to
 * represent exactly in decimal. BigDecimal has 9 decimal digits per digit,
 * so a maximum of 87 digits are required (768 = 85 * 9 + 3).
 * We add an additional safety margin, setting BD_MAX=90.
 */
#define BD_DIG      9
#define BD_MAX      90
#define BD_BASE     1000000000
#define BD_HALF     500000000

struct BigDecimal
{
	uint32_t digits[BD_MAX];
	size_t len;
	int point;
	int truncated; /* Trailing non-zero digits were truncated. */
};

#define BD_DIV_UP(x, y) (((x) + ((y) - 1)) / (y))

static void bd_init(struct BigDecimal* bd,
	const char* i_ptr, size_t i_len,
	const char* f_ptr, size_t f_len,
	int exponent)
{
	size_t scale = 0, offset = 0;
	size_t i;
	uint32_t t;

	bd->len = 0;
	bd->point = 0;
	bd->truncated = 0;

	if (exponent < 0)
	{
		size_t neg_exponent = (size_t)-exponent;
		if (neg_exponent < i_len)
		{
			size_t len = i_len - neg_exponent;
			scale = BD_DIV_UP(len, BD_DIG);
			offset = scale * BD_DIG - len;
		}
		else
		{
			size_t len = neg_exponent - i_len;
			scale = 0;
			offset = len;
		}
	}
	else
	{
		size_t len = i_len + (size_t)exponent;
		scale = BD_DIV_UP(len, BD_DIG);
		offset = scale * BD_DIG - len;
	}

	bd->point = (int)scale - (int)(offset / BD_DIG);
	offset = offset % BD_DIG;

	t = 0;
	for (i = 0; i < i_len; i++)
	{
		t = t * 10 + (uint32_t)(i_ptr[i] - '0');
		offset++;
		if (offset == BD_DIG)
		{
			if (bd->len < BD_MAX)
			{
				bd->digits[bd->len] = t;
				bd->len++;
			}
			else
			{
				/* no more room */
				bd->truncated = 1;
			}
			offset = 0;
			t = 0;
		}
	}
	for (i = 0; i < f_len; i++)
	{
		t = t * 10 + (uint32_t)(f_ptr[i] - '0');
		offset++;
		if (offset == BD_DIG)
		{
			if (bd->len < BD_MAX)
			{
				bd->digits[bd->len] = t;
				bd->len++;
			}
			else
			{
				/* no more room */
				bd->truncated = 1;
			}
			offset = 0;
			t = 0;
		}
	}

	if (offset != 0)
	{
		while (offset < BD_DIG)
		{
			t = t * 10;
			offset++;
		}
		if (bd->len < BD_MAX)
		{
			bd->digits[bd->len] = t;
			bd->len++;
		}
		else
		{
			/* no more room */
			bd->truncated = 1;
		}
	}
}

static int bd_is_lt_1(struct BigDecimal* bd)
{
	return (bd->point <= 0);
}

static int bd_is_ge_2(struct BigDecimal* bd)
{
	return (bd->point > 1) || (bd->point == 1 && (bd->len > 0 && bd->digits[0] > 1));
}

static void bd_mul_2(struct BigDecimal* bd)
{
	if (bd->len == 0)
		return;

	if (bd->digits[0] >= BD_HALF)
	{
		size_t i;
		uint32_t carry = 0;

		if (bd->digits[bd->len - 1] == 0)
		{
			carry = 0;
		}
		else if (bd->digits[bd->len - 1] == BD_HALF)
		{
			carry = 1;
		}
		else if (bd->len < BD_MAX)
		{
			carry = 0;
			bd->digits[bd->len] = 0;
			bd->len++;
		}
		else
		{
			carry = bd->digits[bd->len - 1] >= BD_HALF ? 1 : 0;
			/* no more room */
			bd->truncated = 1;
		}
		bd->point++;

		for (i = bd->len - 1; i > 0; i--)
		{
			if (bd->digits[i - 1] >= BD_HALF)
			{
				bd->digits[i] = (bd->digits[i - 1] - BD_HALF) * 2 + carry;
				carry = 1;
			}
			else
			{
				bd->digits[i] = bd->digits[i - 1] * 2 + carry;
				carry = 0;
			}
		}
		bd->digits[0] = carry;
	}
	else
	{
		size_t i;
		uint32_t carry = 0;
		for (i = bd->len; i > 0; i--)
		{
			if (bd->digits[i - 1] >= BD_HALF)
			{
				bd->digits[i - 1] = (bd->digits[i - 1] - BD_HALF) * 2 + carry;
				carry = 1;
			}
			else
			{
				bd->digits[i - 1] = bd->digits[i - 1] * 2 + carry;
				carry = 0;
			}
		}
		if (bd->digits[bd->len - 1] == 0)
		{
			bd->len--;
		}
	}
}

static void bd_div_2(struct BigDecimal* bd)
{
	if (bd->len == 0)
		return;

	if (bd->digits[0] == 1)
	{
		size_t i;
		uint32_t remainder = 1;
		for (i = 1; i < bd->len; i++)
		{
			if (bd->digits[i] % 2 != 0)
			{
				bd->digits[i - 1] = bd->digits[i] / 2 + (remainder ? BD_HALF : 0);
				remainder = 1;
			}
			else
			{
				bd->digits[i - 1] = bd->digits[i] / 2 + (remainder ? BD_HALF : 0);
				remainder = 0;
			}
		}
		if (remainder != 0)
		{
			bd->digits[bd->len - 1] = BD_HALF;
		}
		else
		{
			bd->digits[bd->len - 1] = 0;
			bd->len--;
		}
		bd->point--;
	}
	else
	{
		size_t i;
		uint32_t remainder = 0;
		for (i = 0; i < bd->len; i++)
		{
			if (bd->digits[i] % 2 != 0)
			{
				bd->digits[i] = bd->digits[i] / 2 + (remainder ? BD_HALF : 0);
				remainder = 1;
			}
			else
			{
				bd->digits[i] = bd->digits[i] / 2 + (remainder ? BD_HALF : 0);
				remainder = 0;
			}
		}
		if (remainder != 0)
		{
			if (bd->len < BD_MAX)
			{
				bd->digits[bd->len] = BD_HALF;
				bd->len++;
			}
			else
			{
				/* no more room */
				bd->truncated = 1;
			}
		}
	}
}

/* 
 * returns
 *   0: frac == .0,
 *   1: .0 < frac < .5,
 *   2: frac == .5,
 *   3: .5 < frac
 */
static int bd_frac_kind(struct BigDecimal* bd)
{
	size_t i;

	if (bd->point < 0)
	{
		if (bd->truncated)
			return 1;

		for (i = 0; i < bd->len; i++)
		{
			if (bd->digits[i] != 0)
			{
				return 1;
			}
		}
		return 0;
	}
	else if ((size_t)bd->point >= bd->len)
	{
		return bd->truncated ? 1 : 0;
	}
	else if (bd->digits[bd->point] == 0)
	{
		if (bd->truncated)
			return 1;

		for (i = (size_t)bd->point + 1; i < bd->len; i++)
		{
			if (bd->digits[i] != 0)
			{
				return 1;
			}
		}
		return 0;
	}
	else if (bd->digits[bd->point] < BD_HALF)
	{
		return 1;
	}
	else if (bd->digits[bd->point] == BD_HALF)
	{
		if (bd->truncated)
			return 3;

		for (i = (size_t)bd->point + 1; i < bd->len; i++)
		{
			if (bd->digits[i] != 0)
			{
				return 3;
			}
		}
		return 2;
	}
	else
	{
		return 3;
	}
}

static uint64_t bd_uint64(struct BigDecimal* bd, int neg_round)
{
	size_t i;
	uint64_t x = 0;
	int round = 0; /* 0: nearest, 1: down, 2, up */
	int frac_kind;

	if (bd->point >= 0)
	{
		for (i = 0; i < (size_t)bd->point && i < bd->len; i++)
		{
			x = x * BD_BASE + (uint64_t)bd->digits[i];
		}
		for (i = bd->len; i < (size_t)bd->point; i++)
		{
			x = x * BD_BASE;
		}
	}

#if defined(STR2DBL_RESPECT_FP_ROUND) && STR2DBL_RESPECT_FP_ROUND
#if defined(_MSC_VER) && _MSC_VER < 1900
	{
		switch (_controlfp(0, 0) & _MCW_RC)
		{
		case _RC_NEAR: round = 0; break;
		case _RC_DOWN: round = neg_round ? 2 : 1; break;
		case _RC_UP:   round = neg_round ? 1 : 2; break;
		case _RC_CHOP: round = 1; break;
		}
	}
#else
	switch (fegetround())
	{
	case FE_TONEAREST:  round = 0; break;
	case FE_DOWNWARD:   round = neg_round ? 2 : 1; break;
	case FE_UPWARD:     round = neg_round ? 1 : 2; break;
	case FE_TOWARDZERO: round = 1; break;
	}
#endif
#endif

	frac_kind = bd_frac_kind(bd);

	if ((round == 0 && frac_kind > 2) || (round == 2 && frac_kind > 0))
	{
		x++;
	}
	else if (round == 0 && frac_kind == 2)
	{
		x = (x & 1) != 0 ? x + 1 : x; /* ties to even */
	}

	return x;
}

static uint32_t bd_uint32(struct BigDecimal* bd, int neg_round)
{
	size_t i;
	uint32_t x = 0;
	int round = 0; /* 0: nearest, 1: down, 2, up */
	int frac_kind;

	if (bd->point >= 0)
	{
		for (i = 0; i < (size_t)bd->point && i < bd->len; i++)
		{
			x = x * BD_BASE + (uint32_t)bd->digits[i];
		}
		for (i = bd->len; i < (size_t)bd->point; i++)
		{
			x = x * BD_BASE;
		}
	}

#if defined(STR2DBL_RESPECT_FP_ROUND) && STR2DBL_RESPECT_FP_ROUND
#if defined(_MSC_VER) && _MSC_VER < 1900
	{
		switch (_controlfp(0, 0) & _MCW_RC)
		{
		case _RC_NEAR: round = 0; break;
		case _RC_DOWN: round = neg_round ? 2 : 1; break;
		case _RC_UP:   round = neg_round ? 1 : 2; break;
		case _RC_CHOP: round = 1; break;
		}
	}
#else
	switch (fegetround())
	{
	case FE_TONEAREST:  round = 0; break;
	case FE_DOWNWARD:   round = neg_round ? 2 : 1; break;
	case FE_UPWARD:     round = neg_round ? 1 : 2; break;
	case FE_TOWARDZERO: round = 1; break;
	}
#endif
#endif

	frac_kind = bd_frac_kind(bd);

	if ((round == 0 && frac_kind > 2) || (round == 2 && frac_kind > 0))
	{
		x++;
	}
	else if (round == 0 && frac_kind == 2)
	{
		x = (x & 1) != 0 ? x + 1 : x; /* ties to even */
	}

	return x;
}

#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG

static void bd_print(struct BigDecimal* bd)
{
	if (bd->point < 0)
	{
		size_t i;
		printf(".");
		for (i = 0; i < (size_t)-bd->point; i++)
		{
			printf("%09u", 0);
		}
		for (i = 0; i < bd->len; i++)
		{
			printf("%09u", bd->digits[i]);
		}
	}
	else if ((size_t)bd->point < bd->len)
	{
		size_t i;
		for (i = 0; i < (size_t)bd->point; i++)
		{
			printf("%09u", bd->digits[i]);
		}
		printf(".");
		for (/**/; i < bd->len; i++)
		{
			printf("%09u", bd->digits[i]);
		}
	}
	else
	{
		size_t i;
		for (i = 0; i < bd->len; i++)
		{
			printf("%09u", bd->digits[i]);
		}
		for (/**/; i < (size_t)bd->point; i++)
		{
			printf("%09u", 0);
		}
		printf(".");
	}
	if (bd->truncated)
		printf("+");
}

#endif

/* end of BigDecimal */

/* str2dbl implementation */

static double str2dbl_core(char* str, char* str_end, char** end_ptr, int no_exponent)
{
	struct ParseFloatNumber parse;
	size_t len;

	len = parse_float_number(&parse, str, str_end, no_exponent);

	/* Invalid float number */
	if (len == 0 || (parse.i_len == 0 && parse.f_len == 0))
	{
		if (end_ptr) *end_ptr = str;
		return 0.0;
	}

	if (end_ptr) *end_ptr = str + len;

	/* Remove leading zeros in integer part */
	while (parse.i_len > 0 && *(parse.i_ptr) == '0')
	{
		parse.i_ptr++;
		parse.i_len--;
	}
	/* Remove trailing zeros in fraction part */
	while (parse.f_len > 0 && *(parse.f_ptr + parse.f_len - 1) == '0')
	{
		parse.f_len--;
	}

	/* Zero */
	if (parse.i_len == 0 && parse.f_len == 0)
	{
		return parse.negative ? STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0;
	}
	/* If the exponent is less than -STR2DBL_EXPONENT_LIMIT, it is treated as an underflow. */
	if (parse.exponent <= -STR2DBL_EXPONENT_LIMIT)
	{
		errno = ERANGE;
		return parse.negative ? STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0;
	}
	/* If the exponent exceeds STR2DBL_EXPONENT_LIMIT, it is treated as an overflow. */
	if (parse.exponent >= STR2DBL_EXPONENT_LIMIT)
	{
		errno = ERANGE;
		return parse.negative ? -STR2DBL_DOUBLE_INFINITY : STR2DBL_DOUBLE_INFINITY;
	}

	/* When there is no fraction part,
	 * remove trailing zeros in integer part and adjust exponent. */
	if (parse.f_len == 0)
	{
		while (*(parse.i_ptr + parse.i_len - 1) == '0' && parse.exponent < STR2DBL_EXPONENT_LIMIT)
		{
			parse.exponent++;
			parse.i_len--;
		}
		/* overflow. */
		if (parse.exponent >= STR2DBL_EXPONENT_LIMIT)
		{
			errno = ERANGE;
			return parse.negative ? -STR2DBL_DOUBLE_INFINITY : STR2DBL_DOUBLE_INFINITY;
		}
	}
	/* When there is no integer part,
	 * remove leading zeros in fraction part and adjust exponent. */
	if (parse.i_len == 0)
	{
		while (*parse.f_ptr == '0' && parse.exponent > -STR2DBL_EXPONENT_LIMIT)
		{
			parse.exponent--;
			parse.f_ptr++;
			parse.f_len--;
		}
		/* underflow. */
		if (parse.exponent <= -STR2DBL_EXPONENT_LIMIT)
		{
			errno = ERANGE;
			return parse.negative ? STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0;
		}
	}

	if (parse.exponent < 0)
	{
		size_t neg_exponent = (size_t)-parse.exponent;
		if (neg_exponent < parse.i_len)
		{
			/* overflow. */
			/* The largest number that converts to a finite double is ~1.7976931348623157e+308 */
			if (parse.i_len - neg_exponent > (308 + 1))
			{
				errno = ERANGE;
				return parse.negative ? -STR2DBL_DOUBLE_INFINITY : STR2DBL_DOUBLE_INFINITY;
			}
		}
		else
		{
			/* underflow. */
			/* The smallest number that converts to a non-zero double is ~2.470328229206232720882843964E-324. */
			if (neg_exponent - parse.i_len > (324 - 1))
			{
				errno = ERANGE;
				return parse.negative ? STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0;
			}
		}
	}
	else
	{
		/* overflow. */
		if (parse.i_len > (308 + 1) || parse.i_len + (size_t)parse.exponent > (308 + 1))
		{
			errno = ERANGE;
			return parse.negative ? -STR2DBL_DOUBLE_INFINITY : STR2DBL_DOUBLE_INFINITY;
		}
	}

	/* Method 1 (fast):
	   When string can parse as int64. */
	if (parse.i_len + parse.f_len <= 18)
	{
		size_t i;
		int room = 18 - (int)(parse.i_len + parse.f_len);
		int exponent = parse.exponent - (int)parse.f_len;
		if (exponent >= 0 && exponent <= room)
		{
			static const int64_t POW10[18] = {
				1,
				10,
				100,
				1000,
				10000,
				100000,
				1000000,
				10000000,
				100000000,
				1000000000,
				10000000000,
				100000000000,
				1000000000000,
				10000000000000,
				100000000000000,
				1000000000000000,
				10000000000000000,
				100000000000000000,
			};
			int64_t w = 0;
			for (i = 0; i < parse.i_len; i++)
				w = w * 10 + (parse.i_ptr[i] - '0');
			for (i = 0; i < parse.f_len; i++)
				w = w * 10 + (parse.f_ptr[i] - '0');
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
			printf("Fast 1: %" PRId64 "E%d\n", w, exponent);
#endif
			w = w * POW10[exponent];
			/* The sign is propagated before converting to double to ensure
			   correct rounding.
			   Here w is not zero, -0.0 is not taken into account. */
			w = parse.negative ? -w : w;
			return (double)w; /* should be correctly rounding */
		}
	}

	/* Method 2 (fast):
	 * When a given floating-point number is represented as
	 *   I * 10^E (|I| < 10^16, |E| <= 22),
	 * |I|  and  10^|E|  can be represented *exactly* in IEEE 754
	 * double-precision floating-point numbers,
	 * so  |I|*10^|E|  or  |I|/10^|E|  will match the absolute value
	 * of the desired value under "correct rounding".
	 *
	 * The idea was taken from Rust's f64::from_str implementation.
	 */
	if (parse.i_len + parse.f_len <= 15)
	{
		int room = 15 - (int)(parse.i_len + parse.f_len);
		int exponent = parse.exponent - (int)parse.f_len;
		if (exponent >= -22 && exponent <= (22 + room))
		{
			static const double POW10[23] = {
				1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
				1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
				1e20, 1e21, 1e22,
			};
			size_t i;
			int64_t w = 0;
			double x;
			for (i = 0; i < parse.i_len; i++)
				w = w * 10 + (parse.i_ptr[i] - '0');
			for (i = 0; i < parse.f_len; i++)
				w = w * 10 + (parse.f_ptr[i] - '0');
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
			printf("Fast 2: %" PRId64 "E%d\n", w, exponent);
#endif
			x = (double)w; /* Accurate because |w| < 10^16 < 2^53 */
			/* The sign is propagated before pow of 10 arithmetic to
			   ensure correct rounding.  */
			x = parse.negative ? -x : x;
			if (exponent < 0)
			{
				x = x / POW10[-exponent]; /* should be correctly rounding */
			}
			else if (exponent <= 22)
			{
				x = x * POW10[exponent]; /* should be correctly rounding */
			}
			else
			{
				/* Accurate because |(x * POW10[room])| < 10^16 < 2^53 */
				x = x * POW10[room];
				/* 9 <= (exponent - room) <= 22 */
				x = x * POW10[exponent - room]; /* should be correctly rounding */
			}
			return x;
		}
	}

	/* Method 3 (slow): 
	   Uses multi-precision decimal arithmetic. */
	{
		struct BigDecimal bd;
		int bin_scale = 0;
		double x;

		bd_init(&bd, parse.i_ptr, parse.i_len, parse.f_ptr, parse.f_len, parse.exponent);

#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
		printf("bd start: "); bd_print(&bd); puts("");
#endif

		if (bd_is_lt_1(&bd))
		{
			while (bin_scale > -1022 && bd_is_lt_1(&bd))
			{
				bd_mul_2(&bd);
				bin_scale--;
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd lt_1 %d: ", bin_scale); bd_print(&bd); printf("\n");
#endif
			}
		}
		else if (bd_is_ge_2(&bd))
		{
			while (bin_scale <= 1023 && bd_is_ge_2(&bd))
			{
				bd_div_2(&bd);
				bin_scale++;
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd ge_2 %d: ", bin_scale); bd_print(&bd); printf("\n");
#endif
			}
		}

		if (bin_scale <= 1023)
		{
			int i;
			int64_t w;
			for (i = 0; i < 52; i++)
			{
				bd_mul_2(&bd);
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd last %d: ", i); bd_print(&bd); printf("\n");
#endif
			}
			/* Some architectures (such as x87 and SSE2) do not have instructions
			   for converting an unsigned integer to a double, so it may be more
			   efficient to cast to a signed integer. */
			w = (int64_t)bd_uint64(&bd, parse.negative ? 1 : 0);
			/* Check overflow/underflow case */
			if (w == 0)
			{
				/* underflow */
				errno = ERANGE;
				x = 0.0;
			}
			else if (w == ((int64_t)1 << 53) && bin_scale == 1023)
			{
				/* overflow */
				errno = ERANGE;
				x = STR2DBL_DOUBLE_INFINITY;
			}
			else
			{
				/* 0 <= w < 2^53 or w == 2^53 if rounded up,
				   both can be exactly represented as a double. */
				x = (double)w;
				x = ldexp(x, bin_scale - 52);
			}
		}
		else
		{
			/* overflow. */
			errno = ERANGE;
			x = STR2DBL_DOUBLE_INFINITY;
		}

		return parse.negative ? -x : x;
	}
}

static double str2dbl_internal(char* str, char* str_end, char** end_ptr, int no_exponent)
{
	/* Changing the x87 fpu precision to 53 bit. */
#if defined(_MSC_VER) && defined(_M_IX86) && (!defined(_M_IX86_FP) || _M_IX86_FP == 0)
	unsigned int oldcword;
	double result;
	oldcword = _controlfp(_PC_53, _MCW_PC);
	result = str2dbl_core(str, str_end, end_ptr, no_exponent);
	_controlfp(oldcword, _MCW_PC);
	return result;
#elif defined(__GNUC__) && defined(__i386__)
	volatile double result;
	unsigned short oldcword, newcword;
	asm("fstcw %0" : "=m"(oldcword));
	newcword = (oldcword & ~0x0300) | 0x0200; /* 53 bits precision */
	asm("fldcw %0" : : "m"(newcword));
	result = str2dbl_core(str, str_end, end_ptr, no_exponent);
	asm("fldcw %0" : : "m"(oldcword));
	return result;
#else
	return str2dbl_core(str, str_end, end_ptr, no_exponent);
#endif
}

double str2dbl(char* str, char** str_end)
{
	return str2dbl_internal(str, NULL, str_end, 0);
}

double strn2dbl(char* str, size_t count, char** str_end)
{
	return str2dbl_internal(str, str + count, str_end, 0);
}

double str2dbl_noexp(char* str, char** str_end)
{
	return str2dbl_internal(str, NULL, str_end, 1);
}

double strn2dbl_noexp(char* str, size_t count, char** str_end)
{
	return str2dbl_internal(str, str + count, str_end, 1);
}

/* end str2dbl implementation */

/* str2flt implementation */

static float str2flt_core(char* str, char* str_end, char** end_ptr, int no_exponent)
{
	struct ParseFloatNumber parse;
	size_t len;

	len = parse_float_number(&parse, str, str_end, no_exponent);

	/* Invalid float number */
	if (len == 0 || (parse.i_len == 0 && parse.f_len == 0))
	{
		if (end_ptr) *end_ptr = str;
		return 0.0f;
	}

	if (end_ptr) *end_ptr = str + len;

	/* Remove leading zeros in integer part */
	while (parse.i_len > 0 && *(parse.i_ptr) == '0')
	{
		parse.i_ptr++;
		parse.i_len--;
	}
	/* Remove trailing zeros in fraction part */
	while (parse.f_len > 0 && *(parse.f_ptr + parse.f_len - 1) == '0')
	{
		parse.f_len--;
	}

	/* Zero */
	if (parse.i_len == 0 && parse.f_len == 0)
	{
		return parse.negative ? (float)STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0f;
	}
	/* If the exponent is less than -STR2DBL_EXPONENT_LIMIT, it is treated as an underflow. */
	if (parse.exponent <= -STR2DBL_EXPONENT_LIMIT)
	{
		errno = ERANGE;
		return parse.negative ? (float)STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0f;
	}
	/* If the exponent exceeds STR2DBL_EXPONENT_LIMIT, it is treated as an overflow. */
	if (parse.exponent >= STR2DBL_EXPONENT_LIMIT)
	{
		errno = ERANGE;
		return parse.negative ? (float)-STR2DBL_DOUBLE_INFINITY : (float)STR2DBL_DOUBLE_INFINITY;
	}

	/* When there is no fraction part,
	 * remove trailing zeros in integer part and adjust exponent. */
	if (parse.f_len == 0)
	{
		while (*(parse.i_ptr + parse.i_len - 1) == '0' && parse.exponent < STR2DBL_EXPONENT_LIMIT)
		{
			parse.exponent++;
			parse.i_len--;
		}
		/* overflow. */
		if (parse.exponent >= STR2DBL_EXPONENT_LIMIT)
		{
			errno = ERANGE;
			return parse.negative ? (float)-STR2DBL_DOUBLE_INFINITY : (float)STR2DBL_DOUBLE_INFINITY;
		}
	}
	/* When there is no integer part,
	 * remove leading zeros in fraction part and adjust exponent. */
	if (parse.i_len == 0)
	{
		while (*parse.f_ptr == '0' && parse.exponent > -STR2DBL_EXPONENT_LIMIT)
		{
			parse.exponent--;
			parse.f_ptr++;
			parse.f_len--;
		}
		/* underflow. */
		if (parse.exponent <= -STR2DBL_EXPONENT_LIMIT)
		{
			errno = ERANGE;
			return parse.negative ? (float)STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0f;
		}
	}

	if (parse.exponent < 0)
	{
		size_t neg_exponent = (size_t)-parse.exponent;
		if (neg_exponent < parse.i_len)
		{
			/* overflow. */
			/* The largest number that converts to a finite float is ~3.4028235677973366e+38 */
			if (parse.i_len - neg_exponent > (38 + 1))
			{
				errno = ERANGE;
				return parse.negative ? (float)-STR2DBL_DOUBLE_INFINITY : (float)STR2DBL_DOUBLE_INFINITY;
			}
		}
		else
		{
			/* underflow. */
			/* The smallest number that converts to a non-zero float is ~7.006492321624085e-46. */
			if (neg_exponent - parse.i_len > (46 - 1))
			{
				errno = ERANGE;
				return parse.negative ? (float)STR2DBL_DOUBLE_NEGATIVE_ZERO : 0.0f;
			}
		}
	}
	else
	{
		/* overflow. */
		if (parse.i_len > (38 + 1) || parse.i_len + (size_t)parse.exponent > (38 + 1))
		{
			errno = ERANGE;
			return parse.negative ? (float)-STR2DBL_DOUBLE_INFINITY : (float)STR2DBL_DOUBLE_INFINITY;
		}
	}

	/* Method 1 (fast):
	   When string can parse as int32. */
	if (parse.i_len + parse.f_len <= 9)
	{
		size_t i;
		int room = 9 - (int)(parse.i_len + parse.f_len);
		int exponent = parse.exponent - (int)parse.f_len;
		if (exponent >= 0 && exponent <= room)
		{
			static const int32_t POW10[9] = {
				1,
				10,
				100,
				1000,
				10000,
				100000,
				1000000,
				10000000,
				100000000,
			};
			int32_t w = 0;
			for (i = 0; i < parse.i_len; i++)
				w = w * 10 + (parse.i_ptr[i] - '0');
			for (i = 0; i < parse.f_len; i++)
				w = w * 10 + (parse.f_ptr[i] - '0');
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
			printf("Fast 1: %" PRId32 "E%d\n", w, exponent);
#endif
			w = w * POW10[exponent];
			/* The sign is propagated before converting to float to ensure
			   correct rounding.
			   Here w is not zero, -0.0 is not taken into account. */
			w = parse.negative ? -w : w;
			return (float)w; /* should be correctly rounding */
		}
	}

	/* Method 2 (fast):
	 * When a given floating-point number is represented as
	 *   I * 10^E (|I| < 10^7, |E| <= 10),
	 * |I|  and  10^|E|  can be represented *exactly* in IEEE 754
	 * single-precision floating-point numbers,
	 * so  |I|*10^|E|  or  |I|/10^|E|  will match the absolute value
	 * of the desired value under "correct rounding".
	 */
	if (parse.i_len + parse.f_len <= 6)
	{
		int room = 6 - (int)(parse.i_len + parse.f_len);
		int exponent = parse.exponent - (int)parse.f_len;
		if (exponent >= -10 && exponent <= (10 + room))
		{
			static const float POW10[11] = {
				1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f,
				1e6f, 1e7f, 1e8f, 1e9f,	1e10,
			};
			size_t i;
			int32_t w = 0;
			float x;
			for (i = 0; i < parse.i_len; i++)
				w = w * 10 + (parse.i_ptr[i] - '0');
			for (i = 0; i < parse.f_len; i++)
				w = w * 10 + (parse.f_ptr[i] - '0');
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
			printf("Fast 2: %" PRId32 "E%d\n", w, exponent);
#endif
			x = (float)w; /* Accurate because |w| < 10^7 < 2^24 */
			/* The sign is propagated before pow of 10 arithmetic to
			   ensure correct rounding.  */
			x = parse.negative ? -x : x;
			if (exponent < 0)
			{
				x = x / POW10[-exponent]; /* should be correctly rounding */
			}
			else if (exponent <= 10)
			{
				x = x * POW10[exponent]; /* should be correctly rounding */
			}
			else
			{
				/* Accurate because |(x * POW10[room])| < 10^7 < 2^24 */
				x = x * POW10[room];
				/* 6 <= (exponent - room) <= 10 */
				x = x * POW10[exponent - room]; /* should be correctly rounding */
			}
			return x;
		}
	}

	/* Method 3 (slow): 
	   Uses multi-precision decimal arithmetic. */
	{
		struct BigDecimal bd;
		int bin_scale = 0;
		float x;

		bd_init(&bd, parse.i_ptr, parse.i_len, parse.f_ptr, parse.f_len, parse.exponent);

#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
		printf("bd start: "); bd_print(&bd); puts("");
#endif

		if (bd_is_lt_1(&bd))
		{
			while (bin_scale > -126 && bd_is_lt_1(&bd))
			{
				bd_mul_2(&bd);
				bin_scale--;
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd lt_1 %d: ", bin_scale); bd_print(&bd); printf("\n");
#endif
			}
		}
		else if (bd_is_ge_2(&bd))
		{
			while (bin_scale <= 127 && bd_is_ge_2(&bd))
			{
				bd_div_2(&bd);
				bin_scale++;
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd ge_2 %d: ", bin_scale); bd_print(&bd); printf("\n");
#endif
			}
		}

		if (bin_scale <= 127)
		{
			int i;
			int32_t w;
			for (i = 0; i < 23; i++)
			{
				bd_mul_2(&bd);
#if defined(STR2DBL_DEBUG) && STR2DBL_DEBUG
				printf("bd last %d: ", i); bd_print(&bd); printf("\n");
#endif
			}
			/* Some architectures (such as x87 and SSE2) do not have instructions
			   for converting an unsigned integer to a float, so it may be more
			   efficient to cast to a signed integer. */
			w = (int32_t)bd_uint32(&bd, parse.negative ? 1 : 0);
			/* Check overflow/underflow case */
			if (w == 0)
			{
				/* underflow */
				errno = ERANGE;
				x = 0.0f;
			}
			else if (w == ((int32_t)1 << 24) && bin_scale == 127)
			{
				/* overflow. */
				errno = ERANGE;
				x = (float)STR2DBL_DOUBLE_INFINITY;
			}
			else
			{
				/* 0 <= w < 2^24 or w == 2^24 if rounded up,
				   both can be exactly represented as a double. */
				x = (float)w;
				x = (float)ldexp(x, bin_scale - 23);
			}
		}
		else
		{
			/* overflow. */
			errno = ERANGE;
			x = (float)STR2DBL_DOUBLE_INFINITY;
		}

		return parse.negative ? -x : x;
	}
}

static float str2flt_internal(char* str, char* str_end, char** end_ptr, int no_exponent)
{
	/* NOTE: No precision change is required,
	 * float * float can be represented exactly in 53/64 bits
	 * so no double-rounding occurs.
	 */
	return str2flt_core(str, str_end, end_ptr, no_exponent);
}

float str2flt(char* str, char** str_end)
{
	return str2flt_internal(str, NULL, str_end, 0);
}

float strn2flt(char* str, size_t count, char** str_end)
{
	return str2flt_internal(str, str + count, str_end, 0);
}

float str2flt_noexp(char* str, char** str_end)
{
	return str2flt_internal(str, NULL, str_end, 1);
}

float strn2flt_noexp(char* str, size_t count, char** str_end)
{
	return str2flt_internal(str, str + count, str_end, 1);
}

/* end str2flt implementation */
