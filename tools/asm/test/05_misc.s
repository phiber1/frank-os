.syntax unified
.thumb
	sxtb	r0, r1
	sxth	r2, r3
	uxtb	r4, r5
	uxth	r6, r7
	rev	r0, r1
	rev16	r2, r3
	revsh	r4, r5
	nop
	svc	#7
