; Main BSDOS logic, BASIC integration
;
; Copyright 2026 Ciaran Anscomb
;
; This file is part of BSDOS, a simple Dragon DOS ROM with enough
; functionality to BOOT from a device.
;
; BSDOS is free software; you can redistribute it and/or modify it under
; the terms of the GNU General Public License as published by the Free
; Software Foundation, either version 3 of the License, or (at your
; option) any later version.
;
; See COPYING.GPL for redistribution conditions.

; Implementation notes about potential future DragonDOS compatibility:
;
; All operations and tracking within the main code is in terms of LBA
; addressing of 512-byte sectors, and these are the addresses passed to
; all driver code.
;
; Internally, DragonDOS LSNs are in terms of 256-byte sectors, so
; translations will need to be made, and when one sector is read, the
; appropriate half of a buffer is copied.

; Writes are harder.  We need to be sure we have the 512-byte LBA in
; memory before we copy 256 bytes of sector to it.

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; Dragon BASIC addresses and constants:

DEVNUM			equ 0x006F

bas_basic_start		equ 0x0019	; start address of BASIC program
bas_stack_top		equ 0x0021	; top of stack, growing down
bas_string_top		equ 0x0023	; top of free string space
bas_ram_top		equ 0x0027
bas_lno			equ 0x0068
bas_restart_flag	equ 0x0071	; cold restart if not 0x55 on reset
bas_restart_vector	equ 0x0072	; warm start if points to NOP
bas_gfx_end		equ 0x00B7	; ptr to last byte+1 of current graphics mode
bas_gfx_start		equ 0x00BA	; ptr to first byte of current graphics mode
bas_gfx_base		equ 0x00BC	; MSB of graphics base

_bas_SWI3		equ 0x0100
_bas_SWI2		equ 0x0103
_bas_SWI		equ 0x0106
_bas_NMI		equ 0x0109
_bas_IRQ		equ 0x010C
_bas_FIRQ		equ 0x010F

bas_nwords		equ 78
bas_nfwords		equ 34

bas_stub0		equ 0x0120
bas_stub0_nwords	equ 0x0120
bas_stub0_words		equ 0x0121
bas_stub0_tok_handle	equ 0x0123
bas_stub0_nfwords	equ 0x0125
bas_stub0_fwords	equ 0x0126
bas_stub0_ftok_handle	equ 0x0128
bas_stub1		equ 0x012A
bas_stub1_nwords	equ 0x012A
bas_stub1_words		equ 0x012B
bas_stub1_tok_handle	equ 0x012D
bas_stub1_nfwords	equ 0x012F
bas_stub1_fwords	equ 0x0130
bas_stub1_ftok_handle	equ 0x0132
bas_stub2		equ 0x0134
bas_stub2_nwords	equ 0x0134
bas_stub2_words		equ 0x0135
bas_stub2_tok_handle	equ 0x0137
bas_stub2_nfwords	equ 0x0139
bas_stub2_fwords	equ 0x013A
bas_stub2_ftok_handle	equ 0x013C
bas_hook_sys_error	equ 0x0191

_POLCAT			equ 0x8006	; scan keyboard
_CHROUT			equ 0x800C	; output character in A to DEVNUM
_CASOFF			equ 0x8018	; turn off cassette motor

bas_errcode_table	equ 0x82A9
bas_system_error	equ 0x8344
bas_print_error		equ 0x835E	; print error B from table X and return to OK prompt
bas_ok_prompt		equ 0x8371
bas_vect2		equ 0x83ED
bas_erase_basic		equ 0x8416
bas_8424		equ 0x8424
bas_reset_stack		equ 0x8434
bas_run_basic		equ 0x849F
bas_tok_handle		equ 0x84ED
bas_sn_error		equ 0x89B4
bas_ftok_handle		equ 0x8A0B
bas_fc_error		equ 0x8B8D
bas_GIVABF		equ 0x8C37	; returns D as 16-bit val in FAC
bas_garbage_collect	equ 0x8CD7	; forces garbage collection of string space
bas_Get8Bit		equ 0x8E51
bas_dev_init_start_line	equ 0x90A5
bas_print_string	equ 0x90E5
bas_print_question	equ 0x90F8
bas_svc_IRQ		equ 0x9D3D

__POLCAT		equ 0xA000	; scan keyboard
__CHROUT		equ 0xA002	; output character in A to DEVNUM

bas_init_gfx_new_1	equ 0xAA87
bas_continue_init	equ 0xB43C
bas_warm_start		equ 0xB44F
bas_s_copyright		equ 0xB4B3
bas_OUTCHR		equ 0xB54A	; character output to device
bas_cpxub		equ 0xB7CC
bas_io_error		equ 0xB84B
bas_audio_off		equ 0xBAC3
bas_OUTCH		equ 0xBCAB	; character output to screen

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; Dragon hardware register definitions:

