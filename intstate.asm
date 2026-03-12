SECTION code_user

PUBLIC _set_motor_on
PUBLIC _set_motor_off
PUBLIC _inportb
PUBLIC _outportb

defc BANK678 = $5B67

; void set_motor_on(void)
; Enable motor (set bit 3), preserving other paging bits.

_set_motor_on:
        ld      a,(BANK678)
        or      $08                 ; set bit 3 (motor on)
        jp      write_1ffd

; void set_motor_off(void)
; Disable motor (clear bit 3), preserving other paging bits.

_set_motor_off:
        ld      a,(BANK678)
        and     $F7                 ; clear bit 3 (motor off)
        jp      write_1ffd

; write_1ffd: write A to port $1FFD and both shadows.
; Use explicit DI/EI around write to keep behaviour simple and stable.

write_1ffd:
        push    bc
        ld      bc,$1FFD
        di
        ld      (BANK678),a         ; update ROM shadow
        out     (c),a               ; write port $1FFD
        ei
        pop     bc
        ret

; unsigned char inportb(unsigned short port)
; __smallc call convention: arg word at sp+2
_inportb:
        pop     de                  ; return address
        pop     bc                  ; port
        in      l,(c)
        ld      h,0
        push    bc
        push    de
        ret

; void outportb(unsigned short port, unsigned char value)
; __smallc call convention: value word at sp+2, port word at sp+4
_outportb:
        pop     de                  ; return address
        pop     hl                  ; value in L
        pop     bc                  ; port
        out     (c),l
        push    bc
        push    hl
        push    de
        ret