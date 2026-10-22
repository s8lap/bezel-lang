//! case: s28_3_0207
//! doc: spec
//! section: 28.3
//! title: a stored plain-parameter array bound to a using var declared before the holder is read correctly
//! rule: Control for #7045. "Neither source is lifetime-extended" (§28.3 *Borrow lifetime rules*, clause 3, v0.340), so the remedy the rule names is to give the array an owner that outlives the holder: a `using var` declared before the holder, passed by name, lives to the end of `Main`, so the stored borrow is valid at the read.
//! cites: 22.10
//! expect: run
//! stdout: 6
module Conf;

public class B
  private _xs: Array<int>;

  public constructor(xs: Array<int>)
    _xs := xs;
  end;

  public method Sum(): int
    var s := 0;
    for var x in _xs do s := s + x; end;
    return s;
  end Sum;
end B;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var h := new B(xs);
    Console.WriteLine(h.Sum());
  end Main;
end Program;
