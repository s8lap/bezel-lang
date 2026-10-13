//! case: s28_8_1_0018
//! doc: spec
//! section: 28.8.1
//! title: a bare take of a nullable owned field stores null and needs no later store
//! rule: Where the location's declared type is nullable — read as declared:
//!   `T?`, a nullable slot type, and not a bare type parameter `T`,
//!   whatever its substitution — the `with` clause may be omitted:
//!   `move x.f` stores `null`.
//! cites: 28.8.1, 17.8
//! expect: run
//! stdout: only True
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Holder
  private owned _c: Res? := new Res("only");

  public method Detach(): owned Res
    return move _c!;
  end Detach;

  public property IsEmpty: bool => _c = null;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var t := h.Detach();
    Console.WriteLine($"{t.Name} {h.IsEmpty}");
  end Main;
end Program;
