// #2051 — §21.5 (v0.201): labeled `break`/`continue` (proposal 0050).
//
// ACCEPT: a labeled `for`, a labeled `while`, nested labeled loops, `break name;`
// / `continue name;` to a lexically enclosing loop, the `end name;` closer, and a
// reused label on non-enclosing (sibling) loops — all compile clean with no
// diagnostics. A local named the same as a label may coexist (labels have their
// own namespace, §21.5).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public static method Main(): int
    var s := 0;
    var scan := 99;                     // a local `scan` may coexist with a label
    scan: for var i in 0..2 do
      for var j in 0..2 do
        if i = 1 and j = 1 then break scan; end;
        if j = 2 then continue scan; end;
        s := s + 1;
      end;
    end scan;
    outer: while s < 100 do
      inner: while true do
        if s > 3 then break outer; end;
        s := s + scan;
        continue inner;
      end inner;
    end outer;
    // A reused label on a non-enclosing (sibling) loop is fine — never in scope
    // simultaneously.
    scan: for var k in 0..1 do break scan; end scan;
    return s;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
