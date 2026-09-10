.syntax unified
.thumb
.thumb_func
start:
	movs	r0, #0
loop:
	adds	r0, #1
	cmp	r0, #10
	bne	loop
	beq	done
	bl	func
	b	start
done:
	nop
func:
	bx	lr
