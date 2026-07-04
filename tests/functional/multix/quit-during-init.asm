; the test must finish while MULTIX is still initializing - needs the real (long) init delay
; OPTS -c configs/multix.ini -O general:timing=all

	hlt	077

; XPCT rz[12] : 0
; XPCT rz[13] : 0
; XPCT rz[14] : 0
; XPCT rz[15] : 0
; XPCT rz[6] : 0
; XPCT alarm : 0
; XPCT ir : 0xec3f
