         ORG $8000
         DB {2*3&1}
         DB 2*3&1
         DB {8-2&3}
         DB 8-2&3
         DB {1+1=2}
         DB 1<2
         DB 3>2
         DB 3#3
         DB {1+2*3}
EQ1      EQU 1+2*3
         DB EQ1
         LDA #EQ1
ABC      EQU 2
         DB ABC+2*3
]V       = 2+3*4
         DB ]V
         DB $-10
         DB $-10+1
         LDA #$-10
         LDA #{1+2*3}
         LDA ##5
         BRK
