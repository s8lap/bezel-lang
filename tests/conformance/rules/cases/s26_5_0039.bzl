//! case: s26_5_0039
//! doc: spec
//! section: 26.5
//! title: the accepting twin: the same extension under a name StringBuilder does not declare is selected
//! rule: Instance members win — by name. If `x`'s declared type (or any base type) *declares* any instance member named `Foo` — of any arity, applicability, or accessibility — instance resolution is committed and extension blocks are never consulted for that name on that receiver. If the committed instance lookup then fails (no applicable overload, §22.5/§22.6, or the member is inaccessible, §13.1), the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.2
//! expect: run
//! output: 100
module Conf;
import System.Text;
extension StringBuilder
  public method AppendTwo(n: int, m: int): StringBuilder
    return this;
  end AppendTwo;
end;
public class C
  public static method Main(): int
    var tot := 0;
    for var i in 0..<100 do
      tot := tot + new StringBuilder("a").AppendTwo(i, 1).ToString().Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
