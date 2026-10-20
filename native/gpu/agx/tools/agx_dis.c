/*
 * agx_dis: disassemble raw AGX shader code. Output format matches the Mesa
 * reference dump used in docs/AGX_ISA_NOTES.md: "<off>: <hex> <text>".
 * Trailing 0x0008 trap padding is trimmed. Exit status 1 on any decode error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../agx_isa.h"
#include "../agx_text.h"

static int dis(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	static uint8_t data[1 << 20];
	size_t n = fread(data, 1, sizeof(data), f);
	fclose(f);
	while (n > 2 && data[n - 2] == 8 && data[n - 1] == 0)
		n -= 2;
	printf("== %s\n", path);
	size_t off = 0;
	while (off < n) {
		struct agx_ins ins;
		int l = agx_decode(data + off, n - off, &ins);
		if (l <= 0) {
			printf("%4zx: ?? error %d (%s)\n", off, l, l == AGX_E_NO_MATCH ? "no match" : agx_ins_table[ins.op].name);
			return 1;
		}
		char hex[40], text[256];
		for (int i = 0; i < l; i++)
			snprintf(hex + 2 * i, 3, "%02x", data[off + i]);
		if (agx_print(&ins, text, sizeof(text)) < 0) {
			printf("%4zx: ?? print error\n", off);
			return 1;
		}
		printf("%4zx: %-20s %s\n", off, hex, text);
		off += (size_t)l;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 0;
	for (int i = 1; i < argc; i++)
		rc |= dis(argv[i]);
	return rc;
}
