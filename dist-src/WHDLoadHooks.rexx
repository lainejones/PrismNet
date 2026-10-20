/* WHDLoadHooks.rexx - PrismNet's lines in S:WHDLoad.prefs
 *
 *   rx WHDLoadHooks.rexx CHECK    rc 0: none set, 5: PrismNet's are set,
 *                                 10: another ExecuteStartup/Cleanup is set
 *   rx WHDLoadHooks.rexx ADD      adds PrismNet's (only when none are set)
 *   rx WHDLoadHooks.rexx REMOVE   takes PrismNet's out again
 *   (a file name after the word works on that file instead)
 *
 * A WHDLoad game takes the whole machine over; a network card that keeps
 * raising interrupts then hangs some games (a black screen). WHDLoad runs
 * ExecuteStartup before the game and ExecuteCleanup after it, so these
 * stop PrismNet for the game ("STOP HOLD": no program can start it again
 * meanwhile) and start it afterwards.
 *
 * Part of PrismNet - GPL-3.0-only, see Docs/LICENSE. */

PARSE ARG mode prefs .
mode = UPPER(mode)
IF prefs = '' THEN prefs = 'S:WHDLoad.prefs'      /* another file: for testing */
mark  = 'PrismNet'

n = 0
IF OPEN('in', prefs, 'R') THEN DO
    DO WHILE ~EOF('in')
        l = READLN('in')
        IF EOF('in') & l = '' THEN LEAVE
        n = n + 1
        line.n = l
    END
    CALL CLOSE('in')
END

ours = 0
other = 0
DO i = 1 TO n
    key = UPPER(STRIP(line.i))
    IF LEFT(key, 15) = 'EXECUTESTARTUP=' | LEFT(key, 15) = 'EXECUTECLEANUP=' THEN DO
        IF POS(UPPER(mark), key) > 0 THEN ours = 1
        ELSE other = 1
    END
END

SELECT
    WHEN mode = 'CHECK' THEN DO
        IF other THEN EXIT 10
        IF ours THEN EXIT 5
        EXIT 0
    END
    WHEN mode = 'ADD' THEN DO
        IF ours | other THEN EXIT 0
        IF ~OPEN('out', prefs, 'A') THEN IF ~OPEN('out', prefs, 'W') THEN EXIT 20
        CALL WRITELN('out', ';PrismNet: network off while a WHDLoad game runs (Install_PrismNet)')
        CALL WRITELN('out', 'ExecuteStartup=C:PrismNet STOP HOLD QUIET')
        CALL WRITELN('out', 'ExecuteCleanup=C:PrismNet START')
        CALL CLOSE('out')
        EXIT 0
    END
    WHEN mode = 'REMOVE' THEN DO
        IF ~ours THEN EXIT 0
        tmp = 'T:WHDLoad.prefs.prismnet'
        IF ~OPEN('out', tmp, 'W') THEN EXIT 20
        DO i = 1 TO n
            key = UPPER(STRIP(line.i))
            IF POS(UPPER(mark), key) > 0 & (LEFT(key, 7) = 'EXECUTE' | LEFT(key, 9) = ';PRISMNET') THEN
                ITERATE
            CALL WRITELN('out', line.i)
        END
        CALL CLOSE('out')
        ADDRESS COMMAND 'Copy >NIL: "' || tmp || '" "' || prefs || '" QUIET'
        r = rc
        ADDRESS COMMAND 'Delete >NIL: "' || tmp || '" QUIET'
        EXIT r
    END
    OTHERWISE DO
        SAY 'Usage: rx WHDLoadHooks.rexx CHECK|ADD|REMOVE'
        EXIT 10
    END
END
