    .section .reset, "ax"
    .global _start
    .type _start, @function
_start:
    /* Setup stack pointer */
    lda.w   sp, _stack
    
    /* Jump to data initialization */
    rjmp    _init_data

    .section .text
    .global _init_data
    .type _init_data, @function
_init_data:
    /* Clear BSS section */
    lda.w   r0, _sbss
    lda.w   r1, _ebss
    mov     r2, 0
1:
    cp.w    r0, r1
    brge    2f
    st.w    r0[0], r2
    sub     r0, -4
    rjmp    1b
2:
    /* Execute main */
    call    main
3:
    /* Loop forever if main returns */
    rjmp    3b