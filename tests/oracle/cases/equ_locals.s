         ORG $0300
BASE     EQU $0400
LEN      = $10
         LDA BASE+LEN
         LDA #LEN
         LDA #<BASE
         LDA #>BASE
         LDA #2+3*5
         LDA #"A"
         LDA #'A'
         LDX *
MAIN     LDA #$01
:LOOP    DEY
         BNE :LOOP
         JMP MAIN
         DFB *-MAIN
         END
         DFB $FF
