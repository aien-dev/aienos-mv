/*
 * agx_as: assemble AGX text, one instruction per line, to raw bytes on
 * stdout (or hex with -x). Lines may carry the agx_dis prefix
 * "<off>: <hex> "; with -c the hex is compared against the assembled bytes
 * (round-trip check). Lines starting with "==" or "#" are skipped.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../agx_isa.h"
#include "../agx_text.h"

int main(int argc, char **argv)
{
	int hex = 0, check = 0, bad = 0, n = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x"))
			hex = 1;
		else if (!strcmp(argv[i], "-c"))
			check = 1;
	}
	char line[512];
	while (fgets(line, sizeof(line), stdin)) {
		char *p = line, want[64] = "";
		while (isspace((unsigned char)*p))
			p++;
		if (!*p || *p == '#' || (p[0] == '=' && p[1] == '='))
			continue;
		char *colon = strstr(p, ": ");
		if (colon && colon - p <= 8) {
			p = colon + 2;
			size_t k = 0;
			while (isxdigit((unsigned char)*p) && k + 1 < sizeof(want))
				want[k++] = *p++;
			want[k] = 0;
			while (*p == ' ')
				p++;
		}
		p[strcspn(p, "\n")] = 0;
		uint8_t out[16];
		int l = agx_assemble(p, out, sizeof(out));
		n++;
		if (l <= 0) {
			fprintf(stderr, "error %d: %s\n", l, p);
			bad++;
			continue;
		}
		char got[40];
		for (int i = 0; i < l; i++)
			snprintf(got + 2 * i, 3, "%02x", out[i]);
		if (check && strcmp(got, want) != 0) {
			fprintf(stderr, "mismatch: %s\n  want %s\n  got  %s\n", p, want, got);
			bad++;
		}
		if (hex)
			printf("%s\n", got);
		else if (!check)
			fwrite(out, 1, (size_t)l, stdout);
	}
	if (check)
		fprintf(stderr, "agx_as: %d lines, %d failed\n", n, bad);
	return bad != 0;
}
