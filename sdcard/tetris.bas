' TETRIS for FRANKOS MMBASIC (Fruit Jam port)
' Controls: left/right arrows (or A/D) move, up (or W) rotate,
'           down (or S) soft drop, SPACE hard drop, P pause, Q quit.
' The falling piece is a SPRITE: SPRITE SHOW moves it atomically
' (restore + save-under + draw in one compound), so piece movement
' cannot tear or ghost no matter how the display flush timing lands.
OPTION DEFAULT INTEGER

CONST CS = 16        ' cell size in pixels
CONST FX = 236       ' field origin x (10 cells * 16 = 160 wide)
CONST FY = 22        ' field origin y (20 cells * 16 = 320 tall)

DIM w(9, 19)                    ' the well: 0 = empty, else colour idx
DIM px(6, 3, 3), py(6, 3, 3)    ' piece blocks: piece, rotation, block
DIM cc(7)                       ' piece colours (1-based into draw)
DIM sz(6)                       ' rotation box size per piece
DIM shp$(6)
DIM score, nlines, lvl, cur, rot, cx, cy, nxt, dropms, gameover, nclr
DIM gspr                        ' sprite # currently shown (0 = none)
DIM lsc(4)
lsc(1) = 40 : lsc(2) = 100 : lsc(3) = 300 : lsc(4) = 1200

ReadPieces
cc(1) = RGB(CYAN) : cc(2) = RGB(YELLOW) : cc(3) = RGB(BLUE)
cc(4) = RGB(255, 128, 0) : cc(5) = RGB(GREEN) : cc(6) = RGB(MAGENTA)
cc(7) = RGB(RED)

BuildSprites
NewGame

DO
  k$ = INKEY$
  IF k$ <> "" THEN HandleKey ASC(k$)
  IF NOT gameover THEN
    IF TIMER > dropms THEN
      TIMER = 0
      IF Fits(cur, rot, cx, cy + 1) THEN
        MovePiece cx, cy + 1, rot
      ELSE
        LockPiece
      ENDIF
    ENDIF
  ENDIF
  PAUSE 10
LOOP

SUB HandleKey k
  LOCAL nr, dy
  IF k = ASC("q") OR k = ASC("Q") THEN
    HideCur
    CLS
    ON ERROR SKIP 1
    PLAY STOP
    END
  ENDIF
  IF gameover THEN
    IF k = ASC(" ") AND TIMER > 500 THEN NewGame
    EXIT SUB
  ENDIF
  IF k = ASC("p") OR k = ASC("P") THEN
    Centre "* PAUSED *"
    DO : PAUSE 20 : LOOP UNTIL INKEY$ <> ""
    HideCur
    RedrawWell
    ShowCur
    EXIT SUB
  ENDIF
  SELECT CASE k
    CASE &H82, ASC("a"), ASC("A")            ' left
      IF Fits(cur, rot, cx - 1, cy) THEN MovePiece cx - 1, cy, rot
    CASE &H83, ASC("d"), ASC("D")            ' right
      IF Fits(cur, rot, cx + 1, cy) THEN MovePiece cx + 1, cy, rot
    CASE &H80, ASC("w"), ASC("W")            ' rotate CW
      nr = (rot + 1) MOD 4
      IF Fits(cur, nr, cx, cy) THEN
        MovePiece cx, cy, nr
        PLAY TONE 880, 880, 15
      ELSEIF Fits(cur, nr, cx - 1, cy) THEN  ' simple wall kick
        MovePiece cx - 1, cy, nr
        PLAY TONE 660, 660, 15               ' lower blip: kicked rotate
      ELSEIF Fits(cur, nr, cx + 1, cy) THEN
        MovePiece cx + 1, cy, nr
        PLAY TONE 660, 660, 15
      ENDIF
    CASE &H81, ASC("s"), ASC("S")            ' soft drop
      IF Fits(cur, rot, cx, cy + 1) THEN MovePiece cx, cy + 1, rot : score = score + 1
    CASE ASC(" ")                            ' hard drop
      dy = cy
      DO WHILE Fits(cur, rot, cx, dy + 1)
        dy = dy + 1 : score = score + 2
      LOOP
      MovePiece cx, dy, rot
      LockPiece
  END SELECT
END SUB

' ── Sprite handling for the falling piece ───────────────────────────
SUB ShowCur
  SPRITE SHOW #(cur * 4 + rot + 1), FX + cx * CS, FY + cy * CS, 1
  gspr = cur * 4 + rot + 1
END SUB

SUB HideCur
  IF gspr THEN SPRITE HIDE #gspr : gspr = 0