; PIA0
PIA0_DDRA		equ 0xFF00
PIA0_PDRA		equ 0xFF00
PIA0_CRA		equ 0xFF01
PIA0_DDRB		equ 0xFF02
PIA0_PDRB		equ 0xFF02
PIA0_CRB		equ 0xFF03

; PIA1
PIA1_DDRA		equ 0xFF20
PIA1_PDRA		equ 0xFF20
PIA1_CRA		equ 0xFF21
PIA1_DDRB		equ 0xFF22
PIA1_PDRB		equ 0xFF22
PIA1_CRB		equ 0xFF23

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; A buffer should be locked while its contents are not final.  An unlocked
; buffer is eligible to be ejected if needed.  Ejecting will write the
; block if marked as dirty.  The highest LRU is ejected first.

nbuffers	equ 3

		org 0
buf_state	rmb 1	; 0: empty, b7: locked, b6: dirty, rest: ru count
buf_drive	rmb 1
buf_lba		rmb 4
buf_data_ptr	rmb 2
sizeof_buf

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; Low-RAM variables

		section "dp"

		org 0x00EA

op_buffer	rmb sizeof_buf
op_drive	equ op_buffer+buf_drive
op_lba		equ op_buffer+buf_lba
op_data_ptr	equ op_buffer+buf_data_ptr

; Mid-RAM workspace

		section "work"

		org 0x0600
work_start

buf		rmb nbuffers*sizeof_buf
buf_end

; List of buffer indices, LRU to MRU.  Empty buffers always listed first.
buf_list	rmb nbuffers*2

ndrives		rmb 1
cur_drive	rmb 1

timer		rmb 2

last_erl	rmb 2
last_err	rmb 1

; Buffers

		align 256
bufdata		rmb nbuffers*512

; ROM preamble

		code

		org 0xC000
code_start
		fcc "DK"
		bra start
_dev_xfer_lba	fdb dev_xfer_lba
		; might need some vectors here

		section "tmp"
		data
data_start

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		code

start
		setdp 0

		; print normal BASIC startup message
		ldx #bas_s_copyright-1
		jsr bas_print_string

		lda bas_basic_start
		adda #work_end_page-6
		sta bas_basic_start
		lda bas_gfx_base
		adda #work_end_page-6
		jsr bas_init_gfx_new_1

		; initialise mid-ram workspace
		ldx #work_start
10		clr ,x+
		cmpx #work_end
		blo 10B

		; specific initialisation
		ldx #ram_init_table
10		ldb ,x+
		beq 20F
		ldu ,x++
		jsr bas_cpxub
		bra 10B
20

		; initialise buffers using eject_all_buffers.  state will
		; be clear thanks to the initialisation above, so that
		; function will not try and write any sectors.
		lbsr eject_all_buffers

		; initialise timers
		ldx #irq_service
		stx _bas_IRQ+1
		lda #$7e
		sta _bas_IRQ

		data

ram_init_table
		fcb sizeof_ram_init0
		fdb bas_stub1
ram_init0
		fcb nwords		; bas_stub1_nwords
		fdb words		; bas_stub1_words
		fdb tok_handle		; bas_stub1_tok_handle
		fcb nfwords		; bas_stub1_nfwords
		fdb fwords		; bas_stub1_fwords
		fdb ftok_handle	; bas_stub1_ftok_handle
		fcb 0			; bas_stub2_nwords
		fdb 0			; bas_stub2_words
		fdb bas_sn_error	; bas_stub2_tok_handle
		fcb 0			; bas_stub2_nfwords
		fdb 0			; bas_stub2_fwords
		fdb bas_sn_error	; bas_stub2_ftok_handle
		fcb 0			; bas_stub3_nwords
		fdb 0			; bas_stub3_words
		fdb 0			; bas_stub3_tok_handle
		fcb 0			; bas_stub3_nfwords
		fdb 0			; bas_stub3_fwords
		fdb 0			; bas_stub3_ftok_handle
sizeof_ram_init0	equ *-ram_init0
		fcb 0

		; print our startup message

		code

		andcc #$af
		ldx #s_copyright-1
		jsr bas_print_string

		data

s_copyright
		fcc "BSDOS 0.1",13
		fcc "(C) 2026 CIARAN ANSCOMB",13
		;fcc "LICENSE: GNU GPL V3 OR LATER",13
		fcb 13,0

		code

		ldx #sys_error_hook
		stx bas_hook_sys_error+1
		lda #$7e
		sta bas_hook_sys_error

		; initialise device
		jsr dev_init

		code

		; set up soft reset vector
		ldx #reset_vector
		stx bas_restart_vector
		lda #$55
		sta bas_restart_flag

		; all done, pass control to BASIC
		jmp bas_ok_prompt

