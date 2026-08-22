public record Guid: IComparable<Guid>, IEquatable<Guid>
  // v0.165 (#1357): CompareTo = lexicographic over the 16 canonical big-endian
  // bytes (stated explicitly — .NET's field-group order is a known trap);
  // ordering is CompareTo only — no <,>,<=,>= overloads are declared.
  public method CompareTo(other: Guid): int;
  public static method FromByteArray(bytes: Array<byte>): Guid;   // borrows; requires Count = 16, ArgumentException otherwise
  public static readonly property Empty: Guid;

  public static method NewGuid(): Guid;
      // v0.231 pin: a version-4 (random) UUID — the version and variant bits are
      //   fixed (0100 / 10), the remaining 122 bits drawn from the runtime's
      //   entropy source (the §1 v0.204 replay-intercept point declared for this
      //   section). Uniqueness is probabilistic, not checked: no dedup, no
      //   cross-process coordination. The entropy source is NOT specified as
      //   cryptographically strong — §2.7's recorded posture: cryptographic
      //   randomness belongs to the future System.Security.Cryptography module —
      //   so NewGuid values must not serve as secrets (session tokens,
      //   capability strings).
  public static method Parse(s: string): Guid;      // accepts the "D" and "N" renderings (§2.18); FormatException otherwise (v0.195)
  public static method TryParse(s: string, out result: Guid): bool;
  public static method ParseOrNull(s: string): Guid?;   // v0.231 (§1 *OrNull tier): null exactly where
                                                        //   Parse(s) raises; otherwise the parsed value

  public method ToString(): string;          // canonical form
  public method ToString(format: string): string;   // "N", "D" — the §2.18 vocabulary; any other specifier raises FormatException (v0.195)
  public method ToByteArray(): owned Array<byte>;
end Guid;
