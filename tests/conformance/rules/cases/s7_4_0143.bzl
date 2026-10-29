//! case: s7_4_0143
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: a loop whose body reassigns null invalidates 
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). A reassignment from null inside the loop reaches the loop head by the back edge.
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method Main(): int
    var d: double? := 1.5;
    var i := 0;
    while i < 3 do
      Console.WriteLine(Double.IsNaN(d));
      d := null;
      i := i + 1;
    end;
    return 0;
  end Main;
end C;
