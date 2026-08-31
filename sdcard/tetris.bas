' TETRIS for FRANKOS MMBASIC (Fruit Jam port)
' Controls: left/right arrows (or A/D) move, up (or W) rotate,
'           down (or S) soft drop, SPACE hard drop, P pause, Q quit.
OPTION DEFAULT INTEGER

CONST CS = 16        ' cell size in pixels
CONST FX = 236       ' field origin x (10 cells * 16 = 160 wide)
CONST FY = 22        ' field origin y (20 cells * 16 = 320 tall)

DIM w(9, 19)                    ' the well: 0 = empty, else colour idx
DIM px(6, 3, 3), py(6, 3, 3)    ' piece blocks: piece, rotation, block
DIM cc(7)                       ' piece colours (1-based into draw)
DIM sz(6)                       ' rotation box size per piece
DIM shp$(6)
DIM score, lines, level, cur, rot, cx, cy, nxt, dropms, gameover

ReadPieces
cc(1) = RGB(CYAN) : cc(2) = RGB(YELLOW) : cc(3) = RGB(BLUE)
cc(4) = RGB(255, 128, 0) : cc(5) = RGB(GREEN) : cc(6) = RGB(MAGENTA)
cc(7) = RGB(RED)

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
  LOCAL nr
  IF k = ASC("q") OR k = ASC("Q") THEN
    CLS
    ON ERROR SKIP 1
    PLAY STOP
    END
  ENDIF
  IF gameover THEN
    IF k = ASC(" ") THEN NewGame
    EXIT SUB
  ENDIF
  IF k = ASC("p") OR k = ASC("P") THEN
    Centre "* PAUSED *"
    DO : PAUSE 20 : LOOP UNTIL INKEY$ <> ""
    RedrawWell : DrawPiece cur, rot, cx, cy
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
      ELSEIF Fits(cur, nr, cx + 1, cy) THEN
        MovePiece cx + 1, cy, nr
      ENDIF
    CASE &H81, ASC("s"), ASC("S")            ' soft drop
      IF Fits(cur, rot, cx, cy + 1) THEN MovePiece cx, cy + 1, rot : score = score + 1
    CASE ASC(" ")                            ' hard drop
      DO WHILE Fits(cur, rot, cx, cy + 1)
        cy = cy + 1 : score = score + 2
      LOOP
      ErasePiece cur, rot, cx, cy : DrawPiece cur, rot, cx, cy
      LockPiece
  END SELECT
END SUB

SUB MovePiece nx, ny, nr
  ErasePiece cur, rot, cx, cy
  cx = nx : cy = ny : rot = nr
  DrawPiece cur, rot, cx, cy
END SUB

SUB LockPiece
  LOCAL i, fx, fy, n
  FOR i = 0 TO 3
    fx = cx + px(cur, rot, i) : fy = cy + py(cur, rot, i)
    IF fy >= 0 THEN w(fx, fy) = cur + 1
    IF fy < 0 THEN gameover = 1
  NEXT
  PLAY TONE 220, 220, 25
  n = ClearLines()
  IF n > 0 THEN
    SELECT CASE n
      CASE 1 : score = score + 40 * (level + 1)
      CASE 2 : score = score + 100 * (level + 1)
      CASE 3 : score = score + 300 * (level + 1)
      CASE 4 : score = score + 1200 * (level + 1)
    END SELECT
    lines = lines + n
    IF lines \ 10 > level THEN
      level = lines \ 10
      dropms = 600 - 50 * level
      IF dropms < 80 THEN dropms = 80
      PLAY TONE 660, 660, 40 : PAUSE 50 : PLAY TONE 990, 990, 60
    ENDIF
  ENDIF
  IF gameover THEN
    PLAY TONE 330, 330, 200 : PAUSE 220 : PLAY TONE 165, 165, 400
    Centre "GAME OVER - SPACE for new game"
    EXIT SUB
  ENDIF
  SpawnPiece
  Panel
END SUB

FUNCTION ClearLines()
  LOCAL x, y, yy, full, n, f
  n = 0
  FOR y = 0 TO 19
    full = 1
    FOR x = 0 TO 9
      IF w(x, y) = 0 THEN full = 0 : EXIT FOR
    NEXT
    IF full THEN
      n = n + 1
      FOR f = 1 TO 2                          ' flash
        BOX FX, FY + y * CS, 10 * CS, CS, , RGB(WHITE), RGB(WHITE)
        PAUSE 40
        BOX FX, FY + y * CS, 10 * CS, CS, , 0, 0
        PAUSE 30
      NEXT
      FOR yy = y TO 1 STEP -1
        FOR x = 0 TO 9
          w(x, yy) = w(x, yy - 1)
        NEXT
      NEXT
      FOR x = 0 TO 9 : w(x, 0) = 0 : NEXT
      PLAY TONE 440 + 110 * n, 440 + 110 * n, 50
    ENDIF
  NEXT
  IF n > 0 THEN RedrawWell
  ClearLines = n
END FUNCTION

SUB SpawnPiece
  cur = nxt : nxt = INT(RND * 7)
  rot = 0 : cx = 3 : cy = -2
  IF NOT Fits(cur, rot, cx, cy) THEN gameover = 1
  DrawPiece cur, rot, cx, cy
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

SUB DrawPiece p, r, x, y
  LOCAL i
  FOR i = 0 TO 3
    Cell x + px(p, r, i), y + py(p, r, i), cc(p + 1)
  NEXT
END SUB

SUB ErasePiece p, r, x, y
  LOCAL i
  FOR i = 0 TO 3
    Cell x + px(p, r, i), y + py(p, r, i), 0
  NEXT
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
  TEXT 430, 110, STR$(lines) + "   ", , , , RGB(CYAN), 0
  TEXT 430, 140, "LEVEL", , , , RGB(WHITE), 0
  TEXT 430, 160, STR$(level) + "   ", , , , RGB(GREEN), 0
  TEXT 430, 200, "NEXT", , , , RGB(WHITE), 0
  BOX 430, 220, 5 * CS, 5 * CS, , 0, 0
  FOR i = 0 TO 3
    c = cc(nxt + 1)
    BOX 430 + (px(nxt, 0, i) + 1) * CS, 220 + (py(nxt, 0, i) + 1) * CS, CS - 1, CS - 1, , c, c
  NEXT
END SUB

SUB Centre m$
  TEXT 316, 180, m$, "C", , , RGB(WHITE), RGB(BLUE)
END SUB

SUB NewGame
  LOCAL x, y
  CLS
  FOR y = 0 TO 19
    FOR x = 0 TO 9 : w(x, y) = 0 : NEXT
  NEXT
  score = 0 : lines = 0 : level = 0 : dropms = 600 : gameover = 0
  BOX FX - 3, FY - 3, 10 * CS + 6, 20 * CS + 6, 2, RGB(128, 128, 128)
  TEXT 316, 6, "T E T R I S", "C", , , RGB(CYAN), 0
  TEXT 60, 340, "arrows/WASD  SPACE=drop  P=pause  Q=quit", , , , RGB(128, 128, 128), 0
  nxt = INT(RND * 7)
  SpawnPiece
  Panel
  TIMER = 0
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
