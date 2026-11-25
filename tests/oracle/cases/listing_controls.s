         ORG $1000
         LST OFF
         NOP
         LST ON
         PAG
         TR ON
         EXP OFF
         ERR 0
         ERR FWD-FWD
         ERR 3-3
         LDA #1
FWD      NOP
         ERR FWD&0
         END
