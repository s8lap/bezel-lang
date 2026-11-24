//! case: s27_5_7911
//! doc: spec
//! section: 27.5
//! title: a lambda in an inner loop captures a local declared in the outer loop body and sees
//!   its updates
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 6600
//! notes: Fix pin for a closure-cell leak (#8949): the corpus runner has no leak leg, so this
//!   asserts the program output; leak freedom is covered by the gtest rows.
module M; public class C
  public static method Run(): int
    var s := 0;
    for var i in 0..<3 do
      var k := i;
      for var j in 0..<3 do
        k := k + j;
        var h := lambda() => k;
        s := s + h();
      end;
      s := s + k;
    end;
    return s;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
