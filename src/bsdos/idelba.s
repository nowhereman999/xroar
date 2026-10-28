; IDE driver code.  LBA mode only.
;
; LBA 0 is the MBR, and may be contained within an initial floppy disk
; image of up to 2048 (512-byte) sectors.  Its maximum size is determined
; by the lowest partition start address up to that maximum.
;
; LSNs 180 and 144 are read.  If they match and validate as DragonDOS with
; nsectors == 18, a single-sided floppy image is assumed and these are
; recorded as the directory (primary, backup) LSNs.
;
; Otherwise, LSNs 360 and 288 are read.  If they match and validate as
; DragonDOS with nsectors == 36, a double-sided floppy image is assumed
; and these values are recorded instead.

IDE_BASE	equ 0xFF50
IDE_DATA	equ IDE_BASE+0
IDE_ERROR_R	equ IDE_BASE+1
IDE_FEATURE_W	equ IDE_BASE+1
IDE_SEC_COUNT	equ IDE_BASE+2
IDE_SEC_NUM	equ IDE_BASE+3
IDE_LBA_LOW	equ IDE_BASE+3
IDE_CYL_LOW	equ IDE_BASE+4
IDE_LBA_MID	equ IDE_BASE+4
IDE_CYL_HI	equ IDE_BASE+5
IDE_LBA_HI	equ IDE_BASE+5
IDE_DEV_HEAD	equ IDE_BASE+6
IDE_LBA_TOP	equ IDE_BASE+6
IDE_STATUS_R	equ IDE_BASE+7
IDE_COMMAND_W	equ IDE_BASE+7
IDE_ALTST_R	equ IDE_BASE+8
IDE_DEVCTRL_W	equ IDE_BASE+8
IDE_DATA_LATCH	equ IDE_BASE+9

		code

dev_init
		clrb
		bsr ide_identify
		bne 99F
		lda #'0'
		bsr print_devname
		ldb #1
		stb ndrives
		bsr ide_identify
		bne 99F
		lda #'1'
		bsr print_devname
		inc ndrives
99		rts

; print character in A, a colon, then the swapped bytes of the drive name
print_devname
		jsr bas_OUTCH
		ldx #s_colon_ide_colon-1
		jsr bas_print_string
		; strip trailing spaces accounting for asinine byte swaps
		ldx #bufdata+94
10		ldd ,--x
		cmpa #0x20
		bne 20F
		clr ,x
		cmpb #0x20
		bne 20F
		clr 1,x
		cmpx #bufdata+54
		bhi 10B
		; print identifier accounting for byte swaps
20		ldx #bufdata+54
		ldb #20
30		lda 1,x
		jsr bas_OUTCH
		lda ,x++
		jsr bas_OUTCH
		decb
		bne 30B
		lda #13
		jmp bas_OUTCH

		setdp 0xFF

; entry: B = drive number
ide_identify
		pshs dp
		lda #0xFF
		tfr a,dp
		lslb
		lslb
		lslb
		lslb
		orb #0xA0
		stb IDE_LBA_TOP
		ldd #100	; 2000ms @ 50Hz
		std timer
		ldb IDE_STATUS_R
		beq 98F
		incb
		beq 98F
		bsr ide_wait_nbusy
		bne 99F
		; At this point DRQ should be low
		ldd #0x0800
		bsr ide_wait_op
		bne 99F
		bsr ide_wait_nbusy
		bne 99F
		bsr ide_wait_drdy
		bne 99F
		ldb #0xEC	; Identify
		stb IDE_COMMAND_W
		bsr ide_wait_drq
		bne 99F
		ldx #bufdata	; ok to trample this during initialisation
		lbsr devide_read_data
		bsr ide_wait_nbusy
		bne 99F
		ldb IDE_STATUS_R
		andb #1
		bne 99F
		bsr ide_wait_drdy
		bne 99F
		ldb bufdata+99
98		comb
		bitb #2
99		puls dp,pc

; wait helpers clearly destroy A, B
ide_wait_drq	ldd #0x0808
		bra ide_wait_op
ide_wait_drdy	ldd #0x4040
		bra ide_wait_op
ide_wait_nbusy	ldd #0x8000
; entry: A = mask, B = test value, must have set up timer
; exit: Z set (EQ) on success
ide_wait_op	pshs a,b
10		ldb IDE_STATUS_R
		andb ,s
		cmpb 1,s
		beq 99F
		ldd timer
		bne 10B
		decb		; indicate failure
99		puls a,b,pc

		setdp 0xFF

dev_xfer_lba
		pshs a,b,dp,x		; b = op: 0 = read, 1 = write
		lda #0xFF
		tfr a,dp
		bsr ide_wait_nbusy
		lbne db_error
		ldb buf_drive,y
		lslb
		lslb
		lslb
		lslb
		orb #0xE0
		pshs b
		stb IDE_LBA_TOP
		bsr ide_wait_nbusy
		lbne db_error
		puls b
		orb buf_lba,y
		lda buf_lba+1,y
		std IDE_LBA_HI	; hi & top, little-endian
		ldb buf_lba+2,y
		lda buf_lba+3,y
		std IDE_LBA_LOW	; low & mid, little-endian
		bsr ide_wait_drdy
		lbne nr_error
		ldb 1,s
		lslb
		lslb
		lslb
		lslb
		orb #0x20
		stb IDE_COMMAND_W
		bsr ide_wait_drq
		lbne bas_io_error
		ldx buf_data_ptr,y
		ldb 1,s
		bne 10F
		; read
		bsr devide_read_data
		bra 20F
		; or write
10		bsr devide_write_data
20		bsr ide_wait_nbusy
		lbne db_error
		ldb #2	; not empty, but not dirty
		stb buf_state,y
		puls a,b,dp,x,pc

; read 512 bytes from IDE to X
; exit: clears A
devide_read_data
		pshs a,b,x
		clr ,s		; 256 word counter
10		lda IDE_DATA
		ldb IDE_DATA_LATCH
		std ,x++
		dec ,s
		bne 10B
		puls a,b,x,pc

; write 512 bytes from X to IDE
; exit: clears A
devide_write_data
		pshs a,b
		clr ,s		; 256 word counter
10		ldd ,x++
		stb IDE_DATA_LATCH
		sta IDE_DATA
		dec ,s
		bne 10B
		puls a,b,x,pc

		setdp 0

		data

s_colon_ide_colon	fcc ":IDE:",0
