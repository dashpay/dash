.text	

.globl	ctx_inverse_mod_384

.def	ctx_inverse_mod_384;	.scl 2;	.type 32;	.endef
.p2align	5
ctx_inverse_mod_384:
	.byte	0xf3,0x0f,0x1e,0xfa
	movq	%rdi,8(%rsp)
	movq	%rsi,16(%rsp)
	movq	%rsp,%r11
.LSEH_begin_ctx_inverse_mod_384:


	movq	%rcx,%rdi
	movq	%rdx,%rsi
	movq	%r8,%rdx
	movq	%r9,%rcx
ct_inverse_mod_384$1:
	pushq	%rbp

	pushq	%rbx

	pushq	%r12

	pushq	%r13

	pushq	%r14

	pushq	%r15

	subq	$1112,%rsp

.LSEH_body_ctx_inverse_mod_384:


	leaq	88+511(%rsp),%rax
	andq	$-512,%rax
	movq	%rdi,32(%rsp)
	movq	%rcx,40(%rsp)

#ifdef	__SGX_LVI_HARDENING__
	lfence
#endif
	movq	0(%rsi),%r8
	movq	8(%rsi),%r9
	movq	16(%rsi),%r10
	movq	24(%rsi),%r11
	movq	32(%rsi),%r12
	movq	40(%rsi),%r13

	movq	0(%rdx),%r14
	movq	8(%rdx),%r15
	movq	16(%rdx),%rbx
	movq	24(%rdx),%rbp
	movq	32(%rdx),%rsi
	movq	40(%rdx),%rdi

	movq	%r8,0(%rax)
	movq	%r9,8(%rax)
	movq	%r10,16(%rax)
	movq	%r11,24(%rax)
	movq	%r12,32(%rax)
	movq	%r13,40(%rax)

	movq	%r14,48(%rax)
	movq	%r15,56(%rax)
	movq	%rbx,64(%rax)
	movq	%rbp,72(%rax)
	movq	%rsi,80(%rax)
	movq	%rax,%rsi
	movq	%rdi,88(%rax)


	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31


	movq	%rdx,96(%rdi)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31


	movq	%rdx,104(%rdi)


	xorq	$256,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31



	movq	96(%rsi),%rax
	movq	152(%rsi),%r11
	movq	%rdx,%rbx
	movq	%rax,%r10
	imulq	56(%rsp)
	movq	%rax,%r8
	movq	%r11,%rax
	movq	%rdx,%r9
	imulq	64(%rsp)
	addq	%rax,%r8
	adcq	%rdx,%r9
	movq	%r8,48(%rdi)
	movq	%r9,56(%rdi)
	sarq	$63,%r9
	movq	%r9,64(%rdi)
	movq	%r9,72(%rdi)
	movq	%r9,80(%rdi)
	movq	%r9,88(%rdi)
	movq	%r9,96(%rdi)
	leaq	96(%rsi),%rsi

	movq	%r10,%rax
	imulq	%rbx
	movq	%rax,%r8
	movq	%r11,%rax
	movq	%rdx,%r9
	imulq	%rcx
	addq	%rax,%r8
	adcq	%rdx,%r9
	movq	%r8,104(%rdi)
	movq	%r9,112(%rdi)
	sarq	$63,%r9
	movq	%r9,120(%rdi)
	movq	%r9,128(%rdi)
	movq	%r9,136(%rdi)
	movq	%r9,144(%rdi)
	movq	%r9,152(%rdi)
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_384x63
	movq	%r14,56(%rdi)
	movq	%r14,64(%rdi)
	movq	%r14,72(%rdi)
	movq	%r14,80(%rdi)
	movq	%r14,88(%rdi)
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_384_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63
	xorq	$256+96,%rsi
	movl	$31,%edi
	call	__ab_approximation_31


	movq	%r12,72(%rsp)
	movq	%r13,80(%rsp)

	movq	$256,%rdi
	xorq	%rsi,%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,56(%rsp)
	movq	%rcx,64(%rsp)

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	48(%rdi),%rdi
	call	__smulx_191_n_shift_by_31
	movq	%rdx,72(%rsp)
	movq	%rcx,80(%rsp)

	movq	56(%rsp),%rdx
	movq	64(%rsp),%rcx
	leaq	96(%rsi),%rsi
	leaq	48(%rdi),%rdi
	call	__smulx_384x63

	movq	72(%rsp),%rdx
	movq	80(%rsp),%rcx
	leaq	56(%rdi),%rdi
	call	__smulx_768x63

	xorq	$256+96,%rsi
	movl	$55,%edi

	movq	0(%rsi),%r8

	movq	48(%rsi),%r10

	call	__tail_loop_55







	leaq	96(%rsi),%rsi





	movq	%r12,%rdx
	movq	%r13,%rcx
	movq	32(%rsp),%rdi
	call	__smulx_768x63

	movq	40(%rsp),%rsi
	movq	%rdx,%r13
	sarq	$63,%r13

	movq	%r13,%r8
	movq	%r13,%r9
	movq	%r13,%r10
