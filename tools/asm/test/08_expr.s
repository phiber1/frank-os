.syntax unified
.thumb
	.word	1 + 2 * 3
	.word	(1 + 2) * 3
	.word	0x100 | 0x22
	.word	1 << 4 + 1
	.word	8 >> 1 + 1
	.word	7 & 3 + 1
	.word	5 - 2 - 1
	.word	2 * 3 + 4 * 5
	.word	0xFF ^ 0x0F
	.word	100 % 7
