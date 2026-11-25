         ORG $1000
         LDA !$10
         LDA |$1000
         LDA !$10,X
         LDA !$1234
         JMP !$10
         LDA ##5
         LDA #{1+2*3}
         BRK
