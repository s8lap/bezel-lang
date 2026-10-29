//! case: s7_4_0146
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: a nullable-returning call result is not narro
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). A call's static type is nullable and establishes no fact.
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  static method G(): double?
    return 1.5;
  end G;
  public static method Main(): int
    Console.WriteLine(Double.IsNaN(G()));
    return 0;
  end Main;
end C;
