/* Tests for str2dbl
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "str2dbl.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
typedef unsigned __int64 uint64_t;
typedef unsigned int uint32_t;
#define PRIX64 "I64X"
#define PRIX32 "I32X"
#else
#include <stdint.h>
#include <inttypes.h>
#endif

union f64u64 { double f64; uint64_t u64; };
static uint64_t f64_to_bits(double x) { union f64u64 fu; fu.f64 = x; return fu.u64; }

static uint64_t u64_from_hex(const char* str, size_t len)
{
	size_t i;
	uint64_t x = 0;
	for (i = 0; i < len; i++)
	{
		int c = str[i];
		if ('0' <= c && c <= '9')
			x = (x << 4) | (uint64_t)(c - '0');
		else if ('A' <= c && c <= 'F')
			x = (x << 4) | (uint64_t)(c - 'A' + 10);
		else if ('a' <= c && c <= 'f')
			x = (x << 4) | (uint64_t)(c - 'a' + 10);
		else
			break;
	}
	return x;
}

union f32u32 { float f32; uint32_t u32; };
static uint32_t f32_to_bits(float x) { union f32u32 fu; fu.f32 = x; return fu.u32; }

static uint32_t u32_from_hex(const char* str, size_t len)
{
	size_t i;
	uint32_t x = 0;
	for (i = 0; i < len; i++)
	{
		int c = str[i];
		if ('0' <= c && c <= '9')
			x = (x << 4) | (uint32_t)(c - '0');
		else if ('A' <= c && c <= 'F')
			x = (x << 4) | (uint32_t)(c - 'A' + 10);
		else if ('a' <= c && c <= 'f')
			x = (x << 4) | (uint32_t)(c - 'a' + 10);
		else
			break;
	}
	return x;
}

int main(int argc, char **argv)
{
	char buf[2048];
	int i_arg;

	for (i_arg = 1; i_arg < argc; i_arg++)
	{
		FILE* fp = fopen(argv[i_arg], "r");
		int i_line = 0;
		int pass_count = 0;
		int fail_count = 0;

		if (fp == NULL)
		{
			printf("error: cannot open file %s\n", argv[i_arg]);
			continue;
		}

		while (fgets(buf, 2047, fp) != NULL)
		{
			size_t len;
			uint64_t expect64 = 0, actual64 = 0;
			uint32_t expect32 = 0, actual32 = 0;
			
			len = strlen(buf);
			if (len > 0 && buf[len - 1] == '\n')
			{
				len--;
				buf[len] = '\0';
			}
			if (len <= 26)
				continue;

			i_line++;

			expect64 = u64_from_hex(&buf[0], 16);
			actual64 = f64_to_bits(str2dbl(&buf[26], NULL));

			if (expect64 != actual64)
			{
				printf("%d: %s\n  Actual: %016" PRIX64 "\n  Expect: %016" PRIX64 "\n", i_line, &buf[26], actual64, expect64);
				fail_count++;
			}
			else
			{
				pass_count++;
			}

			expect32 = u32_from_hex(&buf[17], 8);
			actual32 = f32_to_bits(str2flt(&buf[26], NULL));

			if (expect32 != actual32)
			{
				printf("%d: %s\n  Actual: %08" PRIX32 "\n  Expect: %08" PRIX32 "\n", i_line, &buf[26], actual32, expect32);
				fail_count++;
			}
			else
			{
				pass_count++;
			}
		}

		printf("FILE: %s\n  PASS: %d, FAIL: %d\n", argv[i_arg], pass_count, fail_count);

		fclose(fp);
	}

	return 0;
}
