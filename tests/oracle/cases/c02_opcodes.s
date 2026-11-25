ZPV     EQU   $20
        ORG   $1000
        BRA   T
T       PHX
        PHY
        PLX
        PLY
        STZ   $10
        STZ   $10,X
        STZ   $1234
        STZ   $1234,X
        TRB   $10
        TSB   $1234
        TRB   $1234
        TSB   $10
        ORA   ($10)
        AND   ($10)
        EOR   ($10)
        ADC   ($10)
        STA   ($10)
        LDA   ($10)
        CMP   ($10)
        SBC   ($10)
        BIT   #$01
        BIT   $10,X
        BIT   $1234,X
        INC   A
        DEC   A
        JMP   ($1234,X)
        LDA   ($10,X)
        WAI
        STP
        LDA   (ZPV)
        JMP   (FWD,X)
        BRA   FWD
FWD     NOP
