//! case: s27_5_7912
//! doc: spec
//! section: 27.5
//! title: two lambdas in a loop body over one outer local, each passed to a callee, see its
//!   value
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 156200
//! notes: Fix pin for a closure-cell leak (#8949): the corpus runner has no leak leg, so this
//!   asserts the program output; leak freedom is covered by the gtest rows.
module M; public class C
  public static method Apply(f: () -> int): int
    return f() + f();
  end Apply;
  public static method Run(): int
    var k := 3; var s := 0;
    for var i in 0..<5 do
      k := k + i;
      var h := lambda() => k;
      s := s + Apply(h);
      k := k * 2;
      var h2 := lambda() => k + 1;
      s := s + Apply(h2);
      k := k - 1;
    end;
    return s + k;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
