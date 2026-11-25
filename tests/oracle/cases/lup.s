        ORG   $1000
        LUP   4
        ASL
        --^
        LUP   2
        INX
        --^
]A      =     0
        LUP   $0F
]A      =     ]A+1
        DFB   ]A
        --^
]C      =     3
        LUP   ]C
        INY
        --^
        LUP   3
        NOP
        --^
        LUP   2
@       INX
        --^
        LUP   3
@       INY
        --^
        DFB   ozunid_1,ozunid_3
        LDA   ozunid_5
        LUP   0
        NOP
        --^
        DO    0
        LUP   2
        NOP
        --^
        FIN
        LUP   FWD
        NOP
        --^
FWD     EQU   2
        END
