' SPACE INVADERS for FRANKOS MMBASIC (Fruit Jam port)
' Controls: left/right arrows (or A/D) move, SPACE fire, Q quit.
' Uses: SPRITE READ/WRITE, KEYDOWN(), PLAY SOUND channels, PIXEL()
OPTION DEFAULT INTEGER

CONST COLS = 10, ROWS = 5
CONST GW = 40, GH = 26          ' grid spacing
CONST IW = 24, IH = 16          ' invader sprite size
CONST PY = 350                  ' player y

DIM a(COLS - 1, ROWS - 1)       ' alive flags
DIM score, hi, lives, wave, gameover
DIM gx, gy, gdx, frame, beat, beatms, nalive, mnote
DIM plx, sx, sy, sactive        ' player x, player shot
DIM bx(2), by(2), bact(2)       ' bombs
DIM ux, uact, unext             ' ufo
DIM soff1, soff2, soff3, soff4  ' sound-off deadlines

MakeSprites
hi = 0
NewGame

DO
  t = TIMER
  Player t
  Shot
  Bombs t
  Ufo t
  IF NOT gameover THEN
    IF t > beat THEN March t
  ELSE
    IF KeyHeld(ASC(" ")) THEN NewGame
  ENDIF
  IF KeyHeld(ASC("q")) OR KeyHeld(ASC("Q")) THEN
    CLS
    ON ERROR SKIP 1
    PLAY STOP
    END
  ENDIF
  Sounds t
  PAUSE 12
LOOP

' ── input ────────────────────────────────────────────────────────────
FUNCTION KeyHeld(c)
  LOCAL i
  KeyHeld = 0
  FOR i = 1 TO 6
    IF KEYDOWN(i) = c THEN KeyHeld = 1 : EXIT FUNCTION
  NEXT
END FUNCTION

' ── player ───────────────────────────────────────────────────────────
SUB Player t
  LOCAL nx
  IF gameover THEN EXIT SUB
  nx = plx
  IF KeyHeld(&H82) OR KeyHeld(ASC("a")) THEN nx = plx - 4
  IF KeyHeld(&H83) OR KeyHeld(ASC("d")) THEN nx = plx + 4
  IF nx < 8 THEN nx = 8
  IF nx > 600 THEN nx = 600
  IF nx <> plx THEN
    BOX plx, PY, 30, 16, , 0, 0
    plx = nx
    SPRITE WRITE 7, plx, PY
  ENDIF
  IF KeyHeld(ASC(" ")) AND sactive = 0 THEN
    sactive = 1 : sx = plx + 14 : sy = PY - 8
    PLAY SOUND 2, "B", "Q", 1400, 12
    soff2 = t + 70
  ENDIF
END SUB

' ── player shot ──────────────────────────────────────────────────────
SUB Shot
  LOCAL c, r, ix, iy
  IF sactive = 0 THEN EXIT SUB
  BOX sx, sy, 2, 8, , 0, 0
  sy = sy - 10
  IF sy < 24 THEN sactive = 0 : EXIT SUB
  ' shield collision (shields are green pixels)
  IF PIXEL(sx, sy + 4) <> 0 AND sy > 290 THEN
    BOX sx - 3, sy, 8, 8, , 0, 0
    sactive = 0
    EXIT SUB
  ENDIF
  ' invader collision
  c = (sx - gx) \ GW : r = (sy - gy) \ GH
  IF c >= 0 AND c < COLS AND r >= 0 AND r < ROWS THEN
    ix = gx + c * GW : iy = gy + r * GH
    IF a(c, r) AND sx >= ix AND sx < ix + IW AND sy >= iy AND sy < iy + IH THEN
      a(c, r) = 0 : nalive = nalive - 1
      BOX ix, iy, IW, IH, , 0, 0
      score = score + (ROWS - r) * 10
      Header
      PLAY SOUND 3, "B", "N", 900, 18
      soff3 = TIMER + 120
      sactive = 0
      IF nalive = 0 THEN NextWave
      EXIT SUB
    ENDIF
  ENDIF
  ' ufo collision
  IF uact AND sy < 34 AND sx >= ux AND sx < ux + 24 THEN
    BOX ux, 24, 24, 10, , 0, 0
    uact = 0 : score = score + 50 + 50 * INT(RND * 5)
    Header
    PLAY SOUND 3, "B", "N", 400, 20
    soff3 = TIMER + 250
    sactive = 0
    EXIT SUB
  ENDIF
  BOX sx, sy, 2, 8, , RGB(WHITE), RGB(WHITE)
END SUB

