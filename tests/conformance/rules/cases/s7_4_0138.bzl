//! case: s7_4_0138
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: a conjunction of null tests narrows char and int
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). In `c <> null and i <> null` both are narrowed in the true branch; StringBuilder.Append(char) and Append(int) apply (stdlib 3.1).
//! cites: 7.1, 7.4
//! expect: run
//! output: skip
//!   skip
//!   712
//!   True
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method F(c: char?, i: int?): int
    var sb := new StringBuilder();
    if c <> null and i <> null then
      sb.Append(c); sb.Append(i);
      Console.WriteLine(sb.ToString());
      Console.WriteLine(Char.IsDigit(c));
    else
      Console.WriteLine("skip");
    end;
    return 0;
  end F;
  public static method Main(): int
    F(null, 1); F('7', null); F('7', 12);
    return 0;
  end Main;
end C;
