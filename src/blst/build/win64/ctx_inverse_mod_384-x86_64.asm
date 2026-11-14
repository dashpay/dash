OPTION	DOTNAME
ifdef	__BLST_PORTABLE__
PUBLIC	ct_inverse_mod_384$1
endif
.text$	SEGMENT ALIGN(256) 'CODE'

PUBLIC	ctx_inverse_mod_384


ALIGN	32
ctx_inverse_mod_384	PROC PUBLIC
	DB	243,15,30,250
	mov	QWORD PTR[8+rsp],rdi	;WIN64 prologue
	mov	QWORD PTR[16+rsp],rsi
	mov	r11,rsp
$L$SEH_begin_ctx_inverse_mod_384::


	mov	rdi,rcx
	mov	rsi,rdx
	mov	rdx,r8
	mov	rcx,r9
ct_inverse_mod_384$1::
	push	rbp

	push	rbx

	push	r12

	push	r13

	push	r14

	push	r15

	sub	rsp,1112

$L$SEH_body_ctx_inverse_mod_384::


	lea	rax,QWORD PTR[((88+511))+rsp]
	and	rax,-512
	mov	QWORD PTR[32+rsp],rdi
	mov	QWORD PTR[40+rsp],rcx

ifdef	__SGX_LVI_HARDENING__
	lfence
endif
	mov	r8,QWORD PTR[rsi]
	mov	r9,QWORD PTR[8+rsi]
	mov	r10,QWORD PTR[16+rsi]
	mov	r11,QWORD PTR[24+rsi]
	mov	r12,QWORD PTR[32+rsi]
	mov	r13,QWORD PTR[40+rsi]

	mov	r14,QWORD PTR[rdx]
	mov	r15,QWORD PTR[8+rdx]
	mov	rbx,QWORD PTR[16+rdx]
	mov	rbp,QWORD PTR[24+rdx]
	mov	rsi,QWORD PTR[32+rdx]
	mov	rdi,QWORD PTR[40+rdx]

	mov	QWORD PTR[rax],r8
	mov	QWORD PTR[8+rax],r9
	mov	QWORD PTR[16+rax],r10
	mov	QWORD PTR[24+rax],r11
	mov	QWORD PTR[32+rax],r12
	mov	QWORD PTR[40+rax],r13

	mov	QWORD PTR[48+rax],r14
	mov	QWORD PTR[56+rax],r15
	mov	QWORD PTR[64+rax],rbx
	mov	QWORD PTR[72+rax],rbp
	mov	QWORD PTR[80+rax],rsi
	mov	rsi,rax
	mov	QWORD PTR[88+rax],rdi


	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31


	mov	QWORD PTR[96+rdi],rdx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31


	mov	QWORD PTR[104+rdi],rdx


	xor	rsi,256
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31



	mov	rax,QWORD PTR[96+rsi]
	mov	r11,QWORD PTR[152+rsi]
	mov	rbx,rdx
	mov	r10,rax
	imul	QWORD PTR[56+rsp]
	mov	r8,rax
	mov	rax,r11
	mov	r9,rdx
	imul	QWORD PTR[64+rsp]
	add	r8,rax
	adc	r9,rdx
	mov	QWORD PTR[48+rdi],r8
	mov	QWORD PTR[56+rdi],r9
	sar	r9,63
	mov	QWORD PTR[64+rdi],r9
	mov	QWORD PTR[72+rdi],r9
	mov	QWORD PTR[80+rdi],r9
	mov	QWORD PTR[88+rdi],r9
	mov	QWORD PTR[96+rdi],r9
	lea	rsi,QWORD PTR[96+rsi]

	mov	rax,r10
	imul	rbx
	mov	r8,rax
	mov	rax,r11
	mov	r9,rdx
	imul	rcx
	add	r8,rax
	adc	r9,rdx
	mov	QWORD PTR[104+rdi],r8
	mov	QWORD PTR[112+rdi],r9
	sar	r9,63
	mov	QWORD PTR[120+rdi],r9
	mov	QWORD PTR[128+rdi],r9
	mov	QWORD PTR[136+rdi],r9
	mov	QWORD PTR[144+rdi],r9
	mov	QWORD PTR[152+rdi],r9
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_384x63
	mov	QWORD PTR[56+rdi],r14
	mov	QWORD PTR[64+rdi],r14
	mov	QWORD PTR[72+rdi],r14
	mov	QWORD PTR[80+rdi],r14
	mov	QWORD PTR[88+rdi],r14
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63
	xor	rsi,256+8*12
	mov	edi,31
	call	__ab_approximation_31


	mov	QWORD PTR[72+rsp],r12
	mov	QWORD PTR[80+rsp],r13

	mov	rdi,256
	xor	rdi,rsi
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[56+rsp],rdx
	mov	QWORD PTR[64+rsp],rcx

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_191_n_shift_by_31
	mov	QWORD PTR[72+rsp],rdx
	mov	QWORD PTR[80+rsp],rcx

	mov	rdx,QWORD PTR[56+rsp]
	mov	rcx,QWORD PTR[64+rsp]
	lea	rsi,QWORD PTR[96+rsi]
	lea	rdi,QWORD PTR[48+rdi]
	call	__smulx_384x63

	mov	rdx,QWORD PTR[72+rsp]
	mov	rcx,QWORD PTR[80+rsp]
	lea	rdi,QWORD PTR[56+rdi]
	call	__smulx_768x63

	xor	rsi,256+8*12
	mov	edi,55

	mov	r8,QWORD PTR[rsi]

	mov	r10,QWORD PTR[48+rsi]

	call	__tail_loop_55







	lea	rsi,QWORD PTR[96+rsi]





	mov	rdx,r12
	mov	rcx,r13
	mov	rdi,QWORD PTR[32+rsp]
	call	__smulx_768x63

	mov	rsi,QWORD PTR[40+rsp]
	mov	r13,rdx
	sar	r13,63

	mov	r8,r13
	mov	r9,r13
	mov	r10,r13
