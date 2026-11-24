//! case: s27_5_7915
//! doc: spec
//! section: 27.5
//! title: a lambda formed conditionally in a while loop reads the local the loop updated
//!   before it
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5
//! expect: run
//! stdout: 40
//! issue: 9004
module M; public class C
  public static method Main(): int
    var k := 3; var s := 0;
    var i := 0;
    while i < 6 do
      k := k + i;
      if i mod 2 = 0 then var h := lambda() => k; s := s + h(); end;
      i := i + 1;
    end;
    Console.WriteLine(s + k);
    return 0;
  end Main;
end C;
