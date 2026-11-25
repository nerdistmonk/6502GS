        ORG   $8000
TOP     LDA   >$1234
        LDA   >$1234,X
        LDA   >$10
        STA   >$1234
        JML   $1234
        JML   >$1234
        JSL   $1234
        JML   [$1234]
        JMP   [$1234]
        JMP   >$123456
        LDA   [$10]
        LDA   [$10],Y
        LDA   ($10,S),Y
        ADC   $10,S
        PEA   $10
        PEI   ($10)
        MVN   $01,$02
        MVP   $03,$04
        BRL   TOP
        PER   TOP
        COP   #$01
        WDM   #$01
        JSR   ($1234,X)
        PHB
        PHD
        PHK
        PLB
        PLD
        RTL
        TCD
        TDC
        TCS
        TSC
        XBA
        XCE
        LDX   #$12
        REP   #$30
        LDX   #$1234
        LDA   #$1234
        SEP   #$20
        LDA   #$1234
        REP   #$20
        LDA   #$12
        LDA   #<$1234
        LDA   #>$1234
        LONGA ON
        LONGI ON
        LDA   #$12
        LDX   #$34
