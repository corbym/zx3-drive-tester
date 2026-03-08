SECTION code_user

PUBLIC _push_di_state
PUBLIC _pop_ei_state

; unsigned char push_di_state(void)
; Returns 1 if interrupts were enabled on entry, else 0.
_push_di_state:
    ld   a,r
    di
    jp   po, ints_were_off
    ld   l,1
    ret
ints_were_off:
    ld   l,0
    ret

; void pop_ei_state(unsigned char state)
; state is passed in L 
_pop_ei_state:
    ld   a,l
    or   a
    ret  z
    ei
    ret