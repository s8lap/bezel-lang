//! case: s27_5_7914
//! doc: spec
//! section: 27.5
//! title: an async loop whose lambda is formed before an await reads the local's value after
//!   the await
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9, 30.1
//! expect: run
//! stdout: 12600
//! notes: Fix pin for a closure-cell leak (#8949): the corpus runner has no leak leg, so this
//!   asserts the program output; leak freedom is covered by the gtest rows.
module M; public class C
  public static async method Run(): Task<int>
    var k := 3; var s := 0;
    for var i in 0..<5 do
      k := k + i;
      var h := lambda() => k;
      await Task.Delay(0);
      s := s + h();
      k := k + 1;
    end;
    return s + k;
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
