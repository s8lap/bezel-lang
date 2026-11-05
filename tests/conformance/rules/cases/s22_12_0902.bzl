//! case: s22_12_0902
//! doc: spec
//! section: 22.12
//! title: an inherited member of a generic base is visible in an async lambda of the derived class
//! rule: inherited members are visible at the derived declaration site
//! cites: 12.2, 27.5
//! expect: run
//! output: dee!
//!   7
//! issue: 8127
module Conf;

public class B<T>
  public V: T;

  public constructor(v: T)
    V := v;
  end constructor;

  public method Get(): T => V;
end B;

public class D: B<string>
  public constructor()
    base("dee");
  end constructor;

  public method Run(): string
    var f := async lambda(): Task<string> => Get() + "!";
    using var t := f();
    return t.Result;
  end Run;
end D;

public record R(K: int)
  public method Go(): int
    using var d := new D();
    Console.WriteLine(d.Run());
    return K;
  end Go;
end R;

public class Program
  public static method Main()
    var r := new R(7);
    Console.WriteLine(r.Go());
  end Main;
end Program;