END SUB

SUB MovePiece nx, ny, nr
  IF nr <> rot THEN HideCur                  ' rotation swaps sprite images
  cx = nx : cy = ny : rot = nr
  ShowCur
END SUB

SUB LockPiece
  LOCAL i, fx, fy, n
  HideCur
  FOR i = 0 TO 3
    fx = cx + px(cur, rot, i) : fy = cy + py(cur, rot, i)
    IF fy >= 0 THEN
      w(fx, fy) = cur + 1
      Cell fx, fy, cc(cur + 1)
    ENDIF
  NEXT
  PLAY TONE 220, 220, 25
  DoClearLines
  n = nclr
  IF n > 0 THEN
    IF n > 4 THEN n = 4
    score = score + lsc(n) * (lvl + 1)
    nlines = nlines + n
    IF nlines \ 10 > lvl THEN
      lvl = nlines \ 10
      dropms = 600 - 50 * lvl
      IF dropms < 80 THEN dropms = 80
      PLAY TONE 660, 660, 40 : PAUSE 50 : PLAY TONE 990, 990, 60
    ENDIF
  ENDIF
  SpawnPiece
  IF gameover THEN
    PLAY TONE 330, 330, 200 : PAUSE 220 : PLAY TONE 165, 165, 400
    Centre "GAME OVER - SPACE for new game"
    ' discard keys buffered during the death (a SPACE mashed on a fast
    ' level must not instantly start a new game), and hold off restart
    ' for the first half second
    DO WHILE INKEY$ <> "" : LOOP
    TIMER = 0
    EXIT SUB
  ENDIF
  Panel
END SUB

' Find ALL completed rows, flash them together, then collapse them all
' and repaint once.
SUB DoClearLines
  LOCAL x, y, yy, full, f, i, rows(3)
  nclr = 0
  FOR y = 0 TO 19
    full = 1
    FOR x = 0 TO 9
      IF w(x, y) = 0 THEN full = 0 : EXIT FOR
    NEXT
    IF full THEN
      IF nclr < 4 THEN rows(nclr) = y
      nclr = nclr + 1
    ENDIF
  NEXT
  IF nclr = 0 THEN EXIT SUB
  FOR f = 1 TO 2
    FOR i = 0 TO nclr - 1
      BOX FX, FY + rows(i) * CS, 10 * CS, CS, , RGB(WHITE), RGB(WHITE)
    NEXT
    PAUSE 50
    FOR i = 0 TO nclr - 1
      BOX FX, FY + rows(i) * CS, 10 * CS, CS, , 0, 0
    NEXT
    PAUSE 35
  NEXT
  ' collapse: remove full rows bottom-up
  y = 19
  DO WHILE y >= 0
    full = 1
    FOR x = 0 TO 9
      IF w(x, y) = 0 THEN full = 0 : EXIT FOR
    NEXT
    IF full THEN
      FOR yy = y TO 1 STEP -1
        FOR x = 0 TO 9
          w(x, yy) = w(x, yy - 1)
        NEXT
      NEXT
      FOR x = 0 TO 9 : w(x, 0) = 0 : NEXT
    ELSE
      y = y - 1
    ENDIF
  LOOP
  RedrawWell
  PLAY TONE 440 + 110 * nclr, 440 + 110 * nclr, 60
END SUB

SUB SpawnPiece
  cur = nxt : nxt = INT(RND * 7)
  rot = 0 : cx = 3 : cy = 0
  IF NOT Fits(cur, rot, cx, cy) THEN
    gameover = 1
    EXIT SUB
  ENDIF
  ShowCur
END SUB

FUNCTION Fits(p, r, x, y)
  LOCAL i, fx, fy
  Fits = 0
  FOR i = 0 TO 3
    fx = x + px(p, r, i) : fy = y + py(p, r, i)
    IF fx < 0 OR fx > 9 OR fy > 19 THEN EXIT FUNCTION
    IF fy >= 0 THEN
      IF w(fx, fy) THEN EXIT FUNCTION
    ENDIF
  NEXT
  Fits = 1
END FUNCTION

SUB Cell x, y, c
  IF y < 0 THEN EXIT SUB
  IF c THEN
    BOX FX + x * CS, FY + y * CS, CS - 1, CS - 1, , c, c
  ELSE
    BOX FX + x * CS, FY + y * CS, CS, CS, , 0, 0
  ENDIF
END SUB

SUB RedrawWell
  LOCAL x, y
  FOR y = 0 TO 19
    FOR x = 0 TO 9
      IF w(x, y) THEN Cell x, y, cc(w(x, y)) ELSE Cell x, y, 0
    NEXT
  NEXT
