' SPACE INVADERS for FRANKOS MMBASIC (Fruit Jam port)
' Controls: left/right arrows move, SPACE fire, Q quit.
'
' Architecture (the TRS-80 way): BASIC runs input, sound and game flow;
' one CSUB ("itick") moves, draws and collision-tests everything each
' frame with direct 4bpp buffer access — the interpreted draw calls
' that cost ~6ms each are gone.  Source: tools/csub/invaders_tick.c
OPTION DEFAULT INTEGER

CONST COLS = 10, ROWS = 5
CONST GW = 40, GH = 26          ' grid spacing
CONST IW = 24, IH = 16          ' invader sprite size
CONST PY = 336                  ' player y (raised toward barriers)

' st() indices (mirror invaders_tick.c)
' 0 cmd | 1 gx | 2 gy | 3 dx | 4 dy | 5 anim | 6 sx | 7 sy | 8 ux
' 9 uspd | 10 plx | 11 events | 12 hc | 13 hr | 14 minc | 15 maxc
' 16-21 bombs x/y | 22 white nibble | 23 yellow nibble
DIM st(23)
DIM al(COLS * ROWS)             ' alive list: al(0)=count, then c+r*16
DIM spr(191)                    ' 8 sprite slots x 192 bytes
DIM a(COLS - 1, ROWS - 1)       ' alive flags (bomb targeting)
DIM idx(COLS - 1, ROWS - 1)     ' cell -> alive-list position
DIM ct                          ' CallTable address
DIM score, hi, lives, wave, gameover, nalive, gdxv
DIM plx, nbomb, mnote, beat, unext, ulast
DIM soff1, soff2, soff3, soff4, soff5, fx3, fx4
' channels: 1 march tone, 2 pish hiss, 3 ufo siren/base death,
' 4 kill ring, 5 pish tone, 6 ufo death siren, 7 march tut —
' each sound owns its voice, nothing stomps
DIM k1, k2, k3, kl, kr, kf
DIM gotime, spup, ubx, uboff, ex, ey, exoff
DIM fcnt, flast                 ' TEMP: FPS meter
DIM udir                        ' ufo pass direction
DIM kv3(6)                      ' invader-kill decay envelope

' invader kill: ~280ms of the "V" ring (12Hz gate), decaying; the
' metallic rise happens INSIDE the voice (ring-mod sweep in the synth)
kv3(6) = 15 : kv3(5) = 12 : kv3(4) = 10
kv3(3) = 7 : kv3(2) = 5 : kv3(1) = 3

' ufo siren: the synth's "R" voice — a square sweeping 700-1500Hz on a
' per-sample triangle LFO at 8 wails/sec.  Deliberately distracting.
ct = MM.INFO(CALLTABLE)
MakeSprites
hi = 0
NewGame