ifdef	__SGX_LVI_HARDENING__
	lfence
endif
	and	r8,QWORD PTR[rsi]
	and	r9,QWORD PTR[8+rsi]
	mov	r11,r13
	and	r10,QWORD PTR[16+rsi]
	and	r11,QWORD PTR[24+rsi]
	mov	r12,r13
	and	r12,QWORD PTR[32+rsi]
	and	r13,QWORD PTR[40+rsi]

	add	r14,r8
	adc	r15,r9
	adc	rbx,r10
	adc	rbp,r11
	adc	rcx,r12
	adc	rax,r13
	adc	rdx,0

	mov	r13,rdx
	neg	rdx
	or	r13,rdx
	sar	rdx,63

	mov	r8,r13
	mov	r9,r13
	mov	r10,r13
	and	r8,QWORD PTR[rsi]
	and	r9,QWORD PTR[8+rsi]
	mov	r11,r13
	and	r10,QWORD PTR[16+rsi]
	and	r11,QWORD PTR[24+rsi]
	mov	r12,r13
	and	r12,QWORD PTR[32+rsi]
	and	r13,QWORD PTR[40+rsi]

	xor	r8,rdx
	xor	rsi,rsi
	xor	r9,rdx
	sub	rsi,rdx
	xor	r10,rdx
	xor	r11,rdx
	xor	r12,rdx
	xor	r13,rdx
	add	r8,rsi
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0

	add	r14,r8
	adc	r15,r9
	adc	rbx,r10
	adc	rbp,r11
	adc	rcx,r12
	adc	rax,r13

	mov	QWORD PTR[48+rdi],r14
	mov	QWORD PTR[56+rdi],r15
	mov	QWORD PTR[64+rdi],rbx
	mov	QWORD PTR[72+rdi],rbp
	mov	QWORD PTR[80+rdi],rcx
	mov	QWORD PTR[88+rdi],rax

	lea	r8,QWORD PTR[1112+rsp]
	mov	r15,QWORD PTR[r8]

	mov	r14,QWORD PTR[8+r8]

	mov	r13,QWORD PTR[16+r8]

	mov	r12,QWORD PTR[24+r8]

	mov	rbx,QWORD PTR[32+r8]

	mov	rbp,QWORD PTR[40+r8]

	lea	rsp,QWORD PTR[48+r8]

$L$SEH_epilogue_ctx_inverse_mod_384::
	mov	rdi,QWORD PTR[8+rsp]	;WIN64 epilogue
	mov	rsi,QWORD PTR[16+rsp]

	
