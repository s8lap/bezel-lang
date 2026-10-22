//! case: s28_3_0208
//! doc: spec
//! section: 28.3
//! title: an owned constructor parameter given a literal keeps the array for the object lifetime
//! rule: Control for #7045. "Neither source is lifetime-extended" (§28.3 *Borrow lifetime rules*, clause 3, v0.340), so the remedy the rule names is to give the array an owner that outlives the holder: a parameter declared `owned xs: Array<int>` takes the literal by transfer (§28.8), so the object owns the array and the read after the full expression is valid.
//! cites: 22.10, 28.8
//! expect: run
//! stdout: 6
module Conf;

public class B(owned xs: Array<int>)
  public method Sum(): int
    var s := 0;
    for var x in xs do s := s + x; end;
    return s;
  end Sum;
end B;

public class Program
  public static method Main()
    var h: B? := null;
    do
      h := new B([1, 2, 3]);
    end;
    Console.WriteLine(h!.Sum());
  end Main;
end Program;
