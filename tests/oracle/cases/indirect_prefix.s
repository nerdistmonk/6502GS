         ORG $8000
         LDA [>$12]
         LDA [<$12],Y
         LDA (<$12),Y
         LDA (<$12,X)
         LDA (<$12,S),Y
         LDA (>$12)
         JML [>$1000000]
         JMP (>$1000000)
         RTS
