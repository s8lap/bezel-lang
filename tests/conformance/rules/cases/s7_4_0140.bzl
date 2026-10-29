//! case: s7_4_0140
//! doc: spec
//! section: 7.4
//! title: Narrowed nullable passes to non-nullable parameter: Char statics over a narrowed char local
//! rule: Narrowing is flow-sensitive: after `x := expr;` where expr's static type is non-nullable, x is narrowed until invalidated, and a `readonly` field narrows (spec 7.4, How narrowing is established). A narrowed nullable value converts to the non-nullable parameter type (spec 7.1). Char.ToLower, IsWhiteSpace, IsLetterOrDigit and IsPunctuation take a char (stdlib 9.5).
//! cites: 7.1, 7.4
//! expect: run
//! output: z
//!   False
//!   True
//!   False
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var c: char? := 'z';
    Console.WriteLine(Char.ToLower(c));
    Console.WriteLine(Char.IsWhiteSpace(c));
    Console.WriteLine(Char.IsLetterOrDigit(c));
    Console.WriteLine(Char.IsPunctuation(c));
    return 0;
  end Main;
end C;
