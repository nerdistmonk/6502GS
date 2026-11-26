PV    LDA #1
      LDA PV
      LDA PV,X
      JMP (PV)
CONST EQU $10
      LDA CONST
      RTS
