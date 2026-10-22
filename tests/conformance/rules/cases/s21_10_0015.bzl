//! case: s21_10_0015
//! doc: spec
//! section: 21.10
//! title: the spec's Render example builds its rows from a local record and renders them
//! rule: "The form lets a shape used once — a projection's result record, a small closed constant set scoped to one algorithm — live next to its single use"
//! cites: 9
//! expect: run
//! output: True
module Conf;

public class User
  public readonly property Name: string;
  public readonly property Age: int;

  public constructor(n: string, a: int)
    Name := n;
    Age := a;
  end constructor;
end User;

public class Program
  public static method Render(users: Array<User>): string
    record NameAge(Name: string, Age: int);
    var rows := users.Select(lambda(u) => new NameAge(u.Name, u.Age)).ToArray();
    var sb := new StringBuilder();
    for var row in rows do
      sb.AppendLine($"{row.Name}: {row.Age}");
    end;
    return sb.ToString();
  end Render;

  public static method Main()
    using var a := new User("ann", 3);
    using var b := new User("bob", 5);
    var us := new Array<User>();
    us.Add(a);
    us.Add(b);
    Console.WriteLine(Render(us).Contains("bob: 5"));
  end Main;
end Program;