DO
  t = TIMER
  k1 = KEYDOWN(1) : k2 = KEYDOWN(2) : k3 = KEYDOWN(3)
  kl = (k1 = &H82) OR (k2 = &H82) OR (k3 = &H82)
  kr = (k1 = &H83) OR (k2 = &H83) OR (k3 = &H83)
  kf = (k1 = 32) OR (k2 = 32) OR (k3 = 32)
  DO WHILE INKEY$ <> "" : LOOP
  IF NOT gameover THEN
    IF kl OR kr THEN Player
    IF kf AND st(7) = -1 THEN
      st(6) = plx + 10 : st(7) = 1000 + PY - 10
      ' OS-synth noise rates are honest (changes/sec); the old app synth
      ' ran ~10.8x the requested rate, so approved sounds are restated
      ' in real units.  Timers +~50ms: the pump's buffered tail is gone.
      PLAY SOUND 5, "B", "Q", 2800, 15
      soff5 = t + 130
      PLAY SOUND 2, "B", "N", 18000, 11
      soff2 = t + 230
    ENDIF
    IF nbomb < 3 AND RND < 0.04 + wave * 0.01 THEN DropBomb
    IF st(8) < 0 AND t > unext AND nalive > 7 THEN
      IF RND < 0.5 THEN
        udir = 1 : st(8) = 2
      ELSE
        udir = -1 : st(8) = 604
      ENDIF
      ' speed must be set NOW: itick runs before the per-frame speed
      ' update, and a left spawn inheriting a stale negative speed
      ' exited at x<2 on its first tick (the vanishing-UFO bug)
      st(9) = udir * 2
      ulast = -1
      ' the siren wails autonomously in the synth ("R" voice) — one
      ' call per pass, per-sample smooth.  .2123 = rate 8Hz, span
      ' 1200 (sweep 700-1900); vol 18 = the LOUDEST thing on screen,
      ' a totally distracting center of attention, on purpose
      PLAY SOUND 3, "B", "R", 700.2123, 18
    ENDIF
    st(0) = 14                          ' shot+bombs+ufo
    IF t > beat THEN
      MarchPrep : st(0) = 15
      ' the arcade march: four dissonant NE556 tones, 68/63/58/53 Hz,
      ' fired at the beat decision so screen work can't stagger it
      mnote = (mnote + 1) MOD 4
      PLAY SOUND 1, "B", "Q", 68 - 5 * mnote, 4
      ' the low "tut" blended under each tone: a real drum hit ("D"
      ' voice) — sine striking at 4x and falling to 70Hz in ~12ms,
      ' dying in ~70ms.  One-shot; the pitch-drop transient is what
      ' reads as a chest thump (a steady low tone never does)
      PLAY SOUND 7, "B", "D", 85, 24
      soff1 = t + 60
      beat = t + 40 + nalive * 18
    ENDIF
    itick ct, st(), al(), spr()
    IF st(11) THEN HandleEvents t
    IF st(8) >= 0 THEN
      st(9) = udir * (2 + 2 * (fcnt AND 1))   ' 3px/frame avg, signed
    ENDIF
  ELSE
    IF NOT kf THEN spup = 1
    IF spup AND kf AND t > gotime + 600 THEN NewGame
  ENDIF
  IF (k1 = 113) OR (k1 = 81) OR (k2 = 113) OR (k2 = 81) THEN
    CLS
    ON ERROR SKIP 1
    PLAY STOP
    END
  ENDIF
  IF soff1 OR soff2 OR soff3 OR soff4 OR soff5 OR fx3 OR fx4 THEN Sounds t
  IF uboff OR exoff THEN Cleanups t
  ' TEMP: FPS + free-heap meter, top-right (DIAGNOSTIC: watch heap for
  ' a steady decline over waves — that would prove the slow-motion is
  ' interpreter heap exhaustion)
  fcnt = fcnt + 1
  IF t - flast > 999 THEN TEXT 560, 4, STR$(fcnt) + " " + STR$(MM.INFO(HEAP)\1024) + "K ", , , , RGB(128,128,128), 0 : flast = t : fcnt = 0
  PAUSE 1
LOOP

' ── player (BASIC-side: moves are cheap enough to keep here) ─────────
SUB Player
  LOCAL nx
  nx = plx
  IF kl THEN nx = plx - 6
  IF kr THEN nx = plx + 6
  IF nx < 8 THEN nx = 8
  IF nx > 600 THEN nx = 600
  IF nx <> plx THEN
    BOX plx, PY, 24, 16, , 0, 0
    plx = nx : st(10) = plx
    SPRITE WRITE #7, plx, PY
  ENDIF
END SUB

' ── march step setup: edge logic from the CSUB's min/max columns ─────
SUB MarchPrep
  LOCAL e
  e = 0
  IF gdxv > 0 THEN
    IF st(1) + st(15) * GW + IW + gdxv > 630 THEN e = 1
  ELSE
    IF st(1) + st(14) * GW + gdxv < 2 THEN e = 1
  ENDIF
  IF e THEN
    gdxv = -gdxv : st(3) = 0 : st(4) = 12
  ELSE
    st(3) = gdxv : st(4) = 0
  ENDIF
  st(5) = 1 - st(5)