END SUB

SUB Panel
  LOCAL i, c
  TEXT 430, 40, "SCORE", , , , RGB(WHITE), 0
  TEXT 430, 60, STR$(score) + "    ", , , , RGB(YELLOW), 0
  TEXT 430, 90, "LINES", , , , RGB(WHITE), 0
  TEXT 430, 110, STR$(nlines) + "   ", , , , RGB(CYAN), 0
  TEXT 430, 140, "LEVEL", , , , RGB(WHITE), 0
  TEXT 430, 160, STR$(lvl + 1) + "   ", , , , RGB(GREEN), 0
  TEXT 430, 200, "NEXT", , , , RGB(WHITE), 0
  ' preview aligned with the label: piece cells start at x=430
  BOX 430, 220, 4 * CS, 4 * CS, , 0, 0
  FOR i = 0 TO 3
    c = cc(nxt + 1)
    BOX 430 + px(nxt, 0, i) * CS, 220 + py(nxt, 0, i) * CS, CS - 1, CS - 1, , c, c
  NEXT
END SUB

SUB Centre m$
  TEXT 316, 180, m$, "C", , , RGB(WHITE), RGB(BLUE)
END SUB

SUB NewGame
  LOCAL x, y
  HideCur
  CLS
  FOR y = 0 TO 19
    FOR x = 0 TO 9 : w(x, y) = 0 : NEXT
  NEXT
  score = 0 : nlines = 0 : lvl = 0 : dropms = 600 : gameover = 0
  BOX FX - 3, FY - 3, 10 * CS + 6, 20 * CS + 6, 2, RGB(128, 128, 128)
  TEXT 316, 6, "T E T R I S", "C", , , RGB(CYAN), 0
  TEXT 316, 368, "arrows/WASD  SPACE=drop  P=pause  Q=quit", "C", , , , RGB(128, 128, 128), 0
  nxt = INT(RND * 7)
  SpawnPiece
  Panel
  TIMER = 0
END SUB

' Build 28 sprites (7 pieces x 4 rotations) once at startup: draw each
' shape in the top-left corner, capture it, wipe.  Black (colour 0) is
' transparent so empty box cells never blot out neighbouring blocks.
SUB BuildSprites
  ' The gfx layer has no off-screen page (SPRITE READ captures the live
  ' buffer), so the build is staged centre-screen under the banner and
  ' reads as a piece-cycling loading animation.
  LOCAL p, r, i, n, bx, by
  bx = 284 : by = 200
  CLS
  TEXT 316, 150, "T E T R I S", "C", , , RGB(CYAN), 0
  TEXT 316, 290, "LOADING...", "C", , , RGB(WHITE), 0
  SPRITE SET TRANSPARENT 0
  FOR p = 0 TO 6
    FOR r = 0 TO 3
      n = p * 4 + r + 1
      BOX bx, by, 4 * CS, 4 * CS, , 0, 0
      FOR i = 0 TO 3
        BOX bx + px(p, r, i) * CS, by + py(p, r, i) * CS, CS - 1, CS - 1, , cc(p + 1), cc(p + 1)
      NEXT
      SPRITE READ #n, bx, by, 4 * CS, 4 * CS
    NEXT
  NEXT
END SUB

SUB ReadPieces
  LOCAL p, r, i, x, y, n, s$, t$
  RESTORE PieceData
  FOR p = 0 TO 6
    READ sz(p), shp$(p)
  NEXT
  FOR p = 0 TO 6
    s$ = shp$(p) : n = sz(p)
    FOR r = 0 TO 3
      i = 0
      FOR y = 0 TO n - 1
        FOR x = 0 TO n - 1
          IF MID$(s$, y * n + x + 1, 1) = "1" THEN
            px(p, r, i) = x : py(p, r, i) = y : i = i + 1
          ENDIF
        NEXT
      NEXT
      ' rotate the string CW for the next rotation state
      t$ = s$
      FOR y = 0 TO n - 1
        FOR x = 0 TO n - 1
          MID$(t$, y * n + x + 1, 1) = MID$(s$, (n - 1 - x) * n + y + 1, 1)
        NEXT
      NEXT
      s$ = t$
    NEXT
  NEXT
END SUB

PieceData:
DATA 4, "0000111100000000"
DATA 2, "1111"
DATA 3, "100111000"
DATA 3, "001111000"
DATA 3, "011110000"
DATA 3, "010111000"
DATA 3, "110011000"