ifdef	__SGX_LVI_HARDENING__
	pop	rdx
	lfence
	jmp	rdx
	ud2
else
	DB	0F3h,0C3h
endif

$L$SEH_end_ctx_inverse_mod_384::
ctx_inverse_mod_384	ENDP

ALIGN	32
__smulx_768x63	PROC PRIVATE
	DB	243,15,30,250

	mov	r8,QWORD PTR[rsi]
	mov	r9,QWORD PTR[8+rsi]
	mov	r10,QWORD PTR[16+rsi]
	mov	r11,QWORD PTR[24+rsi]
	mov	r12,QWORD PTR[32+rsi]
	mov	r13,QWORD PTR[40+rsi]
	mov	r14,QWORD PTR[48+rsi]

	mov	rax,rdx
	sar	rax,63
	xor	rbp,rbp
	sub	rbp,rax

	mov	QWORD PTR[8+rsp],rdi
	mov	QWORD PTR[16+rsp],rsi
	lea	rsi,QWORD PTR[56+rsi]

	xor	rdx,rax
	add	rdx,rbp

	xor	r8,rax
	xor	r9,rax
	xor	r10,rax
	xor	r11,rax
	xor	r12,rax
	xor	r13,rax
	xor	r14,rax
	add	r8,rbp
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0
	adc	r14,0

	and	r14,rdx
	neg	r14

	mulx	rbp,r8,r8
	mulx	rax,r9,r9
	add	r9,rbp
	mulx	rbp,r10,r10
	adc	r10,rax
	mulx	rax,r11,r11
	adc	r11,rbp
	mulx	rbp,r12,r12
	adc	r12,rax
	mulx	rax,r13,r13
	adc	r13,rbp
	adc	r14,rax

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	QWORD PTR[40+rdi],r13
	mov	QWORD PTR[48+rdi],r14
	sar	r14,63
	mov	QWORD PTR[56+rdi],r14
	mov	rdx,rcx
	mov	rax,rcx

	mov	r8,QWORD PTR[rsi]
	mov	r9,QWORD PTR[8+rsi]
	mov	r10,QWORD PTR[16+rsi]
	mov	r11,QWORD PTR[24+rsi]
	mov	r12,QWORD PTR[32+rsi]
	mov	r13,QWORD PTR[40+rsi]
	mov	r14,QWORD PTR[48+rsi]
	mov	r15,QWORD PTR[56+rsi]
	mov	rbx,QWORD PTR[64+rsi]
	mov	rbp,QWORD PTR[72+rsi]
	mov	rcx,QWORD PTR[80+rsi]
	mov	rdi,QWORD PTR[88+rsi]

	sar	rax,63
	xor	rsi,rsi
	sub	rsi,rax

	xor	rdx,rax
	add	rdx,rsi

	xor	r8,rax
	xor	r9,rax
	xor	r10,rax
	xor	r11,rax
	xor	r12,rax
	xor	r13,rax
	xor	r14,rax
	xor	r15,rax
	xor	rbx,rax
	xor	rbp,rax
	xor	rcx,rax
	xor	rax,rdi
	add	r8,rsi
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0
	adc	r14,0
	adc	r15,0
	adc	rbx,0
	adc	rbp,0
	adc	rcx,0
	adc	rax,0

	mulx	rsi,r8,r8
	mulx	rdi,r9,r9
	add	r9,rsi
	mulx	rsi,r10,r10
	adc	r10,rdi
	mulx	rdi,r11,r11
	adc	r11,rsi
	mulx	rsi,r12,r12
	adc	r12,rdi
	mulx	rdi,r13,r13
	adc	r13,rsi
	mulx	rsi,r14,r14
	adc	r14,rdi
	mulx	rdi,r15,r15
	adc	r15,rsi
	mulx	rsi,rbx,rbx
	adc	rbx,rdi
	mulx	rdi,rbp,rbp
	adc	rbp,rsi
	mulx	rsi,rcx,rcx
	adc	rcx,rdi
	mov	rdi,QWORD PTR[8+rsp]
	adc	rsi,0
	imul	rdx
	add	rax,rsi
	adc	rdx,0

	add	r8,QWORD PTR[rdi]
	adc	r9,QWORD PTR[8+rdi]
	adc	r10,QWORD PTR[16+rdi]
	adc	r11,QWORD PTR[24+rdi]
	adc	r12,QWORD PTR[32+rdi]
	adc	r13,QWORD PTR[40+rdi]
	adc	r14,QWORD PTR[48+rdi]
	mov	rsi,QWORD PTR[56+rdi]
	adc	r15,rsi
	adc	rbx,rsi
	adc	rbp,rsi
	adc	rcx,rsi
	adc	rax,rsi
	adc	rdx,rsi

	mov	rsi,QWORD PTR[16+rsp]

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	QWORD PTR[40+rdi],r13
	mov	QWORD PTR[48+rdi],r14
	mov	QWORD PTR[56+rdi],r15
	mov	QWORD PTR[64+rdi],rbx
	mov	QWORD PTR[72+rdi],rbp
	mov	QWORD PTR[80+rdi],rcx
	mov	QWORD PTR[88+rdi],rax

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__smulx_768x63	ENDP

