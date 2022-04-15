	.text

	/*
         * save the old thread's registers,
         * restore the new thread's registers.
         */

	.globl thread_switch
thread_switch:
	# a0 is the thread to switch to
	# a1 is last thread

	# point a1 to utrapframe
	# kinda pointness but for the sake of clearity
	addi	a1, a1, 0

	# save callee-saved regs to last trapframe
	sd		sp, 0(a1)
	sd		ra, 8(a1)
	sd		s0, 16(a1)
	sd		s1, 24(a1)
	sd		s2, 32(a1)
	sd		s3, 40(a1)
	sd		s4, 48(a1)
	sd		s5, 56(a1)
	sd		s6, 64(a1)
	sd		s7, 72(a1)
	sd		s8, 80(a1)
	sd		s9, 96(a1)
	sd		s10, 104(a1)
	sd		s11, 112(a1)

	# point a0 to utrapframe
	addi	a0, a0, 0

	# load registers back
	# thread switch happens here
	ld		sp, 0(a0)
	ld		ra, 8(a0)
	ld		s0, 16(a0)
	ld		s1, 24(a0)
	ld		s2, 32(a0)
	ld		s3, 40(a0)
	ld		s4, 48(a0)
	ld		s5, 56(a0)
	ld		s6, 64(a0)
	ld		s7, 72(a0)
	ld		s8, 80(a0)
	ld		s9, 96(a0)
	ld		s10, 104(a0)
	ld		s11, 112(a0)

	ret    /* return to ra */
