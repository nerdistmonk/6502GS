         ORG $1000
         LDA #$01
         LDA $10
         LDA $10,X
         LDA $1234
         LDA $1234,X
         LDA $1234,Y
         LDA ($20,X)
         LDA ($20),Y
         LDA: $10
         ASL
         ASL A
         LSR $10
         JMP $2000
         JMP ($2000)
         JSR $3000
         BIT $10
         STX $10,Y
         LDX $10,Y
         RTS
