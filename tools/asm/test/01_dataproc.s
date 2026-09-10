.syntax unified
.thumb
	movs	r0, #17
	movs	r7, #255
	adds	r1, r0, #3
	subs	r2, r1, r0
	adds	r3, r0, r2
	subs	r4, r3, r2
	adds	r0, #100
	subs	r7, #12
	lsls	r3, r0, #2
	lsrs	r4, r1, #5
	asrs	r5, r2, #1
	ands	r0, r1
	orrs	r2, r3
	eors	r4, r5
	muls	r6, r7, r6
	adcs	r0, r1
	sbcs	r2, r3
	bics	r4, r5
	mvns	r0, r1
	cmp	r0, r1
	cmp	r7, #200
	tst	r2, r3
	cmn	r4, r5
	mov	r8, r9
	add	r10, r11
	cmp	r12, r1
	lsls	r1, r2
	rors	r3, r4