END SUB

' ── CSUB event handling ──────────────────────────────────────────────
SUB HandleEvents t
  LOCAL e, k, m
  e = st(11)
  IF e AND 4 THEN                       ' invader destroyed
    a(st(12), st(13)) = 0
    k = idx(st(12), st(13))
    m = al(0) : al(0) = m - 1
    al(k) = al(m)
    idx(al(k) AND 15, al(k) \ 16) = k
    nalive = nalive - 1
    score = score + (ROWS - st(13)) * 10
    Header
    ' the OFF forces a fresh strike: without it, a kill landing inside
    ' the previous kill's decay inherits its already-ramped (higher)
    ' wobble center instead of restarting at 2000Hz
    PLAY SOUND 4, "B", "O"
    fx3 = 7 : soff3 = 0
    ex = st(1) + st(12) * GW : ey = st(2) + st(13) * GH
    exoff = t + 130
    ' tail-end panic strides
    IF nalive = 11 THEN gdxv = SGN(gdxv) * 12
    IF nalive = 3 THEN gdxv = SGN(gdxv) * 14
    IF nalive = 1 THEN gdxv = SGN(gdxv) * 12
    IF nalive = 0 THEN NextWave
  ENDIF
  IF e AND 8 THEN                       ' ufo destroyed: mystery bonus
    k = INT(RND * 5)
    m = 50 + 50 * k
    IF k = 4 THEN m = 300
    score = score + m
    ubx = st(6) - 24 : IF ubx < 2 THEN ubx = 2
    TEXT ubx, 24, STR$(m), , , , RGB(MAGENTA), 0
    uboff = t + 1200
    unext = t + 25000 + RND * 15000
    Header
    PLAY SOUND 3, "B", "O"
    fx4 = 28
  ENDIF
  IF e AND 64 THEN                      ' ufo escaped
    unext = t + 25000 + RND * 15000
    PLAY SOUND 3, "B", "O"
  ENDIF
  IF e AND 128 THEN                     ' bomb(s) done: recount
    nbomb = 0
    IF st(17) >= 0 THEN nbomb = nbomb + 1
    IF st(19) >= 0 THEN nbomb = nbomb + 1
    IF st(21) >= 0 THEN nbomb = nbomb + 1
  ENDIF
  IF e AND 32 THEN PlayerHit
  IF e AND 16 THEN DoGameOver
END SUB

SUB DropBomb
  LOCAL i, c, r
  FOR i = 0 TO 2
    IF st(17 + i * 2) < 0 THEN
      c = INT(RND * COLS)
      FOR r = ROWS - 1 TO 0 STEP -1
        IF a(c, r) THEN
          st(16 + i * 2) = st(1) + c * GW + 12
          st(17 + i * 2) = st(2) + r * GH + IH
          nbomb = nbomb + 1
          EXIT FOR
        ENDIF
      NEXT
      EXIT FOR
    ENDIF
  NEXT
END SUB

SUB PlayerHit
  LOCAL f, sx0, sy0
  PLAY SOUND 3, "B", "N", 2700, 22
  soff3 = TIMER + 450
  ' starburst/black alternation (CSUB blit cmd); the shot's st slots
  ' are borrowed for the blit args, so save and restore them
  sx0 = st(6) : sy0 = st(7)
  FOR f = 1 TO 6
    st(6) = plx : st(7) = PY : st(12) = 7 : st(13) = 16 : st(0) = 64
    itick ct, st(), al(), spr()
    PAUSE 60
    BOX plx, PY, 24, 16, , 0, 0
    PAUSE 40
  NEXT
  st(6) = sx0 : st(7) = sy0
  lives = lives - 1
  Header
  IF lives <= 0 THEN
    DoGameOver
  ELSE
    SPRITE WRITE #7, plx, PY
  ENDIF
END SUB

