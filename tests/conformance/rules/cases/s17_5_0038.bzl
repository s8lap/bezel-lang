//! case: s17_5_0038
//! doc: spec
//! section: 17.5
//! title: a -0.0f left operand of in over Array<double> matches +0.0 by value equality
//! rule: "`x in collection` evaluates to `true` if `collection.Contains(x)` is true" ([§17.5]), and the
//!   operand converts to double ([§5.1]): "A value of one type implicitly converts to another when no
//!   programmer-visible information is lost". "The members that compare an argument with stored elements
//!   by `Equals` are the equality search" (stdlib §4) include `Contains`. The element comparison is the
//!   `Equals` that "`a = b` invokes `a.Equals(b)`" ([§8.4]) names, and "`-0.0 = +0.0` is true"
//!   ([§4.3.1]), so `in` and `Contains` find +0.0 for -0.0, and the lambda's `=` agrees. All three print
//!   True.
//! cites: 17.5, 5.1, 8.4, 4.3.1
//! expect: run
//! stdout: True True True
//! issue: 6683
module Conf;
public class Program
  public static method Main()
    var v: float := -0.0f;
    var c: Array<double> := [1.5, 0.0];
    Console.WriteLine($"{v in c} {c.Contains(v)} {c.Any(lambda(e) => e = v)}");
  end Main;
end Program;