' ── invader march ────────────────────────────────────────────────────
SUB March t
  LOCAL c, r, x, y, edge, drop
  ' erase all alive at current position
  FOR r = 0 TO ROWS - 1
    FOR c = 0 TO COLS - 1
      IF a(c, r) THEN BOX gx + c * GW, gy + r * GH, IW, IH, , 0, 0
    NEXT
  NEXT
  ' find edges and move
  edge = 0 : drop = 0
  IF gdx > 0 THEN
    FOR c = COLS - 1 TO 0 STEP -1
      IF ColAlive(c) THEN
        IF gx + c * GW + IW + gdx > 630 THEN edge = 1
        EXIT FOR
      ENDIF
    NEXT
  ELSE
    FOR c = 0 TO COLS - 1
      IF ColAlive(c) THEN
        IF gx + c * GW + gdx < 2 THEN edge = 1
        EXIT FOR
      ENDIF
    NEXT
  ENDIF
  IF edge THEN
    gdx = -gdx : gy = gy + 12 : drop = 1
  ELSE
    gx = gx + gdx
  ENDIF
  frame = 1 - frame
  ' redraw
  FOR r = 0 TO ROWS - 1
    FOR c = 0 TO COLS - 1
      IF a(c, r) THEN
        SPRITE WRITE SprFor(r) + frame, gx + c * GW, gy + r * GH
        IF gy + r * GH + IH >= PY THEN gameover = 1   ' invasion!
      ENDIF
    NEXT
  NEXT
  ' march thump: two alternating notes, faster as they die
  mnote = 1 - mnote
  PLAY SOUND 1, "B", "Q", 55 + 25 * mnote, 14
  soff1 = t + 60
  beatms = 60 + nalive * 11
  beat = t + beatms
  IF gameover THEN GameOver
END SUB

FUNCTION ColAlive(c)
  LOCAL r
  ColAlive = 0
  FOR r = 0 TO ROWS - 1
    IF a(c, r) THEN ColAlive = 1 : EXIT FUNCTION
  NEXT
END FUNCTION

FUNCTION SprFor(r)
  IF r = 0 THEN SprFor = 1
  IF r = 1 OR r = 2 THEN SprFor = 3
  IF r >= 3 THEN SprFor = 5
END FUNCTION

' ── bombs ────────────────────────────────────────────────────────────
SUB Bombs t
  LOCAL i, c, r, bc
  IF gameover THEN EXIT SUB
  ' maybe drop a new bomb
  IF RND < 0.02 + wave * 0.005 THEN
    FOR i = 0 TO 2
      IF bact(i) = 0 THEN
        c = INT(RND * COLS)
        FOR r = ROWS - 1 TO 0 STEP -1
          IF a(c, r) THEN
            bact(i) = 1
            bx(i) = gx + c * GW + IW \ 2
            by(i) = gy + r * GH + IH
            EXIT FOR
          ENDIF
        NEXT
        EXIT FOR
      ENDIF
    NEXT
  ENDIF
  FOR i = 0 TO 2
    IF bact(i) THEN
      BOX bx(i), by(i), 2, 6, , 0, 0
      by(i) = by(i) + 5
      IF by(i) > 380 THEN
        bact(i) = 0
      ELSEIF by(i) > 290 AND by(i) < 330 AND PIXEL(bx(i), by(i) + 6) <> 0 THEN
        BOX bx(i) - 3, by(i) + 2, 8, 8, , 0, 0     ' erode shield
        bact(i) = 0
      ELSEIF by(i) + 6 >= PY AND bx(i) >= plx AND bx(i) <= plx + 30 THEN
        bact(i) = 0
        PlayerHit
      ELSE
        BOX bx(i), by(i), 2, 6, , RGB(YELLOW), RGB(YELLOW)
      ENDIF
    ENDIF
  NEXT
END SUB

SUB PlayerHit
  LOCAL f
  PLAY SOUND 3, "B", "N", 250, 22
  soff3 = TIMER + 400
  FOR f = 1 TO 6
    BOX plx, PY, 30, 16, , RGB(RED), RGB(RED)
    PAUSE 60
    BOX plx, PY, 30, 16, , 0, 0
    PAUSE 40
  NEXT
  lives = lives - 1
  Header
  IF lives <= 0 THEN
    GameOver
  ELSE
    SPRITE WRITE 7, plx, PY
  ENDIF
END SUB

' ── ufo ──────────────────────────────────────────────────────────────
SUB Ufo t
  IF gameover THEN EXIT SUB
  IF uact = 0 THEN
    IF t > unext THEN
      uact = 1 : ux = 2
      PLAY SOUND 4, "B", "T", 640, 8
    ENDIF
    EXIT SUB
  ENDIF
  BOX ux, 24, 24, 10, , 0, 0
  ux = ux + 3
  IF ux > 606 THEN
    uact = 0 : unext = t + 15000 + RND * 10000
    PLAY SOUND 4, "B", "O", 0
  ELSE
    RBOX ux, 24, 24, 10, 4, RGB(MAGENTA), RGB(MAGENTA)
    PLAY SOUND 4, "B", "T", 600 + 80 * ((ux \ 8) MOD 2), 8
  ENDIF
END SUB