SUB Cleanups t
  IF uboff > 0 AND t > uboff THEN BOX ubx, 24, 80, 16, , 0, 0 : uboff = 0
  IF exoff > 0 AND t > exoff THEN BOX ex, ey, 24, 16, , 0, 0 : exoff = 0
END SUB

SUB Sounds t
  IF (soff1 > 0) AND (t > soff1) THEN PLAY SOUND 1, "B", "O" : PLAY SOUND 7, "B", "O" : soff1 = 0
  IF (soff2 > 0) AND (t > soff2) THEN PLAY SOUND 2, "B", "O" : soff2 = 0
  IF (soff3 > 0) AND (t > soff3) THEN PLAY SOUND 3, "B", "O" : soff3 = 0
  IF (soff4 > 0) AND (t > soff4) THEN PLAY SOUND 4, "B", "O" : soff4 = 0
  IF (soff5 > 0) AND (t > soff5) THEN PLAY SOUND 5, "B", "O" : soff5 = 0
  ' invader kill: gated ring ("V" at 12Hz) decaying over ~280ms; the
  ' ring-mod wobbles +/-50% around 2000Hz (.7 = osc mode, 5000+2000),
  ' matched by ear against the real Taito effect.  The FRANKOS take
  ' (rising ramp, plunger attack) is plain 12.
  IF fx3 THEN
    fx3 = fx3 - 1
    IF fx3 = 0 THEN
      PLAY SOUND 4, "B", "O"
    ELSE
      PLAY SOUND 4, "B", "V", 12.7, kv3(fx3)
    ENDIF
  ENDIF
  ' ufo kill: the passing siren's death cry — same wail 500Hz lower
  ' end to end (200-1400), a touch slower (7Hz), fading out over
  ' ~8 full cycles (.1867 = rate 7, span 1200)
  IF fx4 THEN
    fx4 = fx4 - 1
    IF fx4 = 0 THEN
      PLAY SOUND 6, "B", "O"
    ELSE
      PLAY SOUND 6, "B", "R", 200.1867, fx4 \ 2
    ENDIF
  ENDIF
END SUB

' ── game flow ────────────────────────────────────────────────────────
SUB Header
  TEXT 8, 4, "SCORE " + STR$(score) + "   ", , , , RGB(WHITE), 0
  TEXT 250, 4, "HI " + STR$(hi) + "   ", , , , RGB(CYAN), 0
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
  LOCAL c, r, n
  n = 0
  FOR r = 0 TO ROWS - 1
    FOR c = 0 TO COLS - 1
      a(c, r) = 1
      n = n + 1
      al(n) = c + r * 16 : idx(c, r) = n
    NEXT
  NEXT
  al(0) = n : nalive = n
  st(14) = 0 : st(15) = COLS - 1
  ' initial draw: a march with zero delta blits the whole armada
  st(3) = 0 : st(4) = 0 : st(0) = 1
  itick ct, st(), al(), spr()
END SUB

SUB NextWave
  LOCAL w
  wave = wave + 1
  PLAY SOUND 1, "B", "O" : soff1 = 0
  ' service the sound sequencers through the wave pause so the final
  ' kill's contour plays out (killing ch3 here silenced the last kill)
  FOR w = 1 TO 20
    PAUSE 40
    IF soff2 OR soff3 OR soff4 OR soff5 OR fx3 OR fx4 THEN Sounds TIMER
  NEXT
  IF exoff THEN BOX ex, ey, 24, 16, , 0, 0 : exoff = 0
  ' a UFO preempted by the wave end vanishes, and every fresh wave
  ' opens with a fixed 25s UFO-free quiet period
  IF st(8) >= 0 THEN BOX st(8), 24, 24, 10, , 0, 0 : st(8) = -1 : PLAY SOUND 3, "B", "O"
  unext = TIMER + 25000
  st(1) = 60 : st(2) = 50 + GH * wave
  IF st(2) > 172 THEN st(2) = 172
  gdxv = 8
  Grid
  beat = TIMER + 600
END SUB

