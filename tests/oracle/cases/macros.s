        ORG   $1000
MOVE    MAC
        LDA   ]1
        STA   ]2
        EOM
LOOPY   MAC
        LDX   #]0
LOOP    DEY
        BNE   LOOP
        EOM
TWO     MAC
        MOVE  ]1;]2
        MOVE  ]2;]1
        EOM
        MOVE  #$10;$20
        MOVE  #$11;$21
        LOOPY 1;2
        LOOPY 1
        TWO   $30;$40
        NOP
INNER   MAC
ITOP    INX
        BNE   ITOP
        EOM
OUTER   MAC
        INNER
        LDA   ]1
        EOM
NOARG   MAC
        NOP
        LDA   #5
        EOM
LUPM    MAC
        LUP   2
        ASL
        --^
        EOM
        DO    0
SKIPM   MAC
        LDA   #$77
        EOM
        SKIPM
        FIN
        SKIPM
        OUTER $12
        NOARG
        PMC   NOARG
HERE    NOARG
        LUPM
        LUP   2
@       INY
        --^
        LUPM
        OUTER $34
        LDX   #3
        LUP   2
        NOARG
        --^
        DFB   HERE
TSTM    MAC
T1      LDA   #]1
        BNE   T1
        JMP   FAR
        EOM
GO      MAC
        LDX   ]1
        BRA   SKIP2
SKIP2   NOP
        EOM
        LUP   30
        TSTM  $01
        GO    $02
        --^
        LUP   2
        TSTM  $03
@       INX
        JSR   FAR2
        --^
        DFB   1
        JMP   FAR
        LUP   3
        GO    $44
        --^
FAR     NOP
        DS    $300
FAR2    RTS
        END
