        ORG   $1000
        NOP
        DUM   $2000
AA      DS    2
BB      DS    3
        DW    BB
        ADR   AA
        DEND
        LDA   AA
CC      NOP
        DW    BB
        DEND
        DS    2
        DUM   $3000
        ORG   $4000
        DS    4
        DEND
        DS    2
        DEND
        DUM   $5000
ZZ      NOP
        DEND
        LDA   ZZ
        DW    CC