ALIGN	32
__smulx_384x63	PROC PRIVATE
	DB	243,15,30,250

	mov	r8,QWORD PTR[((0+0))+rsi]
	mov	r9,QWORD PTR[((0+8))+rsi]
	mov	r10,QWORD PTR[((0+16))+rsi]
	mov	r11,QWORD PTR[((0+24))+rsi]
	mov	r12,QWORD PTR[((0+32))+rsi]
	mov	r13,QWORD PTR[((0+40))+rsi]
	mov	r14,QWORD PTR[((0+48))+rsi]

	mov	rbp,rdx
	sar	rbp,63
	xor	rax,rax
	sub	rax,rbp

	xor	rdx,rbp
	add	rdx,rax

	xor	r8,rbp
	xor	r9,rbp
	xor	r10,rbp
	xor	r11,rbp
	xor	r12,rbp
	xor	r13,rbp
	xor	r14,rbp
	add	r8,rax
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0
	adc	r14,0

	and	r14,rdx
	neg	r14

	mulx	rbp,r8,r8
	mulx	rax,r9,r9
	add	r9,rbp
	mulx	rbp,r10,r10
	adc	r10,rax
	mulx	rax,r11,r11
	adc	r11,rbp
	mulx	rbp,r12,r12
	adc	r12,rax
	mulx	rax,r13,r13
	mov	rdx,rcx
	adc	r13,rbp
	adc	r14,rax

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	r15,r13
	mov	rbx,r14
	mov	r8,QWORD PTR[((56+0))+rsi]
	mov	r9,QWORD PTR[((56+8))+rsi]
	mov	r10,QWORD PTR[((56+16))+rsi]
	mov	r11,QWORD PTR[((56+24))+rsi]
	mov	r12,QWORD PTR[((56+32))+rsi]
	mov	r13,QWORD PTR[((56+40))+rsi]
	mov	r14,QWORD PTR[((56+48))+rsi]

	mov	rbp,rdx
	sar	rbp,63
	xor	rax,rax
	sub	rax,rbp

	xor	rdx,rbp
	add	rdx,rax

	xor	r8,rbp
	xor	r9,rbp
	xor	r10,rbp
	xor	r11,rbp
	xor	r12,rbp
	xor	r13,rbp
	xor	r14,rbp
	add	r8,rax
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0
	adc	r14,0

	and	r14,rdx
	neg	r14

	mulx	rbp,r8,r8
	mulx	rax,r9,r9
	add	r9,rbp
	mulx	rbp,r10,r10
	adc	r10,rax
	mulx	rax,r11,r11
	adc	r11,rbp
	mulx	rbp,r12,r12
	adc	r12,rax
	mulx	rax,r13,r13
	adc	r13,rbp
	adc	r14,rax

	add	r8,QWORD PTR[rdi]
	adc	r9,QWORD PTR[8+rdi]
	adc	r10,QWORD PTR[16+rdi]
	adc	r11,QWORD PTR[24+rdi]
	adc	r12,QWORD PTR[32+rdi]
	adc	r13,r15
	adc	r14,rbx

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	QWORD PTR[40+rdi],r13
	mov	QWORD PTR[48+rdi],r14

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__smulx_384x63	ENDP

