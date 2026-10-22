//! case: s35_3_0017
//! doc: spec
//! section: 35.3
//! title: a nested choice type is the declared result for bare case constructions
//! rule: "where the declared type — nullability stripped — is a choice instantiation `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form `N(args)` is a case construction of `C<…>.N(args)`, and the bare identifier `N` is the payload-free case value `C<…>.N`" (§35.3, v0.340). The declared type `Outer.Inner` is a non-generic choice that declares `A` and `B`.
//! cites: 35.5
//! expect: run
//! output: b
//!   a 4
module Conf;

public class Outer
  public choice Inner
    A(n: int),
    B;
  end Inner;
end Outer;

public class User
  public static method Make(n: int): Outer.Inner
    if n = 0 then return B; end;
    return A(n);
  end Make;
end User;

public class Program
  public static method Main()
    for var i in [0, 4] do
      Console.WriteLine((case User.Make(i) of Outer.Inner.A(var n) => $"a {n}", Outer.Inner.B => "b"));
    end;
  end Main;
end Program;
