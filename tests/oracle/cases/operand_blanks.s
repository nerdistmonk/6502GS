         ORG   $1000
MOVE     MAC
         LDA   ]1
         STA   ]2
         EOM
         DFB   1 2
         LDA   #1 2
         DFB   5	6  ;  c
         ASC   'a b' ; c
         DFB   7 ; a 8
         DFB   1,2 3
         DAT   1 hi
         DAT   1 2
         PMC   MOVE.#1 2;$22
         >>> MOVE.#3 4;$23
         END
