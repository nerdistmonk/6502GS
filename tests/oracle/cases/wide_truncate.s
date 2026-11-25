         ORG $8000
         LDA $12345
         LDA $12345,X
         STA $E10000
         JMP $FFFFFF
         LDA >$1234567
         DA $12345
         DW $12345
         RTS
