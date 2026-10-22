//! case: s12_7_0039
//! doc: spec
//! section: 12.7
//! title: an is not null test on an as-cast result, passed directly to Console.WriteLine, prints its value
//! rule: "`as` returns the value cast to the target type, or `null` if the cast fails. Its result is always nullable." ([§12.7]). "`is not null` tests whether it is non-null and narrows to the non-nullable form." ([§12.7]). The first `as` casts a `StringBuilder` held in an `Object` local and succeeds, so `is not null` is true; the second casts a `Random` and yields null, so `is null` is true.
//! cites: 17.4
//! expect: run
//! output: True
//!   True
module Conf;
public class Program
  public static method Main()
    var o1: Object := new StringBuilder();
    var o2: Object := new Random(1);
    var s := o1 as StringBuilder;
    Console.WriteLine(s is not null);
    var t := o2 as StringBuilder;
    Console.WriteLine(t is null);
  end Main;
end Program;
