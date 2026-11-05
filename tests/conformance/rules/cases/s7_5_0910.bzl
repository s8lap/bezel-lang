//! case: s7_5_0910
//! doc: spec
//! section: 7.5
//! title: LINQ lambdas after ?. on a nullable Array of class instances read members of the element
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path where it runs
//!   — the preceding segment's own declared type, never the nullable type the `?.` gives the
//!   chain's result
//! cites: 27.4
//! expect: run
//! output: 2
//!   ab
//!   1
//! issue: 8166
module Conf;

public class Person
  public Name: string;

  public constructor(n: string)
    Name := n;
  end constructor;
end Person;

public class Program
  public static method Main()
    using var arr := new Array<Person>();
    using var p1 := new Person("ab");
    using var p2 := new Person("c");
    arr.Add(p1);
    arr.Add(p2);
    var a: Array<Person>? := arr;
    var r := a?.Select(lambda(p) => p.Name.Length)?.ToArray();
    var r2 := a?.Select(lambda(p) => p.Name)?.ToArray();
    var r3 := a?.Where(lambda(p) => p.Name.Length > 1)?.ToArray();
    Console.WriteLine(r!.Count);
    Console.WriteLine(r2![0]);
    Console.WriteLine(r3!.Count);
  end Main;
end Program;
