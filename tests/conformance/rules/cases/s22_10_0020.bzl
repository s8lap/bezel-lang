//! case: s22_10_0020
//! doc: spec
//! section: 22.10
//! title: inheritance-clause base call forwards the paramName to a built-in argument exception
//! rule: "When a class with a primary constructor has a base class, the base call appears in the inheritance clause" ([§22.10]). The base call is the clause's argument list, so R's message and paramName reach `ArgumentOutOfRangeException`'s `(message, paramName)` form; "public readonly property ParamName: string?;" ([§31.4]) is the property that form supplies, and the `ArgumentException` arm reads it back.
//! cites: 31.4
//! expect: run
//! output: bad=0
module Conf;
public class R(m: string, p: string): ArgumentOutOfRangeException($"{m}~", p + "!")
end R;
public class Program
  public static method Main(): int
    var bad: int := 0;
    var i: int := 0;
    var ok: bool := false;
    while i < 200 do
      i := i + 1;
      var s: string := i.ToString();
      ok := false;
      try
        throw new R("m" + s, "p" + s);
      catch var e: ArgumentException =>
        ok := e.Message = "m" + s + "~" and e.ParamName = "p" + s + "!";
      end;
      if not ok then bad := bad + 1; end if;
    end while;
    Console.WriteLine($"bad={bad}");
    return bad;
  end Main;
end Program;