#ifdef	__SGX_LVI_HARDENING__
	lfence
#endif
	andq	0(%rsi),%r8
	andq	8(%rsi),%r9
	movq	%r13,%r11
	andq	16(%rsi),%r10
	andq	24(%rsi),%r11
	movq	%r13,%r12
	andq	32(%rsi),%r12
	andq	40(%rsi),%r13

	addq	%r8,%r14
	adcq	%r9,%r15
	adcq	%r10,%rbx
	adcq	%r11,%rbp
	adcq	%r12,%rcx
	adcq	%r13,%rax
	adcq	$0,%rdx

	movq	%rdx,%r13
	negq	%rdx
	orq	%rdx,%r13
	sarq	$63,%rdx

	movq	%r13,%r8
	movq	%r13,%r9
	movq	%r13,%r10
	andq	0(%rsi),%r8
	andq	8(%rsi),%r9
	movq	%r13,%r11
	andq	16(%rsi),%r10
	andq	24(%rsi),%r11
	movq	%r13,%r12
	andq	32(%rsi),%r12
	andq	40(%rsi),%r13

	xorq	%rdx,%r8
	xorq	%rsi,%rsi
	xorq	%rdx,%r9
	subq	%rdx,%rsi
	xorq	%rdx,%r10
	xorq	%rdx,%r11
	xorq	%rdx,%r12
	xorq	%rdx,%r13
	addq	%rsi,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13

	addq	%r8,%r14
	adcq	%r9,%r15
	adcq	%r10,%rbx
	adcq	%r11,%rbp
	adcq	%r12,%rcx
	adcq	%r13,%rax

	movq	%r14,48(%rdi)
	movq	%r15,56(%rdi)
	movq	%rbx,64(%rdi)
	movq	%rbp,72(%rdi)
	movq	%rcx,80(%rdi)
	movq	%rax,88(%rdi)

	leaq	1112(%rsp),%r8
	movq	0(%r8),%r15

	movq	8(%r8),%r14

	movq	16(%r8),%r13

	movq	24(%r8),%r12

	movq	32(%r8),%rbx

	movq	40(%r8),%rbp

	leaq	48(%r8),%rsp

.LSEH_epilogue_ctx_inverse_mod_384:
	mov	8(%rsp),%rdi
	mov	16(%rsp),%rsi

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%rdx
	lfence
	jmpq	*%rdx
	ud2
#else
	.byte	0xf3,0xc3
#endif