ALIGN	32
__smulx_384_n_shift_by_31	PROC PRIVATE
	DB	243,15,30,250

	mov	rbx,rdx
	mov	r8,QWORD PTR[((0+0))+rsi]
	mov	r9,QWORD PTR[((0+8))+rsi]
	mov	r10,QWORD PTR[((0+16))+rsi]
	mov	r11,QWORD PTR[((0+24))+rsi]
	mov	r12,QWORD PTR[((0+32))+rsi]
	mov	r13,QWORD PTR[((0+40))+rsi]

	mov	rax,rdx
	sar	rax,63
	xor	rbp,rbp
	sub	rbp,rax

	xor	rdx,rax
	add	rdx,rbp

	xor	r8,rax
	xor	r9,rax
	xor	r10,rax
	xor	r11,rax
	xor	r12,rax
	xor	r13,rax
	add	r8,rbp
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0

	and	rax,rdx
	neg	rax

	mulx	rbp,r8,r8
	mulx	r14,r9,r9
	add	r9,rbp
	mulx	rbp,r10,r10
	adc	r10,r14
	mulx	r14,r11,r11
	adc	r11,rbp
	mulx	rbp,r12,r12
	adc	r12,r14
	mulx	r14,r13,r13
	adc	r13,rbp
	adc	r14,rax

	mov	rdx,rcx

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	QWORD PTR[40+rdi],r13
	mov	r15,r14
	mov	r8,QWORD PTR[((48+0))+rsi]
	mov	r9,QWORD PTR[((48+8))+rsi]
	mov	r10,QWORD PTR[((48+16))+rsi]
	mov	r11,QWORD PTR[((48+24))+rsi]
	mov	r12,QWORD PTR[((48+32))+rsi]
	mov	r13,QWORD PTR[((48+40))+rsi]

	mov	rax,rdx
	sar	rax,63
	xor	rbp,rbp
	sub	rbp,rax

	xor	rdx,rax
	add	rdx,rbp

	xor	r8,rax
	xor	r9,rax
	xor	r10,rax
	xor	r11,rax
	xor	r12,rax
	xor	r13,rax
	add	r8,rbp
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0

	and	rax,rdx
	neg	rax

	mulx	rbp,r8,r8
	mulx	r14,r9,r9
	add	r9,rbp
	mulx	rbp,r10,r10
	adc	r10,r14
	mulx	r14,r11,r11
	adc	r11,rbp
	mulx	rbp,r12,r12
	adc	r12,r14
	mulx	r14,r13,r13
	adc	r13,rbp
	adc	r14,rax

	add	r8,QWORD PTR[rdi]
	adc	r9,QWORD PTR[8+rdi]
	adc	r10,QWORD PTR[16+rdi]
	adc	r11,QWORD PTR[24+rdi]
	adc	r12,QWORD PTR[32+rdi]
	adc	r13,QWORD PTR[40+rdi]
	adc	r14,r15
	mov	rdx,rbx

	shrd	r8,r9,31
	shrd	r9,r10,31
	shrd	r10,r11,31
	shrd	r11,r12,31
	shrd	r12,r13,31
	shrd	r13,r14,31

	sar	r14,63
	xor	rbp,rbp
	sub	rbp,r14

	xor	r8,r14
	xor	r9,r14
	xor	r10,r14
	xor	r11,r14
	xor	r12,r14
	xor	r13,r14
	add	r8,rbp
	adc	r9,0
	adc	r10,0
	adc	r11,0
	adc	r12,0
	adc	r13,0

	mov	QWORD PTR[rdi],r8
	mov	QWORD PTR[8+rdi],r9
	mov	QWORD PTR[16+rdi],r10
	mov	QWORD PTR[24+rdi],r11
	mov	QWORD PTR[32+rdi],r12
	mov	QWORD PTR[40+rdi],r13

	xor	rdx,r14
	xor	rcx,r14
	add	rdx,rbp
	add	rcx,rbp

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__smulx_384_n_shift_by_31	ENDP

