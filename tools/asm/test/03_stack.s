.syntax unified
.thumb
	push	{r4, r5, r6, r7, lr}
	push	{r0}
	pop	{r4, r5, r6, r7, pc}
	pop	{r3}
	stmia	r0!, {r1, r2, r3}
	ldmia	r4!, {r5, r6, r7}
	add	sp, sp, #16
	sub	sp, sp, #32
	add	r0, sp, #12
