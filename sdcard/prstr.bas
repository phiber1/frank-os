' prstr.bas — run a hand-written ARM assembly routine that prints a
' string to the screen.  The CSUB below was produced by the FRANKOS
' native assembler:  asm prstr.s -c hello
'
' MMBasic passes CSUB args by reference, so the CSUB receives &ct and
' dereferences it to reach the CallTable, then calls MMPrintString
' (CallTable[8], byte offset 32).
OPTION DEFAULT INTEGER
ct = MM.INFO(CALLTABLE)
CLS
PRINT "Calling machine code..."
PRINT
hello ct
PRINT
PRINT "^ that line was printed by hand-written Thumb assembly."
PRINT "Press any key to exit."
DO : LOOP UNTIL INKEY$ <> ""
END

CSUB hello integer, integer, integer, integer
  00000000
  6800B500 A0016A01 BD004788 4C4C4548
  5246204F 41204D4F 4D455353 21594C42
  00000A0D
END CSUB
