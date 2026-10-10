; Tiny HiROM test cartridge for the host program (no EarthBound data).
; Cycles the backdrop colour and writes counters where EarthBound keeps Ness's X/Y,
; so the F1 debug panel and --frames test runs have something to show.
.p816
.smart

.segment "CODE"
reset:
    sei
    clc
    xce
    rep #$38
    ldx #$1FFF
    txs
    sep #$20
    lda #$80
    sta $2100           ; force blank
    stz $2121           ; CGRAM address 0
    lda #$0F
    sta $2100           ; screen on, full brightness
    rep #$20
    lda #1000
    sta $7E9877         ; "Ness X"
    lda #2000
    sta $7E987B         ; "Ness Y"
    lda #4
    sta $7E987F         ; facing down
loop:
    sep #$20
@wait_vblank:
    lda $4212
    bpl @wait_vblank
    rep #$20
    lda $7E9877
    inc a
    sta $7E9877
    lda $7E987B
    dec a
    sta $7E987B
    sep #$20
    stz $2121
    lda $7E9877
    sta $2122           ; backdrop colour low byte
    lda #$7C
    sta $2122
@wait_end:
    lda $4212
    bmi @wait_end
    bra loop

nmi:
    rti

.segment "HEADER"       ; $FFC0
    .byte "FPEB TEST ROM        "
    .byte $31           ; HiROM, FastROM
    .byte $00           ; ROM only
    .byte $08           ; 256 KB
    .byte $00, $01, $00, $00
    .word $0000, $FFFF  ; checksum complement, checksum
    .word 0, 0, 0, 0, 0, .loword(nmi), 0, 0  ; native vectors
    .word 0, 0, 0, 0, 0, 0, .loword(reset), 0 ; emulation vectors (code runs from the $00:8000 mirror)