; Soft reset vector handler.  Not much to do right now but there will be.

reset_vector
		nop
		jmp bas_warm_start+1	; skip its NOP

; IRQ handler

irq_service
		ldd >timer
		beq 10F
		subd #1
		std >timer
10		jmp bas_svc_IRQ

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		code

sn_error
		jmp bas_sn_error

tok_handle
		cmpa #$ff
		beq sn_error
		suba #$ce
		bmi sn_error
		cmpa #nwords
		blo 10F
		jmp [bas_stub2_tok_handle]
10		ldx #word_dispatch
		jmp bas_tok_handle

ftok_handle
		subb #bas_nfwords*2
		bmi sn_error
		cmpb #nfwords*2
		blo 10F
		jmp [bas_stub2_ftok_handle]
10		ldx #fword_dispatch
		jmp bas_ftok_handle

nr_error
		ldb #nr_error_code
		fcb $8c		; skip byte
ld_error
		ldb #ld_error_code
		fcb $8c		; skip byte
db_error
		ldb #db_error_code
		fcb $8c		; skip byte
bt_error
		ldb #bt_error_code
		fcb $8c		; skip byte
dn_error
		ldb #dn_error_code
		fcb $8c		; skip byte
tf_error
		ldb #tf_error_code
		fcb $8c		; skip byte
qq_error
		ldb #qq_error_code
sys_error_hook
		stb last_err		; stash error code
		jsr _CASOFF
		jsr bas_audio_off
		jsr bas_reset_stack
		clr DEVNUM
		ldx bas_lno
		stx last_erl
		jsr bas_dev_init_start_line
		jsr bas_print_question
		ldx #bas_errcode_table
		ldb last_err
		bpl 10F
		ldx #errcode_table-128
10		jmp bas_print_error

		data

nwords		equ 2

words
		fcs "DRIVE"
		fcs "BOOT"

word_dispatch
		fdb word_DRIVE
		fdb word_BOOT

nfwords		equ 4

fwords
		fcs "ERL"
		fcs "ERR"
		fcs "HIMEM"
		fcs "FRE$"

fword_dispatch
		fdb fword_ERL
		fdb fword_ERR
		fdb fword_HIMEM
		fdb fword_FRE

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	
		code

word_DRIVE	beq sn_error
		jsr bas_Get8Bit
		cmpb ndrives
		bhs dn_error
		stb cur_drive
		rts

; XXX including a buffered version of BOOT just to check that buffers are
; ejected properly, etc.  Final product should not include this!

word_BOOT
		beq 5F
		jsr bas_Get8Bit
		bra 10F
		; XXX FILES close ALL files
		lbsr eject_all_buffers
5		ldb cur_drive
10		cmpb ndrives
		bhs dn_error
		;
		ldy #op_buffer
		stb buf_drive,y
		; start with LBA 1
		clra
		clrb
		std buf_lba,y
		ldb #1
		std buf_lba+2,y
		ldx #0x2600
		stx buf_data_ptr,y
		clrb		; read
		jsr [_dev_xfer_lba]
		; check for boot signature
		ldd ,x
		cmpd #"OS"
		bne bt_error
		; read up to and including LBA 8
20		leax 512,x
		stx buf_data_ptr,y
		ldb buf_lba+3,y
		cmpb #8
		lbhs 0x2602	; dispatch
		incb
		stb buf_lba+3,y
		clrb		; read
		jsr [_dev_xfer_lba]
		bra 20B

; ERL, ERR not much use until we implement ON ERROR GOTO, but they're easy
; so do them now.

fword_ERL	ldd last_erl
		bra 10F
fword_ERR	clra
		ldb last_err
		bra 10F
fword_HIMEM	ldd bas_ram_top
10		jmp bas_GIVABF
fword_FRE	jsr bas_garbage_collect
		ldd bas_string_top
		subd bas_stack_top
		bra 10B

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; XXX think about which of these should be helpers and which should
; generate errors

buf_state_locked	equ 0x80
buf_state_used		equ 0x02
buf_state_dirty		equ 0x01	; conveniently also xfer write op

dirty_buffer
		ldb #buf_state_dirty
		bra 97F

lock_buffer
		ldb #buf_state_locked
97		orb buf_state,y
98		stb buf_state,y
99		rts

unlock_buffer
		ldb #~buf_state_locked
		andb buf_state,y
		bra 98B

; XXX temp dupe of comment above
; A buffer should be locked while its contents are not final.  An unlocked
; buffer is eligible to be ejected if needed.  Ejecting will write the
; block if marked as dirty.  The highest LRU is ejected first.
;buf_state	rmb 1	; 0: empty, b7: locked, b0: dirty

