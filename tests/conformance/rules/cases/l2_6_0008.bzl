//! case: l2_6_0008
//! doc: stdlib
//! section: 2.6
//! title: Guid? members after each narrowing form render the canonical D and N forms
//! rule: A Guid? is a nullable value; a member access on it requires narrowing it to non-null first (spec 7.4): guard with a null test, a pattern, ! or ?. ; Guid.ToString() renders the canonical form and ToString("N") the 32-digit form (stdlib 2.6, 2.18).
//! cites: 7.4, 7.5, 2.6
//! expect: run
//! output: 1 11111111-2222-3333-4444-555555555555
//!   1b 11111111222233334444555555555555
//!   2 11111111-2222-3333-4444-555555555555
//!   3b 00000000-0000-0000-0000-000000000000
//!   4b 11111111222233334444555555555555
//!   6 11111111222233334444555555555555
//!   6b none
//!   7 11111111-2222-3333-4444-555555555555
//!   7b null
module Conf;

public class Program
  public static method Mk(): Guid? => Guid.Parse("11111111-2222-3333-4444-555555555555");
  public static method Nn(): Guid? => null;
  public static method Early(g: Guid?): string
    if g = null then return "none"; end;
    return g.ToString("N");
  end Early;
  public static method Main()
    var g: Guid? := Program.Mk();
    var n: Guid? := Program.Nn();
    if g <> null then Console.WriteLine("1 " + g.ToString()); end;
    if g is not null then Console.WriteLine("1b " + g.ToString("N")); end;
    Console.WriteLine("2 " + g!.ToString("D"));
    Console.WriteLine("3b " + (n ?? Guid.Empty).ToString());
    case g of
      var y: Guid => Console.WriteLine("4b " + y.ToString("N")),
      null => Console.WriteLine("4c none")
    end;
    Console.WriteLine("6 " + Program.Early(g));
    Console.WriteLine("6b " + Program.Early(n));
    var q := g?.ToString();
    Console.WriteLine("7 " + (q ?? "null"));
    var q2 := n?.ToString();
    Console.WriteLine("7b " + (q2 ?? "null"));
  end Main;
end Program;
