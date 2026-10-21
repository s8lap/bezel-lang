//! case: s17_5_0037
//! doc: spec
//! section: 17.5
//! title: a float left operand of in over Array<double> converts to double and matches
//! rule: "`x in collection` evaluates to `true` if `collection.Contains(x)` is true" ([§17.5]), and the left
//!   operand is "admitted exactly when it is admissible as that call's argument under the ordinary
//!   conversion rules" ([§17.5]). "A value of one type implicitly converts to another when no
//!   programmer-visible information is lost" ([§5.1]), and the table row "| `float` | `double` |"
//!   ([§5.1]) makes 2.5f the double 2.5. Contains finds 2.5, and the lambda's `=` finds it too, since "`a
//!   = b` invokes `a.Equals(b)`" ([§8.4]) and 2.5 = 2.5. All three checks print True.
//! cites: 17.5, 5.1, 8.4
//! expect: run
//! stdout: True True True
//! issue: 6683
module Conf;
public class Program
  public static method Main()
    var v: float := 2.5f;
    var c: Array<double> := [1.5, 2.5];
    Console.WriteLine($"{v in c} {c.Contains(v)} {c.Any(lambda(e) => e = v)}");
  end Main;
end Program;
