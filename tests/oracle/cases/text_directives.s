         ORG $2000
         ASC "AB"
         ASC 'AB'
         ASC "Bye,Bye",8D
         DCI 'AB',8D,8D
         DCI "AB",8D
         DCI 'AB'
         DCI "AB"
         STR "HI"
         STR 'HI',8D
         STR 'HI',8D,8D
         STR "/PATH/"
         INV "STOP!"
         INV 'END',8D
         INV 'A',8D,8D
         FLS "AB"
         FLS 'THE END',8D00
         FLS "A",FF
         REV "DISK VOLUME"
         REV 'Insert'
KEY      KBD
         DFB KEY
         KBD
