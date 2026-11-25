ZPF2     EQU $12
         ORG $0010
ZPF      DFB $AA
         ORG $0300
         LDA ZPF
         LDA ZPF2,X
         STA ZPF2
         ORG $0400
         ASC "ab",8D
         ASC 'cd',8D
         DCI "ef\8D"
         HEX 0a0b0c
         DS 0
         DS 2,$7
         ORG $0500
         DFB 9
         LDA #ZPF2
         END
