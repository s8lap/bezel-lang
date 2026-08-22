/// Compiler-intrinsic; pointer-sized integer (32 or 64 bits depending on target).
public record IntPtr
  public static readonly property Zero: IntPtr;

  // §25.2's co-declaration rule, with no intrinsic exemption (retired v0.275):
  // a record overloading homogeneous =/<> declares Equals(T) and
  // `override GetHashCode` beside them. The homogeneous overload already
  // strips auto-hashable (§9.2's membership list and its third disqualifier,
  // ruled v0.276), so these two members ADD the correct address-based equality
  // surface rather than replacing a synthesized one -- ADDING them reclassifies
  // no type, because the operators already did. (A record with an `IntPtr`
  // component is not auto-hashable under that ruling; see the language spec
  // §9.2 recursive component rule and the v0.276 changelog entry's stated cost.)
  // The §10.5 `Ptr<T>` spelling, verbatim.
  public method Equals(other: IntPtr): bool;
  public override method GetHashCode(): int;

  public static method operator =(a: IntPtr, b: IntPtr): bool;
  public static method operator <>(a: IntPtr, b: IntPtr): bool;
end IntPtr;

/// Compiler-intrinsic; unsigned pointer-sized integer.
public record UIntPtr
  public static readonly property Zero: UIntPtr;

  public method Equals(other: UIntPtr): bool;        // §25.2 co-declaration set, as `IntPtr` above
  public override method GetHashCode(): int;

  public static method operator =(a: UIntPtr, b: UIntPtr): bool;
  public static method operator <>(a: UIntPtr, b: UIntPtr): bool;
end UIntPtr;
