 ORG $2000
 DO 0
 LDA #$02 ; in false do
 FIN
MAC1 MAC
 LDA #$03 ; in macro body
 <<<
 MAC1 ; invoke comment
; between
 RTS
