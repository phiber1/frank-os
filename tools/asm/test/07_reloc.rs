.syntax unified
.thumb
main:
	ldr	r0, =tab
	ldr	r1, =tab
	bl	helper
	bx	lr
helper:
	adds	r0, r0, #1
	bx	lr
tab:
	.word	main
	.word	helper
	.word	0xCAFEBABE
	.ltorg
