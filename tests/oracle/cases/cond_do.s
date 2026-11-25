        ORG   $1000
        MX    %00
        DO    1
        NOP
        ELSE
        INX
        FIN
        DO    0
        LDA   #1
        ELSE
        LDA   #2
        FIN
        DO    0
        DO    1
        LDA   #3
        FIN
        LDY   #4
        ELSE
        LDY   #5
        FIN
        DO    0
LBL     NOP
X       EQU   5
        FIN
        DO    FWD
        DEY
        FIN
        DO    1/2
        TAX
        ELSE
        TAY
        FIN
        DO    0
        DO    0
        ELSE
        INY
        FIN
        ELSE
        INY
        FIN
        IF    MX/3
        ASL
        FIN
        IF    MX&1
        ASL
        ELSE
        LSR
        FIN
        DO    FWD
        NOP
        FIN
FWD     EQU   1
        DO    MX
        PHA
        FIN
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        DO    1
        PLA
        FIN
        FIN
        FIN
        FIN
        FIN
        FIN
        FIN
        FIN
        FIN
        FIN
        NOP
        END
