//! case: l5_0040
//! doc: stdlib
//! section: 5
//! title: Contains on an IReadOnlyList of a record with a function-typed component is rejected
//! rule: The Contains(value: T) extension is declared where T: hashable (stdlib 5); a record with a function-typed component is not hashable, so the extension is not applicable to IReadOnlyList<Fn>.
//! cites: 5, 4.1
//! expect: reject
//! diagnostic: hashable
module Conf;

public record Fn(F: (int) -> int);

public class Program
  public static method Main()
    var fa: Array<Fn> := [new Fn(lambda(x: int) => x)];
    var fr: IReadOnlyList<Fn> := fa;
    var f := new Fn(lambda(x: int) => x);
    Console.WriteLine(fr.Contains(f));
  end Main;
end Program;