SUB DoGameOver
  LOCAL i
  gameover = 1
  IF score > hi THEN hi = score
  ' the landing explosion owns the stage: silence every voice and
  ' cancel all pending sound sequencers before it speaks
  FOR i = 1 TO 8
    PLAY SOUND i, "B", "O"
  NEXT
  soff1 = 0 : soff2 = 0 : soff4 = 0 : soff5 = 0
  fx3 = 0 : fx4 = 0
  PLAY SOUND 3, "B", "N", 1600, 22
  soff3 = TIMER + 950
  TEXT 316, 170, "  G A M E   O V E R  ", "C", , , RGB(WHITE), RGB(RED)
  TEXT 316, 195, "  press SPACE to play again  ", "C", , , RGB(WHITE), 0
  gotime = TIMER : spup = 0
END SUB

SUB NewGame
  LOCAL i
  CLS
  score = 0 : lives = 3 : wave = 0 : gameover = 0
  nbomb = 0 : mnote = 0
  st(6) = 0 : st(7) = -1                ' no shot
  st(8) = -1 : st(9) = 2                ' no ufo, speed 2 (slow cruiser)
  FOR i = 0 TO 2 : st(17 + i * 2) = -1 : NEXT
  st(1) = 60 : st(2) = 50 : gdxv = 8 : st(5) = 0
  plx = 300 : st(10) = plx
  Header
  Shields
  BOX 0, 372, 632, 2, , RGB(GREEN), RGB(GREEN)   ' Earth
  Grid
  SPRITE WRITE #7, plx, PY
  unext = TIMER + 25000                 ' 25s quiet period, wave 1 included
  beat = TIMER + 700
END SUB

' ── sprite art: draw on the loading screen, GRAB into the CSUB store ─
SUB MakeSprites
  LOCAL n, bx, by
  bx = 304 : by = 200
  CLS
  TEXT 316, 150, "S P A C E   I N V A D E R S", "C", , , RGB(GREEN), 0
  TEXT 316, 260, "LOADING...", "C", , , RGB(WHITE), 0
  SPRITE SET TRANSPARENT 0
  RESTORE Art
  FOR n = 0 TO 5                        ' 6 invader frames -> slots 0-5
    DrawArt bx, by
    Grab n, bx, by, 16
    BOX bx, by, IW, IH, , 0, 0
  NEXT
  ' player ship -> PicoMite sprite #7 (BASIC-side drawing)
  DrawArt bx, by
  SPRITE READ #7, bx, by, IW, IH
  BOX bx, by, IW, IH, , 0, 0
  ' explosion -> slot 7
  DrawArt bx, by
  Grab 7, bx, by, 16
  BOX bx, by, IW, IH, , 0, 0
  ' ufo -> slot 6
  RBOX bx, by, 24, 10, 4, RGB(MAGENTA), RGB(MAGENTA)
  Grab 6, bx, by, 10
  BOX bx, by, 24, 10, , 0, 0
  ' palette nibbles for the CSUB's own drawing
  BOX bx, by, 4, 4, , RGB(WHITE), RGB(WHITE)
  st(6) = bx : st(7) = by : st(0) = 32
  itick ct, st(), al(), spr()
  st(22) = st(12)
  BOX bx, by, 4, 4, , RGB(YELLOW), RGB(YELLOW)
  st(0) = 32
  itick ct, st(), al(), spr()
  st(23) = st(12)
  BOX bx, by, 4, 4, , 0, 0
END SUB

SUB Grab slot, x, y, h
  st(6) = x : st(7) = y : st(12) = slot : st(13) = h
  st(0) = 16
  itick ct, st(), al(), spr()
END SUB

SUB DrawArt bx, by
  LOCAL r, c, s$, col
  READ col
  FOR r = 0 TO 7
    READ s$
    FOR c = 1 TO 12
      IF MID$(s$, c, 1) = "1" THEN
        BOX bx + (c - 1) * 2, by + r * 2, 2, 2, , col, col
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
' explosion starburst
DATA &HFFFFFF
DATA "010000010010", "001001000100", "100100101001", "000010010000"
DATA "010001000110", "001010101000", "100100010010", "010000100001"