' ── sound envelopes ──────────────────────────────────────────────────
SUB Sounds t
  ' note: AND is bitwise in MMBasic — keep both operands 0/1
  IF (soff1 > 0) AND (t > soff1) THEN PLAY SOUND 1, "B", "O", 0 : soff1 = 0
  IF (soff2 > 0) AND (t > soff2) THEN PLAY SOUND 2, "B", "O", 0 : soff2 = 0
  IF (soff3 > 0) AND (t > soff3) THEN PLAY SOUND 3, "B", "O", 0 : soff3 = 0
END SUB

' ── game flow ────────────────────────────────────────────────────────
SUB Header
  TEXT 8, 4, "SCORE " + STR$(score) + "   ", , , , RGB(WHITE), 0
  TEXT 250, 4, "HI " + STR$(hi), , , , RGB(CYAN), 0
  TEXT 500, 4, "LIVES " + STR$(lives) + " ", , , , RGB(GREEN), 0
END SUB

SUB Shields
  LOCAL i, x
  FOR i = 0 TO 2
    x = 110 + i * 170
    BOX x, 300, 60, 20, , RGB(GREEN), RGB(GREEN)
    BOX x + 20, 312, 20, 8, , 0, 0        ' notch
  NEXT
END SUB

SUB Grid
  LOCAL c, r
  FOR r = 0 TO ROWS - 1
    FOR c = 0 TO COLS - 1
      a(c, r) = 1
      SPRITE WRITE SprFor(r) + frame, gx + c * GW, gy + r * GH
    NEXT
  NEXT
  nalive = COLS * ROWS
END SUB

SUB NextWave
  wave = wave + 1
  PAUSE 800
  gx = 60 : gy = 50 + 8 * wave : gdx = 8
  IF gy > 120 THEN gy = 120
  Grid
  beat = TIMER + 600
END SUB

SUB GameOver
  gameover = 1
  IF score > hi THEN hi = score
  PLAY SOUND 1, "B", "O", 0
  PLAY SOUND 4, "B", "O", 0
  PLAY SOUND 3, "B", "N", 150, 22
  soff3 = TIMER + 900
  TEXT 316, 170, "  G A M E   O V E R  ", "C", , , RGB(WHITE), RGB(RED)
  TEXT 316, 195, "  hold SPACE to play again  ", "C", , , RGB(WHITE), 0
END SUB

SUB NewGame
  LOCAL i
  CLS
  score = 0 : lives = 3 : wave = 0 : gameover = 0
  sactive = 0 : uact = 0
  FOR i = 0 TO 2 : bact(i) = 0 : NEXT
  gx = 60 : gy = 50 : gdx = 8 : frame = 0
  plx = 300
  Header
  Shields
  Grid
  SPRITE WRITE 7, plx, PY
  unext = TIMER + 12000
  beat = TIMER + 700
END SUB

' ── sprite art ───────────────────────────────────────────────────────
SUB MakeSprites
  LOCAL n
  CLS
  RESTORE Art
  FOR n = 1 TO 7
    DrawArt n
    SPRITE READ n, 0, 0, IW, IH
    BOX 0, 0, IW, IH, , 0, 0
  NEXT
  CLS
END SUB

SUB DrawArt n
  LOCAL r, c, s$, col
  READ col
  FOR r = 0 TO 7
    READ s$
    FOR c = 1 TO 12
      IF MID$(s$, c, 1) = "1" THEN
        BOX (c - 1) * 2, r * 2, 2, 2, , col, col
      ENDIF
    NEXT
  NEXT
END SUB

Art:
' type 1 (top row squid) frame 0
DATA &H0AFF55
DATA "000011110000", "000111111000", "001111111100", "011011110110"
DATA "011111111110", "001011110100", "010000000010", "001000000100"
' type 1 frame 1
DATA &H0AFF55
DATA "000011110000", "000111111000", "001111111100", "011011110110"
DATA "011111111110", "000100001000", "001011110100", "010100001010"
' type 2 (crab) frame 0
DATA &H55FFFF
DATA "001000000100", "000100001000", "001111111100", "011011110110"
DATA "111111111111", "101111111101", "101000000101", "000110011000"
' type 2 frame 1
DATA &H55FFFF
DATA "001000000100", "100100001001", "101111111101", "111011110111"
DATA "111111111111", "001111111100", "001000000100", "010000000010"
' type 3 (octopus) frame 0
DATA &HFF55FF
DATA "000111111000", "011111111110", "111111111111", "111001100111"
DATA "111111111111", "000110011000", "001101101100", "110000000011"
' type 3 frame 1
DATA &HFF55FF
DATA "000111111000", "011111111110", "111111111111", "111001100111"
DATA "111111111111", "001101101100", "010010010010", "001000000100"
' player ship
DATA &H55FF55
DATA "000001000000", "000011100000", "000011100000", "011111111100"
DATA "111111111110", "111111111110", "111111111110", "111111111110"
