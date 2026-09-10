' CSUB smoke test: machine-code routine called from BASIC.
' Expected output: r = 11, then a timing comparison.
OPTION DEFAULT INTEGER
DIM q = 5, r = 0, i, t1, t2

hello r, q
PRINT "CSUB result: "; r; "   (expect 11)"

' timing: 1000 CSUB calls vs 1000 interpreted equivalents
TIMER = 0
FOR i = 1 TO 1000
  hello r, q
NEXT
t1 = TIMER
TIMER = 0
FOR i = 1 TO 1000
  r = q * 2 + 1
NEXT
t2 = TIMER
PRINT "1000 CSUB calls:   "; t1; " ms"
PRINT "1000 BASIC stmts:  "; t2; " ms"
END

CSUB hello integer, integer
  00000000
  4684460A 3200E9D2 415218DB F1423301
  20000200 F8CC2100 F8CC3000 47702004
END CSUB
