//! case: s7_4_0149
//! doc: spec
//! section: 7.4
//! title: Nullable passed to non-nullable parameter is rejected: an un-narrowed nullable int passed to StringB
//! rule: Narrowing is invalidated when the variable is reassigned from a nullable-typed expression, is passed as `ref`/`out`, or is mutably captured (spec 7.4, How narrowing is invalidated); an un-narrowed nullable argument for a non-nullable parameter is a compile error (spec 7.1). StringBuilder declares no Append overload over a nullable value (stdlib 3.1).
//! cites: 7.1, 7.4
//! expect: reject
//! diagnostic: nullable
module M; import System; import System.Text; import System.Threading.Tasks;
class C
  public static method F(x: int?): int
    var sb := new StringBuilder();
    sb.Append(x);
    Console.WriteLine(sb.ToString());
    return 0;
  end F;
  public static method Main(): int
    return F(null);
  end Main;
end C;
