//! case: s14_2_6611
//! doc: spec
//! section: 14.2
//! title: a user record is a value type and does not satisfy `where T: class`
//! rule: Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types.
//! cites: 14.2
//! expect: reject
//! diagnostic: class
//! issue: 8591
module Conf;
public record UserRecord(V: int);
public class Program
  public static method Main(): int
    return F(new UserRecord(1));
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