CSUB itick integer, integer, integer, integer
  00000000
  4FF0E92D 34FFF04F B09D4D38 5516E9CD
  93086805 46906CEB F8D5681B EA4F20E0
  F8D10C63 680F30B0 E000F8D2 F8D1931A
  06FE30B8 9419468A F8CD9418 F8CDE050
  931BC054 81CFF100 0B20F017 81C2F040
  0340F017 F0409303 688B8189 0B01F017
  690B9309 F8D8930A 930C3000 80B5F000
  2B00698A 6A0A920D 920E9405 8254F340
  9304230A E9CD6A8B E9CD5710 930F1812
  AB149C03 8018F8CD F8CD930B 9A06B01C
  F852211A 9D153F08 070FF003 9A099206
  0987EB07 09C9EB02 0869EA4F 0E0CF108
  BFA845AE 9A0A46AE FB01111B 99142603
  78E8EA28 1106FB05 F0294630 F1060B01
  44710C10 0A0EEBA8 BF00E006 000186A0
  45603001 D00E4429 7FF0F5B0 45F0D2F8
  EB0ADAF6 F8020201 428A4B01 3001D1FB
  44294560 9A16D1F0 0017F10B 9A174593
  F8CDBFB8 4296B058 BFB89A18 42909617
  F1069A19 BFC8010F 42919018 9119BFC8
  F0002B00 2B0281A4 BFD49B0F 1D1A1C9A
  9B0E2110 441E9101 99089B0D 980B444B
  F0009600 F5B6FAB5 9A037FA0 2210BFA8
  9A049203 42BA9B07 463ABFA8 9A059204
  42BA3301 463ABFB8 9A0C9205 429A9307
  E9DDDA85 E9DD5710 9A09A812 990A9B0D
  9B0E441A B00CF8DD 9B044419 F8CA9209
  17DB3070 3074F8CA 910A9B05 3078F8CA
  F8CA17DB 17D3307C 300CF8CA F8CA17CB
  F8CA2008 F8CA1010 077C3014 4654D547
  3050F8DA 5704E9CD F10A9303 F8D40930
  2E006088 2306DB36 F8D42000 46327080
  3000E9CD 23024639 1DB5A814 F9FEF000
  7FB7F5B5 813DF280 718BF46F 2B2C1873
  4639D807 020CF106 FA3AF000 F0402800
  F5B5813F DB067FA5 42BB9B03 3317DC03
  F28042BB 23068144 9B1B9300 93014639
  2302462A F000A814 F8C4F9D9 17ED5088
  508CF8C4 454C3410 E9DDD1C1 073A5704
  E9DAD504 2B006310 80EEF280 F14007BB
  E9DA8082 F1B3630E BF083FFF 3FFFF1B6
  1CB7D079 9030F8DA 816DF000 7F7AF5B6
  8145F280 27082300 AB149301 930B4618
  23024632 97004649 040EF1A6 F9A6F000
  F3402C0B F5A68144 2B1C7399 8104F240
  454A9A09 8162F300 42A1990A 815EF300
  0602EBA9 7FC8F5B6 8158F280 2F811A67
  8154F300 FBB62328 FB03F6F3 F1032306
  454A0217 814AF2C0 FBB7221A FB02F7F2
  F1021207 42A1010F 8140F2C0 210146C4
  4281980C 813AF300 8F08F85C F0083101
  42B0000F EBB7D1F4 D1F11F28 22109200
  92019908 2207980B F9C2F000 73E8EA4F
  6060F8CA F04F17F6 F8CA34FF F8CA7068
  F8CA6064 F04B306C E0D90B04 A8146E8B
  F8DA9301 99083038 F8DA9300 F8DA3030
  F0002060 9A18F9A5 73EBEA4F E9CA2A00
  DB1AB316 3015E9DD 0F43EBB2 0643EA4F
  70E0EA20 F106BFA8 428232FF 9C199917
  F240DB0B 42A313DF 4623BFA8 71E1EA21
  DC024299 40DCF8D5 200047A0 B01D2100
  8FF0E8BD A8146B8A F0006B09 17C3F961
  0318E9CA F04FE7F1 22C00800 6E0B4640
  99086E8D 6038F8DA 1303FB02 F70CFB06
  0904EB03 3030F8DA 0A63EA4F 040CF10A
  EB094653 E00E0208 7FF0F5B6 2B00D20D
  459CDB0B 0107EB0E 5CC9DD07 42A33301
  1F01F802 4285D007 2100DCEE 42A33301
  1F01F802 3001D1F7 F1082810 4467080C
  0601F106 E7B8D1DC 240A2300 23189301
  461A4631 9400A814 F8D0F000 3048F8DA
  1EB3441E 7F17F5B3 F04FD974 F04B36FF
  F8CA0B40 17F66040 6044F8CA 9A0FE6F6
  2300E65E 23029301 72BAF44F 1EB99300
  F0002304 F04FF8B3 F04B35FF E6D40B80
  93012300 F1062308 1F390208 9300A814
  F8A4F000 35FFF04F 0B80F04B F04FE6C5
  F04B35FF E6C00BA0 46494622 F8E0F000
  9F0BB978 46384649 020AF1A6 F8D8F000
  4649B938 1FF24638 F8D2F000 F43F2800
  4622AEE7 24002310 E9CD980B F1A93400
  23080104 F87AF000 34FFF04F 0B02F04B
  E9CA17E3 E72E430E 9304230A 2308E635
  9B1A9300 767AF5A6 46499301 46322302
  F000A814 17F3F863 630EE9CA 9B1AE71B
  93014649 23022214 F0009700 F06FF857
  E7DD0401 22062318 99089300 94014633
  F8AEF000 2300E785 46492208 2300E9CD
  2302A814 F0002214 F04FF841 F04F32FF
  F04B33FF E9CA0B01 E6F4230E 1310E9DA
  DB132B00 DC112C21 DD0F2C0F 0002F109
  428117C2 0202EB73 F111DA08 F1430017
  EA4F0300 454872E9 DA0A4193 93002308
  46499B1A 46229301 980B2302 F816F000
  230AE79E E9CD2200 23183200 461A980B
  F80CF000 32FFF04F 33FFF04F 34FFF04F
  2310E9CA 0B08F04B BF00E78A 47F0E92D
  104A4690 F99D461E EA224024 EB0277E2
  68430263 429A9D08 1404EA44 461ABFA8
  4643B2E4 0901F021 E0024445 42AB3301
  F5B3D015 D2F97FF0 1C00E9D0 FB0C42BA
  DDF3FA03 0C0AEB07 0E02EB01 44D6448C
  4B01F80C D1FB45F4 42AB3301 6882D1E9
  4591444E BFB868C2 9008F8C0 69024590
  36FFF106 F8C0BFB8 4296800C F1036942
  BFC833FF 42936106 6143BFC8 87F0E8BD
  DB132900 7FF0F5B2 6843D210 0C61EA4F
  0F61EBB3 6800DD0A 0303FB02 000CF813
  BF4C07CB 000FF000 47700900 47702000
  47F0E92D 8408E9DD 461E2C00 0242EB02
  4444BFD8 1077DD29 019346C6 1BCD4444
  0C0CF107 F10EE005 45A60E01 030CF103
  F5BED01B D2F67FF0 2100E9D0 290EFB01
  EB05463A 2A000A03 6841DB06 BFC44291
  1002F81A 1002F809 45943201 F10ED1F3
  45A60E01 030CF103 6882D1E3 0317F106
  68C24296 6086BFB8 69024590 F8C0BFB8
  4293800C 6103BFC8 3C016943 BFC8429C
  E8BD6144 BF0087F0
END CSUB
