// Compiler-intrinsic value record; module System.Interop (NOT auto-imported).
// Representation identical to IntPtr. T is gated (per-instantiation, language
// spec §3.6-style diagnostic) to the FFI-POINTEE SET: integer types, float,
// double, bool, IntPtr, UIntPtr — and C-compatible records (language spec
// §32.13). Nested Ptr<Ptr<T>> is NOT admitted (language spec §32.12).
public record Ptr<T>
  public static readonly property Zero: Ptr<T>;        // the null pointer, like IntPtr.Zero

  public static method FromIntPtr(p: IntPtr): Ptr<T>;  // the only untyped->typed entry: EXPLICIT
  public method ToIntPtr(): IntPtr;

  // Every member below is TRUSTED BUT UNVERIFIED (language spec §32's preamble
  // posture, verbatim): the address's validity, alignment, and pointee type are
  // the programmer's declaration; a wrong one is undefined behavior.
  public method Load(): T;                             // read  *p
  public method Store(value: T);                       // write *p
  public method LoadAt(index: int): T;                 // read  p[index]  (index scaled by T's ABI stride)
  public method StoreAt(index: int, value: T);         // write p[index]
  public method Add(count: int): Ptr<T>;               // p + count elements; negative counts allowed

  public method CopyToArray(count: int): owned Array<T>;   // count elements, copied Bezel-side
  public method CopyFromArray(source: Array<T>);           // source.Count elements written at p

  // Equality is declared explicitly, NOT left to record synthesis — the §10.0
  // IntPtr precedent, for the same reason: Ptr<T> has no auto-properties, so the
  // synthesized `=` would be the zero-property "every-pair-equal" form. The
  // intrinsic compares the address representation.
  public method Equals(other: Ptr<T>): bool;           // §25.2 co-declaration rule: a record
  public override method GetHashCode(): int;           //   overloading homogeneous =/<> declares
                                                       //   both. §25.2 admits no intrinsic
                                                       //   exemption; §10.0's IntPtr/UIntPtr
                                                       //   declare the same four members.
  public static method operator =(a: Ptr<T>, b: Ptr<T>): bool;
  public static method operator <>(a: Ptr<T>, b: Ptr<T>): bool;
end Ptr;
