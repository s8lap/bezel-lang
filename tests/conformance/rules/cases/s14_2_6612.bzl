//! case: s14_2_6612
//! doc: spec
//! section: 14.2
//! title: stdlib record `Guid` satisfies `where T: value`
//! rule: Non-nullable value types — records, enums, choices
//! cites: 14.2
//! expect: run
//! output: accepted
//! issue: 8733
module Conf;
public class Program
  public static method Main(): int
    return F(Guid.NewGuid());
  end Main;
  public static method F<T>(x: T): int where T: value
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
