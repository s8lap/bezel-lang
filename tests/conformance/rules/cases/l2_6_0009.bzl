//! case: l2_6_0009
//! doc: stdlib
//! section: 2.6
//! title: A member access on a Guid? that is not narrowed is rejected
//! rule: A member access on a nullable value requires narrowing it to non-null first (spec 7.4); Guid? is nullable, so g.ToString() without a guard is a compile error.
//! cites: 7.4
//! expect: reject
//! diagnostic: requires narrowing
module Conf;

public class Program
  public static method Mk(): Guid? => Guid.Parse("11111111-2222-3333-4444-555555555555");
  public static method Main()
    var g: Guid? := Program.Mk();
    Console.WriteLine(g.ToString());
  end Main;
end Program;
