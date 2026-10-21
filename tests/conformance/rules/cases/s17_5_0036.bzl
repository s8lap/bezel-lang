//! case: s17_5_0036
//! doc: spec
//! section: 17.5
//! title: a float left operand widens to double for membership in an Array<double>
//! rule: "`x in c` *is* the call `c.Contains(x)`, so the left operand is admitted exactly when it is admissible as that call's argument under the ordinary conversion rules" ([§5.1]). The §5.1 table lists `float` as implicitly converting to `double`, so the `float` value 2.5 is admitted as `Contains(item: double)`'s argument, and the element 2.5 is present: the membership test is True.
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
//! issue: 6683
module Conf;

public class Program
  public static method Main()
    var a: Array<double> := [1.5, 2.5];
    var f: float := 2.5f;
    Console.WriteLine(f in a);
  end Main;
end Program;