buffer_lba
		pshs a,u
		bsr find_buffer
		bne 50F
		; no buffer found, check the list for empty or ejectable
		ldu #buf_list
10		pulu y
		lda buf_state,y
		beq 30F
		bpl 20F
		; locked - loop
		cmpu #buf_list+nbuffers*2
		blo 10B
		lbra qq_error
		; unlocked buffer found - eject it
20		bsr eject_buffer_y
		; empty buffer found - use it
30		ldb op_drive
		stb buf_drive,y
		ldd op_lba
		std buf_lba,y
		ldd op_lba+2
		std buf_lba+2,y
		clrb		; read
		jsr [_dev_xfer_lba]
		; fall through
		; matching buffer found
50		bsr make_mru
		ldx buf_data_ptr,y
		puls a,u,pc

; Make buffer in Y the most-recently-used (e.g. we just read it)
make_mru
		pshs a,b,x,u
		ldx #buf_list
		leau ,x
10		ldd ,x
		cmpy ,x++
		beq 20F
		std ,u++
20		cmpx #buf_list+nbuffers*2
		blo 10B
		; XXX assert U == X-2
		sty ,u
		puls a,b,x,u,pc

; Make buffer in Y the least-recently-used (e.g. we just ejected it)
make_lru
		pshs a,b,x,u
		ldx #buf_list+nbuffers*2
		leau ,x
10		ldd ,--x
		cmpy ,x
		beq 20F
		pshu d
20		cmpx #buf_list
		bhi 10B
		; XXX assert U == X+2
		pshu y
		puls a,b,x,u,pc

; Find buffer for configured LBA.
;
; On return, if buffer found, Z is clear (NE), Y points to buffer.
;
; Else, Z is set (EQ), B and Y are undefined.

find_buffer
		pshs b,x,u
		ldu #buf_list
10		pulu y
		ldb buf_state,y
		beq 50F
		ldb op_drive
		cmpb buf_drive,y
		bne 50F
		ldx op_lba
		cmpx buf_lba,y
		bne 50F
		ldx op_lba+2
		cmpx buf_lba+2,y
		bne 50F
		; non-empty buffer with the block we want
		; Z will be clear
		clrb
		puls b,x,u,pc
		; loop
50		cmpu #buf_list+nbuffers*2
		blo 10B
		; Z will be set
		puls b,x,u,pc

eject_buffer
		bsr find_buffer
		lbeq qq_error
eject_buffer_y
		ldb buf_state,y
		beq 99F
		andb #1
		beq release_buffer_y	; not dirty - release it
		; b = 1: write
		jsr [_dev_xfer_lba]
		bra release_buffer_y
release_buffer_lba
		bsr find_buffer
		bne release_buffer_y
99		rts
release_buffer_y
		clr buf_state,y
		bra make_lru

eject_all_buffers
		; initialise buffers
		pshs a,b,x,y,u
		ldy #buf
		ldu #bufdata
		ldx #buf_list
10		bsr eject_buffer_y
		stu buf_data_ptr,y	; data pointer
		sty ,x++
		leau 512,u
		leay sizeof_buf,y
		cmpy #buf_end
		blo 10B
		puls a,b,x,y,u,pc

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

dn_error_code	equ 40			; bad device number
nr_error_code	equ 3			; not ready

		data
errcode_table
ld_error_code	equ 128+(*-errcode_table)
		fcc /LD/		; lost data
db_error_code	equ ld_error_code+1	; device busy (string straddles LD and BT)
bt_error_code	equ 128+(*-errcode_table)
		fcc /BT/		; BOOT error (no "OS" signature)
tf_error_code	equ 128+(*-errcode_table)
		fcc /TF/		; too many files open
qq_error_code	equ 128+(*-errcode_table)
		fcc /??/		; internal error
num_error_code	equ (*-errcode_table)/2

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

; Device-specific code
;
; dev_init
;
; Sets up device.
;
; dev_xfer_lba
;
; Read or write sector.  On entry, Y must point to a buffer, with
; buf_drive, buf_lba and buf_data_ptr all valid.  B contains the command:
; 0 to read, else write.  Will invoke a BASIC error on failure, so no need
; to check returns.  Otherwise, X will point to the byte after the read
; buffer, and buf_state will be set to 2 (valid, not dirty).

		include "bsdos/idelba.s"

; - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		section "work"
		align 256
work_end
work_end_page	equ work_end>>8
sizeof_work	equ *-work_start

		code
code_end
sizeof_code	equ *-code_start

		data
data_end
sizeof_data	equ *-data_start
	if !NOALIGN
		align 256,$ff
	endif
		end
