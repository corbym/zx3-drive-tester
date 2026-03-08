SECTION code_user

PUBLIC _motor_on_asm
PUBLIC _motor_off_asm

defc BANK678 = $5B67

; DD_L_ON_MOTOR  (user-space safe version)
;
; The ROM version references tm_mtron ($E428), tm_mtroff ($E429) and
; timeout ($E600) which are +3DOS internal variables in page 7 of RAM.
; Page 7 is only mapped at $C000-$FFFF when the DOS ROM itself executes.
; In a user program a different RAM page is there; those writes corrupt
; program memory and cause a crash that surfaces when interrupts are
; later re-enabled (INTRQ floods the stack via the IM1 handler).
;
; Spinup delay is handled by delay_ms(500) on the C side.
; The motor-off ticker is not needed: user code turns the motor off
; explicitly, and the ticker variable is inaccessible anyway.
;
; Interrupt state is preserved exactly as in the ROM's set_bank_to_a:
; LD A,R copies IFF2 into P/V; the following LD A,B (reg-to-reg) does
; NOT alter flags, so P/V survives to the RET PO / EI test.

_motor_on_asm:
        push    bc
        push    af
        ld      a,(BANK678)
        bit     3,a
        jr      nz,motor_on_exit    ; motor already on, nothing to do
        or      $08
        call    set_bank_to_a       ; set motor bit and write port
motor_on_exit:
        pop     af
        pop     bc
        ret

; DD_L_OFF_MOTOR  (user-space safe version)

_motor_off_asm:
        push    bc
        push    af
        ld      a,(BANK678)
        and     $F7                 ; clear motor bit
        call    set_bank_to_a       ; clear motor bit and write port
        pop     af
        pop     bc
        ret

; Write A to BANK678 shadow and port $1FFD with interrupt-state preservation.
; Identical logic to the ROM's set_bank_to_a routine.

set_bank_to_a:
        push    bc
        ld      b,a                 ; save value; LD r,r does NOT alter flags
        ld      a,r                 ; P/V = IFF2 (NMOS Z80)
        ld      a,b                 ; restore value; flags unchanged
        ld      bc,$1FFD
        di
        ld      (BANK678),a         ; update shadow before port write
        out     (c),a               ; write port $1FFD
        pop     bc
        ret     po                  ; P/V=0: ints were off, return without EI
        ei
        ret