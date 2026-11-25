         ORG $0214
LBL      NOP
         LDA #>LBL+$10
         LDA #<LBL+$10
         DFB >LBL+$10
         DFB <LBL+$10
         DW LBL
         DW <LBL
         DW >LBL
         DA <LBL
         DA >LBL
         DA >$1234
         DA <$1234
         LDA #$1234
         END
