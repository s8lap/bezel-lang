//! case: s27_5_7913
//! doc: spec
//! section: 27.5
//! title: a lambda in a loop over a local initialised from a parameter reads the
//!   parameter-derived values
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 154700
//! notes: Fix pin for a closure-cell leak (#8949): the corpus runner has no leak leg, so this
//!   asserts the program output; leak freedom is covered by the gtest rows.
module M; public class C
  public static method Run(k0: int): int
    var k := k0; var s := 0;
    for var i in 0..<5 do k := k + i; var h := lambda() => k; s := s + h(); k := k + 1; end;
    return s + k + k0;
  end Run;
  public static method Run2(t0: string): int
    var t := t0; var n := 0;
    for var i in 0..<5 do t := t + "x"; var h := lambda() => t.Length; n := n + h(); end;
    return n + t.Length;
  end Run2;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(j) + C.Run2("ab"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
