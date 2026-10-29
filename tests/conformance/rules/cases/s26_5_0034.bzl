//! case: s26_5_0034
//! doc: spec
//! section: 26.5
//! title: the accepting twin: the instance Contains is selected and yields bool next to a same-named extension
//! rule: Instance members win — by name. If `x`'s declared type (or any base type) *declares* any instance member named `Foo` — of any arity, applicability, or accessibility — instance resolution is committed and extension blocks are never consulted for that name on that receiver. If the committed instance lookup then fails (no applicable overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.2
//! expect: run
//! output: True
//!   False
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
    var b: bool := bx.data.Contains(1); var c: bool := a.Contains(5); Console.WriteLine(b); Console.WriteLine(c);
    return 0;
  end Main;
end Program;
