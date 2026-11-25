         ORG $0800
START    LDX #$03
LOOP     DEX
         BNE LOOP
         BEQ DONE
         JMP START
DONE     LDA DATA
         STA DATA,X
         RTS
DATA     DFB $11
