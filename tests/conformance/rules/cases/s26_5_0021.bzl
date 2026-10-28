//! case: s26_5_0021
//! doc: spec
//! section: 26.5
//! title: an instance member inherited from a base type wins over a same-named extension
//! rule: Language spec §26.5 (v0.345) rule 1: "If `x`'s declared type (**or any base type**)
//!   *declares* any instance member named `Foo` … instance resolution is committed and extension
//!   blocks are **never consulted** for that name on that receiver."
//! cites: 26.5
//! expect: run
//! stdout: base
module Conf;

public class Base
  public method Speak(): string
    return "base";
  end Speak;
end Base;

public class Derived: Base
end Derived;

extension Base
  public method Speak(): string
    return "ext";
  end Speak;
end;

public class Program
  public static method Main()
    var d := new Derived();
    Console.WriteLine(d.Speak());
  end Main;
end Program;
