        ORG   $1000
MOVE    MAC
        LDA   ]1
        STA   ]2
        EOM
        PMC   MOVE.#$10;$20
        PMC   MOVE,#$11;$21
        PMC   MOVE(#$12;$22
        PMC   MOVE-#$13;$23
        PMC   MOVE/#$14;$24
        >>> MOVE.#$15;$25
        END
