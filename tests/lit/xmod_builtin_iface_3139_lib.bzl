// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for xmod_builtin_iface_conformance_reject_3139.bzl.
// It carries its own trivial RUN line per the tests/lit convention (see
// xmod_static_interface_2294_lib.bzl): lit collects every `.bzl` under this
// directory, so a companion without one is reported UNRESOLVED. The RUN line
// also earns its keep — it asserts that the EXPORTER compiles, without which
// every reject the importer file pins would be meaningless.
//
// Every declaration here names a built-in interface that has no user
// TypeSymbol, which is precisely the fact that could not cross a module
// boundary before #3139 (family F1 of #3152).
module LibF1Lit3139;

// §18.5 / stdlib §2.18 — the auto-imported `IFormattable` opt-in.
public class Money3139: IFormattable
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public method ToString(format: string): string => "MX";
end Money3139;

// §14.2 — `IComparable<T>`, a built-in the compiler DOES synthesize a
// TypeSymbol for (#1115/#2776).
public class Rank3139: IComparable<Rank3139>
  private _r: int;
  public constructor(r: int) _r := r; end constructor;
  public method Rank(): int => _r;
  public method CompareTo(other: Rank3139): int => _r - other.Rank();
end Rank3139;

// The negative control: no conformance at all.
public class Plain3139
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public method V(): int => _v;
end Plain3139;

// §9.8 — a serializable positional record, for the interlock case.
public record Pt3139(X: int, Y: int): ISerializable, IDeserializable
end Pt3139;