.LSEH_end_ctx_inverse_mod_384:
.def	__smulx_768x63;	.scl 3;	.type 32;	.endef
.p2align	5
__smulx_768x63:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	0(%rsi),%r8
	movq	8(%rsi),%r9
	movq	16(%rsi),%r10
	movq	24(%rsi),%r11
	movq	32(%rsi),%r12
	movq	40(%rsi),%r13
	movq	48(%rsi),%r14

	movq	%rdx,%rax
	sarq	$63,%rax
	xorq	%rbp,%rbp
	subq	%rax,%rbp

	movq	%rdi,8(%rsp)
	movq	%rsi,16(%rsp)
	leaq	56(%rsi),%rsi

	xorq	%rax,%rdx
	addq	%rbp,%rdx

	xorq	%rax,%r8
	xorq	%rax,%r9
	xorq	%rax,%r10
	xorq	%rax,%r11
	xorq	%rax,%r12
	xorq	%rax,%r13
	xorq	%rax,%r14
	addq	%rbp,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13
	adcq	$0,%r14

	andq	%rdx,%r14
	negq	%r14

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%rax
	addq	%rbp,%r9
	mulxq	%r10,%r10,%rbp
	adcq	%rax,%r10
	mulxq	%r11,%r11,%rax
	adcq	%rbp,%r11
	mulxq	%r12,%r12,%rbp
	adcq	%rax,%r12
	mulxq	%r13,%r13,%rax
	adcq	%rbp,%r13
	adcq	%rax,%r14

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,40(%rdi)
	movq	%r14,48(%rdi)
	sarq	$63,%r14
	movq	%r14,56(%rdi)
	movq	%rcx,%rdx
	movq	%rcx,%rax

	movq	0(%rsi),%r8
	movq	8(%rsi),%r9
	movq	16(%rsi),%r10
	movq	24(%rsi),%r11
	movq	32(%rsi),%r12
	movq	40(%rsi),%r13
	movq	48(%rsi),%r14
	movq	56(%rsi),%r15
	movq	64(%rsi),%rbx
	movq	72(%rsi),%rbp
	movq	80(%rsi),%rcx
	movq	88(%rsi),%rdi

	sarq	$63,%rax
	xorq	%rsi,%rsi
	subq	%rax,%rsi

	xorq	%rax,%rdx
	addq	%rsi,%rdx

	xorq	%rax,%r8
	xorq	%rax,%r9
	xorq	%rax,%r10
	xorq	%rax,%r11
	xorq	%rax,%r12
	xorq	%rax,%r13
	xorq	%rax,%r14
	xorq	%rax,%r15
	xorq	%rax,%rbx
	xorq	%rax,%rbp
	xorq	%rax,%rcx
	xorq	%rdi,%rax
	addq	%rsi,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13
	adcq	$0,%r14
	adcq	$0,%r15
	adcq	$0,%rbx
	adcq	$0,%rbp
	adcq	$0,%rcx
	adcq	$0,%rax

	mulxq	%r8,%r8,%rsi
	mulxq	%r9,%r9,%rdi
	addq	%rsi,%r9
	mulxq	%r10,%r10,%rsi
	adcq	%rdi,%r10
	mulxq	%r11,%r11,%rdi
	adcq	%rsi,%r11
	mulxq	%r12,%r12,%rsi
	adcq	%rdi,%r12
	mulxq	%r13,%r13,%rdi
	adcq	%rsi,%r13
	mulxq	%r14,%r14,%rsi
	adcq	%rdi,%r14
	mulxq	%r15,%r15,%rdi
	adcq	%rsi,%r15
	mulxq	%rbx,%rbx,%rsi
	adcq	%rdi,%rbx
	mulxq	%rbp,%rbp,%rdi
	adcq	%rsi,%rbp
	mulxq	%rcx,%rcx,%rsi
	adcq	%rdi,%rcx
	movq	8(%rsp),%rdi
	adcq	$0,%rsi
	imulq	%rdx
	addq	%rsi,%rax
	adcq	$0,%rdx

	addq	0(%rdi),%r8
	adcq	8(%rdi),%r9
	adcq	16(%rdi),%r10
	adcq	24(%rdi),%r11
	adcq	32(%rdi),%r12
	adcq	40(%rdi),%r13
	adcq	48(%rdi),%r14
	movq	56(%rdi),%rsi
	adcq	%rsi,%r15
	adcq	%rsi,%rbx
	adcq	%rsi,%rbp
	adcq	%rsi,%rcx
	adcq	%rsi,%rax
	adcq	%rsi,%rdx

	movq	16(%rsp),%rsi

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,40(%rdi)
	movq	%r14,48(%rdi)
	movq	%r15,56(%rdi)
	movq	%rbx,64(%rdi)
	movq	%rbp,72(%rdi)
	movq	%rcx,80(%rdi)
	movq	%rax,88(%rdi)

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif

.def	__smulx_384x63;	.scl 3;	.type 32;	.endef
.p2align	5
__smulx_384x63:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	0+0(%rsi),%r8
	movq	0+8(%rsi),%r9
	movq	0+16(%rsi),%r10
	movq	0+24(%rsi),%r11
	movq	0+32(%rsi),%r12
	movq	0+40(%rsi),%r13
	movq	0+48(%rsi),%r14

	movq	%rdx,%rbp
	sarq	$63,%rbp
	xorq	%rax,%rax
	subq	%rbp,%rax

	xorq	%rbp,%rdx
	addq	%rax,%rdx

	xorq	%rbp,%r8
	xorq	%rbp,%r9
	xorq	%rbp,%r10
	xorq	%rbp,%r11
	xorq	%rbp,%r12
	xorq	%rbp,%r13
	xorq	%rbp,%r14
	addq	%rax,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13
	adcq	$0,%r14

	andq	%rdx,%r14
	negq	%r14

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%rax
	addq	%rbp,%r9
	mulxq	%r10,%r10,%rbp
	adcq	%rax,%r10
	mulxq	%r11,%r11,%rax
	adcq	%rbp,%r11
	mulxq	%r12,%r12,%rbp
	adcq	%rax,%r12
	mulxq	%r13,%r13,%rax
	movq	%rcx,%rdx
	adcq	%rbp,%r13
	adcq	%rax,%r14

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,%r15
	movq	%r14,%rbx
	movq	56+0(%rsi),%r8
	movq	56+8(%rsi),%r9
	movq	56+16(%rsi),%r10
	movq	56+24(%rsi),%r11
	movq	56+32(%rsi),%r12
	movq	56+40(%rsi),%r13
	movq	56+48(%rsi),%r14

	movq	%rdx,%rbp
	sarq	$63,%rbp
	xorq	%rax,%rax
	subq	%rbp,%rax

	xorq	%rbp,%rdx
	addq	%rax,%rdx

	xorq	%rbp,%r8
	xorq	%rbp,%r9
	xorq	%rbp,%r10
	xorq	%rbp,%r11
	xorq	%rbp,%r12
	xorq	%rbp,%r13
	xorq	%rbp,%r14
	addq	%rax,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13
	adcq	$0,%r14

	andq	%rdx,%r14
	negq	%r14

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%rax
	addq	%rbp,%r9
	mulxq	%r10,%r10,%rbp
	adcq	%rax,%r10
	mulxq	%r11,%r11,%rax
	adcq	%rbp,%r11
	mulxq	%r12,%r12,%rbp
	adcq	%rax,%r12
	mulxq	%r13,%r13,%rax
	adcq	%rbp,%r13
	adcq	%rax,%r14

	addq	0(%rdi),%r8
	adcq	8(%rdi),%r9
	adcq	16(%rdi),%r10
	adcq	24(%rdi),%r11
	adcq	32(%rdi),%r12
	adcq	%r15,%r13
	adcq	%rbx,%r14

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,40(%rdi)
	movq	%r14,48(%rdi)

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif

