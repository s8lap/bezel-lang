// bug: B22
// title: Generic-method inference fails for a `Range<int>` argument
// issue: #8393
// rests-on: stdlib §9.8 `Range<T>: IEnumerable<T>`; spec §14.5 call-site inference for generic methods
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 5
module P;
public class Program
  private static method CountIf<T>(xs: IEnumerable<T>, pred: (T) -> bool): int
    var n := 0;
    for var x in xs do
      if pred(x) then n++; end;
    end;
    return n;
  end CountIf;
  public static method Main()
    Console.WriteLine(CountIf(1..10, lambda(n) => n mod 2 = 0));
  end Main;
end Program;