ALIGN	32
__smulx_191_n_shift_by_31	PROC PRIVATE
	DB	243,15,30,250

	mov	rbx,rdx
	mov	r8,QWORD PTR[((0+0))+rsi]
	mov	r9,QWORD PTR[((0+8))+rsi]
	mov	r10,QWORD PTR[((0+16))+rsi]

	mov	rax,rdx
	sar	rax,63
	xor	rbp,rbp
	sub	rbp,rax

	xor	rdx,rax
	add	rdx,rbp

	xor	r8,rax
	xor	r9,rax
	xor	rax,r10
	add	r8,rbp
	adc	r9,0
	adc	rax,0

	mulx	rbp,r8,r8
	mulx	r10,r9,r9
	add	r9,rbp
	adc	r10,0
	imul	rdx
	add	r10,rax
	adc	rdx,0
	mov	r14,rdx
	mov	rdx,rcx
	mov	r11,QWORD PTR[((48+0))+rsi]
	mov	r12,QWORD PTR[((48+8))+rsi]
	mov	r13,QWORD PTR[((48+16))+rsi]

	mov	rax,rdx
	sar	rax,63
	xor	rbp,rbp
	sub	rbp,rax

	xor	rdx,rax
	add	rdx,rbp

	xor	r11,rax
	xor	r12,rax
	xor	rax,r13
	add	r11,rbp
	adc	r12,0
	adc	rax,0

	mulx	rbp,r11,r11
	mulx	r13,r12,r12
	add	r12,rbp
	adc	r13,0
	imul	rdx
	add	r13,rax
	adc	rdx,0
	add	r11,r8
	adc	r12,r9
	adc	r13,r10
	adc	r14,rdx
	mov	rdx,rbx

	shrd	r11,r12,31
	shrd	r12,r13,31
	shrd	r13,r14,31

	sar	r14,63
	xor	rbp,rbp
	sub	rbp,r14

	xor	r11,r14
	xor	r12,r14
	xor	r13,r14
	add	r11,rbp
	adc	r12,0
	adc	r13,0

	mov	QWORD PTR[rdi],r11
	mov	QWORD PTR[8+rdi],r12
	mov	QWORD PTR[16+rdi],r13

	xor	rdx,r14
	xor	rcx,r14
	add	rdx,rbp
	add	rcx,rbp

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__smulx_191_n_shift_by_31	ENDP

ALIGN	32
__ab_approximation_31	PROC PRIVATE
	DB	243,15,30,250

	mov	r9,QWORD PTR[40+rsi]
	mov	r11,QWORD PTR[88+rsi]
	mov	rbx,QWORD PTR[32+rsi]
	mov	rbp,QWORD PTR[80+rsi]
	mov	r8,QWORD PTR[24+rsi]
	mov	r10,QWORD PTR[72+rsi]

	mov	rax,r9
	or	rax,r11
	cmovz	r9,rbx
	cmovz	r11,rbp
	cmovz	rbx,r8
	mov	r8,QWORD PTR[16+rsi]
	cmovz	rbp,r10
	mov	r10,QWORD PTR[64+rsi]

	mov	rax,r9
	or	rax,r11
	cmovz	r9,rbx
	cmovz	r11,rbp
	cmovz	rbx,r8
	mov	r8,QWORD PTR[8+rsi]
	cmovz	rbp,r10
	mov	r10,QWORD PTR[56+rsi]

	mov	rax,r9
	or	rax,r11
	cmovz	r9,rbx
	cmovz	r11,rbp
	cmovz	rbx,r8
	mov	r8,QWORD PTR[rsi]
	cmovz	rbp,r10
	mov	r10,QWORD PTR[48+rsi]

	mov	rax,r9
	or	rax,r11
	cmovz	r9,rbx
	cmovz	r11,rbp
	cmovz	rbx,r8
	cmovz	rbp,r10

	mov	rax,r9
	or	rax,r11
	bsr	rcx,rax
	lea	rcx,QWORD PTR[1+rcx]
	cmovz	r9,r8
	cmovz	r11,r10
	cmovz	rcx,rax
	neg	rcx


	shld	r9,rbx,cl
	shld	r11,rbp,cl

	mov	eax,07FFFFFFFh
	and	r8,rax
	and	r10,rax
	andn	r9,rax,r9
	andn	r11,rax,r11
	or	r8,r9
	or	r10,r11

	jmp	__inner_loop_31

	
ifdef	__SGX_LVI_HARDENING__
	pop	rdx
	lfence
	jmp	rdx
	ud2
else
	DB	0F3h,0C3h
endif
__ab_approximation_31	ENDP

