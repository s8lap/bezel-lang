//! case: s26_5_0037
//! doc: spec
//! section: 26.5
//! title: a same-named extension ToString on a class never shadows the universal instance ToString
//! rule: Instance members win — by name. If `x`'s declared type (or any base type) *declares* any instance member named `Foo` — of any arity, applicability, or accessibility — instance resolution is committed and extension blocks are never consulted for that name on that receiver. If the committed instance lookup then fails (no applicable overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.2, 8.5
//! expect: run
//! output: Conf.Foo
module Conf;
public class Foo end Foo;
extension Foo public method ToString(): string => "EXT"; end;
public class Program
  public static method Main(): int
    var f := new Foo();
    Console.WriteLine(f.ToString());
    return 0;
  end Main;
end Program;
