//! case: s7_4_0144
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: a branch that assigns null leaves the join un
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). A fact holds at a join only when it holds on every path.
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text;
class C
  public static method Main(): int
    var d: double? := 1.5;
    if Environment.ProcessorCount > 100000 then d := null; end;
    Console.WriteLine(Double.IsNaN(d));
    return 0;
  end Main;
end C;
