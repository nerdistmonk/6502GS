 ORG $2000
; full line comment
* star comment
   ; indented comment
LBL ; label only comment
 ANOP ; opcode only comment
 LDA #$01 ; code comment
 LDA #$01 ;
 LDA #$01 ;  two spaces kept
 DFB 1,2,3,4,5,6,7,8 ; operand 19 chars
 DFB 1,2,3,4,5,6,7,8,9,10,11,12 ; long operand overflows
 DFB 1,2,3,4,5,6,7,8,9,10,11,12
 ASC "a b" ; quoted space
 RTS
