         ORG $0300
         LDA FWDZ
         LDA FWDZX,X
         STA FWDE
         LDA FWDL
         JMP FWDL
         LDX FWDZ,Y
FWDL     NOP
FWDZ     EQU $12
FWDZX    EQU $20
FWDE     EQU $1234
         END
