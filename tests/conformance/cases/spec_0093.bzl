public sealed class Type
  public readonly property Name: string;            // "BankAccount"
  public readonly property FullName: string;        // "BankingApp.BankAccount"

  // Kind predicates — see stdlib §2.13 for the full set and the exact mapping
  // to the §3.1 type kinds.
  public readonly property IsClass: bool;
  public readonly property IsInterface: bool;
  public readonly property IsRecord: bool;
  public readonly property IsEnum: bool;
  public readonly property IsChoice: bool;
  public readonly property IsValueType: bool;

  // Lattice queries.
  public readonly property BaseType: Type?;
  public method Implements(iface: Type): bool;
  public method IsAssignableFrom(other: Type): bool;

  // Enum metadata (valid only when IsEnum; otherwise raises
  // InvalidOperationException).
  public readonly property EnumNames: IReadOnlyList<string>;
  public readonly property EnumValues: IReadOnlyList<long>;
end Type;
