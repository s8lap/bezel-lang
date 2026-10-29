//! case: s7_4_0148
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: a ref argument invalidates the narrowing
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). A `ref` argument may reassign the local, uniformly.
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  static method Kill(ref d: double?): int
    d := null;
    return 0;
  end Kill;
  public static method Main(): int
    var d: double? := 1.5;
    Console.WriteLine(Double.IsNaN(d));
    Kill(ref d);
    Console.WriteLine(Double.IsNaN(d));
    return 0;
  end Main;
end C;
