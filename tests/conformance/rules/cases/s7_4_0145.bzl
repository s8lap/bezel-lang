//! case: s7_4_0145
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: a mutably captured local is not narrowed in t
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). A lambda reads a captured var local at its declared type.
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := 1.5;
    var f := lambda(): bool => Double.IsNaN(d);
    d := null;
    Console.WriteLine(f());
    return 0;
  end Main;
end C;
