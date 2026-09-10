.syntax unified
.thumb
	ldr	r0, =0x12345678
	ldr	r1, =0xDEADBEEF
	ldr	r2, =0x12345678
	bx	lr
	.ltorg
