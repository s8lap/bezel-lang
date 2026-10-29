//! case: s26_5_0033
//! doc: spec
//! section: 26.5
//! title: an instance member Contains inside a lambda blocks a same-named string-returning extension
//! rule: Instance members win — by name. If `x`'s declared type (or any base type) *declares* any instance member named `Foo` — of any arity, applicability, or accessibility — instance resolution is committed and extension blocks are never consulted for that name on that receiver. If the committed instance lookup then fails (no applicable overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.2
//! expect: reject
//! diagnostic: convertible
module Conf;
extension Array<int> public method Contains(x: int): string => "EXT"; end;
public class Box
  public data: Array<int> := [1];
  public static method Mk(): owned Array<int> return [1]; end Mk;
end Box;
public class Program
  public static method Main(): int
    var a: Array<int> := [1,2];
    var n: Array<int>? := a;
    var bx := new Box();
    var f := lambda(): string => a.Contains(1);
    return 0;
  end Main;
end Program;
