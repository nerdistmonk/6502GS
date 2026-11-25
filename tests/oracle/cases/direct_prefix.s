LBL     EQU   $0E
ABS     EQU   $1234
        ORG   $1000
        LDA   <LBL
        LDA   <LBL,X
        LDA   <ABS
        LDA   <ABS,X
        LDX   <LBL,Y
        STA   <LBL
        JMP   <LBL
        JMP   <ABS
        JSR   <ABS
        LDA   <FWD
        ORG   $2000
FWD     NOP