ALIGN	32
__inner_loop_31	PROC PRIVATE
	DB	243,15,30,250

	mov	rcx,07FFFFFFF80000000h
	mov	r13,0800000007FFFFFFFh
	mov	r15,07FFFFFFF7FFFFFFFh

$L$oop_31::
	cmp	r8,r10
	mov	rax,r8
	mov	rbx,r10
	mov	rbp,rcx
	mov	r14,r13
	cmovb	r8,r10
	cmovb	r10,rax
	cmovb	rcx,r13
	cmovb	r13,rbp

	sub	r8,r10
	sub	rcx,r13
	add	rcx,r15

	test	rax,1
	cmovz	r8,rax
	cmovz	r10,rbx
	cmovz	rcx,rbp
	cmovz	r13,r14

	shr	r8,1
	add	r13,r13
	sub	r13,r15
	sub	edi,1
	jnz	$L$oop_31

	shr	r15,32
	mov	edx,ecx
	mov	r12d,r13d
	shr	rcx,32
	shr	r13,32
	sub	rdx,r15
	sub	rcx,r15
	sub	r12,r15
	sub	r13,r15

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__inner_loop_31	ENDP


ALIGN	32
__tail_loop_55	PROC PRIVATE
	DB	243,15,30,250

	mov	rdx,1
	xor	rcx,rcx
	xor	r12,r12
	mov	r13,1

$L$oop_55::
	xor	rax,rax
	test	r8,1
	mov	rbx,r10
	cmovnz	rax,r10
	sub	rbx,r8
	mov	rbp,r8
	sub	r8,rax
	cmovc	r8,rbx
	cmovc	r10,rbp
	mov	rax,rdx
	cmovc	rdx,r12
	cmovc	r12,rax
	mov	rbx,rcx
	cmovc	rcx,r13
	cmovc	r13,rbx
	xor	rax,rax
	xor	rbx,rbx
	shr	r8,1
	test	rbp,1
	cmovnz	rax,r12
	cmovnz	rbx,r13
	add	r12,r12
	add	r13,r13
	sub	rdx,rax
	sub	rcx,rbx
	sub	edi,1
	jnz	$L$oop_55

	
ifdef	__SGX_LVI_HARDENING__
	pop	r8
	lfence
	jmp	r8
	ud2
else
	DB	0F3h,0C3h
endif
__tail_loop_55	ENDP
.text$	ENDS
.pdata	SEGMENT READONLY ALIGN(4)
ALIGN	4
	DD	imagerel $L$SEH_begin_ctx_inverse_mod_384
	DD	imagerel $L$SEH_body_ctx_inverse_mod_384
	DD	imagerel $L$SEH_info_ctx_inverse_mod_384_prologue

	DD	imagerel $L$SEH_body_ctx_inverse_mod_384
	DD	imagerel $L$SEH_epilogue_ctx_inverse_mod_384
	DD	imagerel $L$SEH_info_ctx_inverse_mod_384_body

	DD	imagerel $L$SEH_epilogue_ctx_inverse_mod_384
	DD	imagerel $L$SEH_end_ctx_inverse_mod_384
	DD	imagerel $L$SEH_info_ctx_inverse_mod_384_epilogue

.pdata	ENDS
.xdata	SEGMENT READONLY ALIGN(8)
ALIGN	8
$L$SEH_info_ctx_inverse_mod_384_prologue::
DB	1,0,5,00bh
DB	0,074h,1,0
DB	0,064h,2,0
DB	0,0b3h
DB	0,0
	DD	0,0
$L$SEH_info_ctx_inverse_mod_384_body::
DB	1,0,18,0
DB	000h,0f4h,08bh,000h
DB	000h,0e4h,08ch,000h
DB	000h,0d4h,08dh,000h
DB	000h,0c4h,08eh,000h
DB	000h,034h,08fh,000h
DB	000h,054h,090h,000h
DB	000h,074h,092h,000h
DB	000h,064h,093h,000h
DB	000h,001h,091h,000h
DB	000h,000h,000h,000h
DB	000h,000h,000h,000h
$L$SEH_info_ctx_inverse_mod_384_epilogue::
DB	1,0,4,0
DB	000h,074h,001h,000h
DB	000h,064h,002h,000h
DB	000h,000h,000h,000h


.xdata	ENDS
END
