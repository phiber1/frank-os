' invkill.bas — A/B harness for the invader-kill sound effect
'
' "V" voice freq encoding: integer part = gate rate in Hz; the
' fraction * 10000 sets the modulator.  Below 5000 = rising-ramp
' sweep starting there; 5000+ = back-and-forth mode, the mod freq
' swinging +/-50% around (value-5000) Hz with a gently climbing
' center.  12.62 = 12Hz gate, wobble around 1200Hz.

DIM kv(6), ms, md, k$
kv(6) = 15 : kv(5) = 12 : kv(4) = 10 : kv(3) = 7 : kv(2) = 5 : kv(1) = 3
ms = 200 : md = 0

PRINT "Invader-kill sound A/B"
PRINT "  1     FRANKOS take (rising ramp from 200Hz)"
PRINT "  2     Taito take   (wobble around 1200Hz)"
PRINT "  3     game-exact form: literal 12.7 (A/B against 2)"
PRINT "  + / - value up/down 100Hz"
PRINT "  SPACE replay   Q quit"
PRINT
Fire

DO
  k$ = UCASE$(INKEY$)
  IF k$ = "1" THEN md = 0 : ms = 200 : Fire
  IF k$ = "2" THEN md = 1 : ms = 1200 : Fire
  IF k$ = "3" THEN FireLit
  IF k$ = "+" AND ms < 4800 THEN ms = ms + 100 : Fire
  IF k$ = "-" AND ms > 100 THEN ms = ms - 100 : Fire
  IF k$ = " " THEN Fire
  IF k$ = "Q" THEN END
LOOP

SUB Fire
  LOCAL f
  IF md THEN PRINT "wobble center"; ms; "Hz" ELSE PRINT "ramp start"; ms; "Hz"
  FOR f = 6 TO 1 STEP -1
    PLAY SOUND 4, "B", "V", 12 + (ms + 5000 * md) / 10000, kv(f)
    PAUSE 40
  NEXT
  PLAY SOUND 4, "B", "O"
END SUB

' the game's exact form: literal 12.7, everything else identical
SUB FireLit
  LOCAL f
  PRINT "literal 12.7 (game form)"
  FOR f = 6 TO 1 STEP -1
    PLAY SOUND 4, "B", "V", 12.7, kv(f)
    PAUSE 40
  NEXT
  PLAY SOUND 4, "B", "O"
END SUB
