         ORG $1000
         LST OFF
         NOP
         LST ON
         PAG
         TR ON
         EXP OFF
M1       MAC
         PAG
         TR ON
         EXP OFF
         LST
         LST ON
         NOP
         EOM
M2       MAC
         LST OFF
         NOP
         INY
         EOM
M3       MAC
         LST OFF
         PAG
         LST ON
         EOM
         M1
         M2
         M2
         M3
         NOP
         LUP 2
         LST OFF
         NOP
         --^
         LUP 2
         M2
         --^
         LST ON
         NOP
         END