.def	__smulx_384_n_shift_by_31;	.scl 3;	.type 32;	.endef
.p2align	5
__smulx_384_n_shift_by_31:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	%rdx,%rbx
	movq	0+0(%rsi),%r8
	movq	0+8(%rsi),%r9
	movq	0+16(%rsi),%r10
	movq	0+24(%rsi),%r11
	movq	0+32(%rsi),%r12
	movq	0+40(%rsi),%r13

	movq	%rdx,%rax
	sarq	$63,%rax
	xorq	%rbp,%rbp
	subq	%rax,%rbp

	xorq	%rax,%rdx
	addq	%rbp,%rdx

	xorq	%rax,%r8
	xorq	%rax,%r9
	xorq	%rax,%r10
	xorq	%rax,%r11
	xorq	%rax,%r12
	xorq	%rax,%r13
	addq	%rbp,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13

	andq	%rdx,%rax
	negq	%rax

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%r14
	addq	%rbp,%r9
	mulxq	%r10,%r10,%rbp
	adcq	%r14,%r10
	mulxq	%r11,%r11,%r14
	adcq	%rbp,%r11
	mulxq	%r12,%r12,%rbp
	adcq	%r14,%r12
	mulxq	%r13,%r13,%r14
	adcq	%rbp,%r13
	adcq	%rax,%r14

	movq	%rcx,%rdx

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,40(%rdi)
	movq	%r14,%r15
	movq	48+0(%rsi),%r8
	movq	48+8(%rsi),%r9
	movq	48+16(%rsi),%r10
	movq	48+24(%rsi),%r11
	movq	48+32(%rsi),%r12
	movq	48+40(%rsi),%r13

	movq	%rdx,%rax
	sarq	$63,%rax
	xorq	%rbp,%rbp
	subq	%rax,%rbp

	xorq	%rax,%rdx
	addq	%rbp,%rdx

	xorq	%rax,%r8
	xorq	%rax,%r9
	xorq	%rax,%r10
	xorq	%rax,%r11
	xorq	%rax,%r12
	xorq	%rax,%r13
	addq	%rbp,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13

	andq	%rdx,%rax
	negq	%rax

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%r14
	addq	%rbp,%r9
	mulxq	%r10,%r10,%rbp
	adcq	%r14,%r10
	mulxq	%r11,%r11,%r14
	adcq	%rbp,%r11
	mulxq	%r12,%r12,%rbp
	adcq	%r14,%r12
	mulxq	%r13,%r13,%r14
	adcq	%rbp,%r13
	adcq	%rax,%r14

	addq	0(%rdi),%r8
	adcq	8(%rdi),%r9
	adcq	16(%rdi),%r10
	adcq	24(%rdi),%r11
	adcq	32(%rdi),%r12
	adcq	40(%rdi),%r13
	adcq	%r15,%r14
	movq	%rbx,%rdx

	shrdq	$31,%r9,%r8
	shrdq	$31,%r10,%r9
	shrdq	$31,%r11,%r10
	shrdq	$31,%r12,%r11
	shrdq	$31,%r13,%r12
	shrdq	$31,%r14,%r13

	sarq	$63,%r14
	xorq	%rbp,%rbp
	subq	%r14,%rbp

	xorq	%r14,%r8
	xorq	%r14,%r9
	xorq	%r14,%r10
	xorq	%r14,%r11
	xorq	%r14,%r12
	xorq	%r14,%r13
	addq	%rbp,%r8
	adcq	$0,%r9
	adcq	$0,%r10
	adcq	$0,%r11
	adcq	$0,%r12
	adcq	$0,%r13

	movq	%r8,0(%rdi)
	movq	%r9,8(%rdi)
	movq	%r10,16(%rdi)
	movq	%r11,24(%rdi)
	movq	%r12,32(%rdi)
	movq	%r13,40(%rdi)

	xorq	%r14,%rdx
	xorq	%r14,%rcx
	addq	%rbp,%rdx
	addq	%rbp,%rcx

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif

