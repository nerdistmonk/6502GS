        ORG   $1000
]A      =     5
        DFB   ]A
        LDA   #]A
]A      =     ]A+1
        DFB   ]A,]A*2
]B      EQU   7
        DFB   ]B
        LDA   ]B,X
        LDA   ]F
]F      =     $10
        LDA   ]F
        DFB   ]G
]G      =     9
]G      =     10
        DFB   ]G
        LDY   #0
]J      INX
        BNE   ]J
        JMP   ]J
]P      NOP
        LDA   ]P
        DO    ]A
        NOP
        FIN
        END
