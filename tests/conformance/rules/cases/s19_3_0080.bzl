//! case: s19_3_0080
//! doc: spec
//! section: 19.3
//! title: an Object binding in a case statement over T = int? is rejected at the instantiation site
//! rule: "a type pattern with binding is rejected at the instantiation site whatever the static answer, as `as` is" (§19.3, v0.340, *Type patterns* paragraph): a value substitution (here `int?`) has no box, and a binding to a reference type would form one.
//! cites: 12.7, 3.6
//! expect: reject
//! diagnostic: reference-typed binding
module Conf;

public class Program2
  public static method Q<T>(v: T): int
    var r: int := 0;
    case v of var x: Object => r := 1, _ => r := 0 end case;
    return r;
  end Q;
end Program2;

public class Program
  public static method Main()
    Console.WriteLine(Program2.Q<int?>(42));
  end Main;
end Program;