.def	__smulx_191_n_shift_by_31;	.scl 3;	.type 32;	.endef
.p2align	5
__smulx_191_n_shift_by_31:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	%rdx,%rbx
	movq	0+0(%rsi),%r8
	movq	0+8(%rsi),%r9
	movq	0+16(%rsi),%r10

	movq	%rdx,%rax
	sarq	$63,%rax
	xorq	%rbp,%rbp
	subq	%rax,%rbp

	xorq	%rax,%rdx
	addq	%rbp,%rdx

	xorq	%rax,%r8
	xorq	%rax,%r9
	xorq	%r10,%rax
	addq	%rbp,%r8
	adcq	$0,%r9
	adcq	$0,%rax

	mulxq	%r8,%r8,%rbp
	mulxq	%r9,%r9,%r10
	addq	%rbp,%r9
	adcq	$0,%r10
	imulq	%rdx
	addq	%rax,%r10
	adcq	$0,%rdx
	movq	%rdx,%r14
	movq	%rcx,%rdx
	movq	48+0(%rsi),%r11
	movq	48+8(%rsi),%r12
	movq	48+16(%rsi),%r13

	movq	%rdx,%rax
	sarq	$63,%rax
	xorq	%rbp,%rbp
	subq	%rax,%rbp

	xorq	%rax,%rdx
	addq	%rbp,%rdx

	xorq	%rax,%r11
	xorq	%rax,%r12
	xorq	%r13,%rax
	addq	%rbp,%r11
	adcq	$0,%r12
	adcq	$0,%rax

	mulxq	%r11,%r11,%rbp
	mulxq	%r12,%r12,%r13
	addq	%rbp,%r12
	adcq	$0,%r13
	imulq	%rdx
	addq	%rax,%r13
	adcq	$0,%rdx
	addq	%r8,%r11
	adcq	%r9,%r12
	adcq	%r10,%r13
	adcq	%rdx,%r14
	movq	%rbx,%rdx

	shrdq	$31,%r12,%r11
	shrdq	$31,%r13,%r12
	shrdq	$31,%r14,%r13

	sarq	$63,%r14
	xorq	%rbp,%rbp
	subq	%r14,%rbp

	xorq	%r14,%r11
	xorq	%r14,%r12
	xorq	%r14,%r13
	addq	%rbp,%r11
	adcq	$0,%r12
	adcq	$0,%r13

	movq	%r11,0(%rdi)
	movq	%r12,8(%rdi)
	movq	%r13,16(%rdi)

	xorq	%r14,%rdx
	xorq	%r14,%rcx
	addq	%rbp,%rdx
	addq	%rbp,%rcx

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif

.def	__ab_approximation_31;	.scl 3;	.type 32;	.endef
.p2align	5
__ab_approximation_31:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	40(%rsi),%r9
	movq	88(%rsi),%r11
	movq	32(%rsi),%rbx
	movq	80(%rsi),%rbp
	movq	24(%rsi),%r8
	movq	72(%rsi),%r10

	movq	%r9,%rax
	orq	%r11,%rax
	cmovzq	%rbx,%r9
	cmovzq	%rbp,%r11
	cmovzq	%r8,%rbx
	movq	16(%rsi),%r8
	cmovzq	%r10,%rbp
	movq	64(%rsi),%r10

	movq	%r9,%rax
	orq	%r11,%rax
	cmovzq	%rbx,%r9
	cmovzq	%rbp,%r11
	cmovzq	%r8,%rbx
	movq	8(%rsi),%r8
	cmovzq	%r10,%rbp
	movq	56(%rsi),%r10

	movq	%r9,%rax
	orq	%r11,%rax
	cmovzq	%rbx,%r9
	cmovzq	%rbp,%r11
	cmovzq	%r8,%rbx
	movq	0(%rsi),%r8
	cmovzq	%r10,%rbp
	movq	48(%rsi),%r10

	movq	%r9,%rax
	orq	%r11,%rax
	cmovzq	%rbx,%r9
	cmovzq	%rbp,%r11
	cmovzq	%r8,%rbx
	cmovzq	%r10,%rbp

	movq	%r9,%rax
	orq	%r11,%rax
	bsrq	%rax,%rcx
	leaq	1(%rcx),%rcx
	cmovzq	%r8,%r9
	cmovzq	%r10,%r11
	cmovzq	%rax,%rcx
	negq	%rcx


	shldq	%cl,%rbx,%r9
	shldq	%cl,%rbp,%r11

	movl	$0x7FFFFFFF,%eax
	andq	%rax,%r8
	andq	%rax,%r10
	andnq	%r9,%rax,%r9
	andnq	%r11,%rax,%r11
	orq	%r9,%r8
	orq	%r11,%r10

	jmp	__inner_loop_31

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%rdx
	lfence
	jmpq	*%rdx
	ud2
#else
	.byte	0xf3,0xc3
#endif

.def	__inner_loop_31;	.scl 3;	.type 32;	.endef
.p2align	5
__inner_loop_31:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	$0x7FFFFFFF80000000,%rcx
	movq	$0x800000007FFFFFFF,%r13
	movq	$0x7FFFFFFF7FFFFFFF,%r15

