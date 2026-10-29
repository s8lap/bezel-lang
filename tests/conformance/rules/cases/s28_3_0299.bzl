//! case: s28_3_0299
//! doc: spec
//! section: 28.3
//! title: builder temporaries nested inside the named arguments of another builder temporary call are used
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order — on the normal edge as on the exception edge, where the pass runs during unwinding.
//! cites: 22.5, 28.3
//! expect: run
//! output: 1100
module Conf;
import System.Text;
class C
  public static method Ix(i: int): int return i mod 2; end Ix;
  public static method Main(): int
    var tot := 0;
    for var i in 0..<100 do
      tot := tot + new StringBuilder("abc").Insert(value := new StringBuilder("q").Insert(value := "r", index := Ix(i)).ToString(), index := Ix(i)).Length;
      tot := tot + new StringBuilder("abc").Insert(value := new StringBuilder("q").Append("r").ToString(), index := Ix(i)).Insert(value := "s", index := Ix(0)).ToString().Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
