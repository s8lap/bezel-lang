// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for the
// imported_literal_target_false_reject_5328rv__*.bzl rows (review of issue
// #5328 / spec §15.4, v0.313). It carries its own trivial RUN line per the
// tests/lit convention (lit collects every `.bzl` under this directory, so a
// companion without one is reported UNRESOLVED — see tests/lit/README.md). The
// RUN line also earns its keep: it asserts the EXPORTER compiles, without
// which every importer row's result would be vacuous.
//
// The five formals below are the shapes this review probes for a FALSE REJECT
// — a §15.4-admitted or merely-legal target that the new imported-channel
// gate must NOT reject — plus the two user-declared types that separate the
// predicate's two branches (declaration-set vs builtin-name):
//
//   Arr      — `Array<int>`, §15.4-admitted, the plain heap-borrowing case.
//   Que      — `Queue<int>`, §15.4-admitted, a DIFFERENT builtin head than
//              Arr/St so the sweep covers more than one name.
//   OSt      — `OwningSet<Box>`, §15.4-admitted in the OWNING spelling, with
//              `where T: class` satisfied by the `Box` element. The
//              `Owning…` prefix is a second axis on which a name matcher
//              could go wrong.
//   IShape   — a USER-DECLARED interface. §15.4's closed list does not admit
//              it, so a literal here must be REJECTED — this row is the
//              positive twin, not a false-reject case.
//   IList    — a USER-DECLARED CLASS whose simple name collides with the
//              builtin collection INTERFACE `IList<T>`. It is a legitimate
//              §15.4.1 sequence-literal target (it declares the conformance
//              below), so rejecting it "because the name looks like an
//              interface" would be a false reject. This is the row that
//              separates the predicate's two branches.
module FR5328Lib;

public class Box
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public method V(): int => _v;
end Box;

public interface IShape
  method Area(): int;
end IShape;

public class IList
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method N(): int => _n;
  public static method FromCollectionLiteral(
      elements: ReadOnlyArraySpan<int>): owned IList
    var n := 0;
    for var e in elements do n := n + e; end for;
    return new IList(n);
  end FromCollectionLiteral;
end IList;

public class FROps
  public static method Arr(x: Array<int>): int => x.Count;
  public static method Que(q: Queue<int>): int => q.Count;
  public static method OSt(s: OwningSet<Box>): int => s.Count;
  public static method TakesShape(s: IShape): int => s.Area();
  public static method TakesIList(s: IList): int => s.N();
end FROps;