.Loop_31:
	cmpq	%r10,%r8
	movq	%r8,%rax
	movq	%r10,%rbx
	movq	%rcx,%rbp
	movq	%r13,%r14
	cmovbq	%r10,%r8
	cmovbq	%rax,%r10
	cmovbq	%r13,%rcx
	cmovbq	%rbp,%r13

	subq	%r10,%r8
	subq	%r13,%rcx
	addq	%r15,%rcx

	testq	$1,%rax
	cmovzq	%rax,%r8
	cmovzq	%rbx,%r10
	cmovzq	%rbp,%rcx
	cmovzq	%r14,%r13

	shrq	$1,%r8
	addq	%r13,%r13
	subq	%r15,%r13
	subl	$1,%edi
	jnz	.Loop_31

	shrq	$32,%r15
	movl	%ecx,%edx
	movl	%r13d,%r12d
	shrq	$32,%rcx
	shrq	$32,%r13
	subq	%r15,%rdx
	subq	%r15,%rcx
	subq	%r15,%r12
	subq	%r15,%r13

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif


.def	__tail_loop_55;	.scl 3;	.type 32;	.endef
.p2align	5
__tail_loop_55:
	.byte	0xf3,0x0f,0x1e,0xfa

	movq	$1,%rdx
	xorq	%rcx,%rcx
	xorq	%r12,%r12
	movq	$1,%r13

.Loop_55:
	xorq	%rax,%rax
	testq	$1,%r8
	movq	%r10,%rbx
	cmovnzq	%r10,%rax
	subq	%r8,%rbx
	movq	%r8,%rbp
	subq	%rax,%r8
	cmovcq	%rbx,%r8
	cmovcq	%rbp,%r10
	movq	%rdx,%rax
	cmovcq	%r12,%rdx
	cmovcq	%rax,%r12
	movq	%rcx,%rbx
	cmovcq	%r13,%rcx
	cmovcq	%rbx,%r13
	xorq	%rax,%rax
	xorq	%rbx,%rbx
	shrq	$1,%r8
	testq	$1,%rbp
	cmovnzq	%r12,%rax
	cmovnzq	%r13,%rbx
	addq	%r12,%r12
	addq	%r13,%r13
	subq	%rax,%rdx
	subq	%rbx,%rcx
	subl	$1,%edi
	jnz	.Loop_55

	
#ifdef	__SGX_LVI_HARDENING__
	popq	%r8
	lfence
	jmpq	*%r8
	ud2
#else
	.byte	0xf3,0xc3
#endif

.section	.pdata
.p2align	2
.rva	.LSEH_begin_ctx_inverse_mod_384
.rva	.LSEH_body_ctx_inverse_mod_384
.rva	.LSEH_info_ctx_inverse_mod_384_prologue

.rva	.LSEH_body_ctx_inverse_mod_384
.rva	.LSEH_epilogue_ctx_inverse_mod_384
.rva	.LSEH_info_ctx_inverse_mod_384_body

.rva	.LSEH_epilogue_ctx_inverse_mod_384
.rva	.LSEH_end_ctx_inverse_mod_384
.rva	.LSEH_info_ctx_inverse_mod_384_epilogue

.section	.xdata
.p2align	3
.LSEH_info_ctx_inverse_mod_384_prologue:
.byte	1,0,5,0x0b
.byte	0,0x74,1,0
.byte	0,0x64,2,0
.byte	0,0xb3
.byte	0,0
.long	0,0
.LSEH_info_ctx_inverse_mod_384_body:
.byte	1,0,18,0
.byte	0x00,0xf4,0x8b,0x00
.byte	0x00,0xe4,0x8c,0x00
.byte	0x00,0xd4,0x8d,0x00
.byte	0x00,0xc4,0x8e,0x00
.byte	0x00,0x34,0x8f,0x00
.byte	0x00,0x54,0x90,0x00
.byte	0x00,0x74,0x92,0x00
.byte	0x00,0x64,0x93,0x00
.byte	0x00,0x01,0x91,0x00
.byte	0x00,0x00,0x00,0x00
.byte	0x00,0x00,0x00,0x00
.LSEH_info_ctx_inverse_mod_384_epilogue:
.byte	1,0,4,0
.byte	0x00,0x74,0x01,0x00
.byte	0x00,0x64,0x02,0x00
.byte	0x00,0x00,0x00,0x00

