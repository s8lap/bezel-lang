public sealed class Type
  public readonly property Name: string;            // unqualified: "BankAccount"
  public readonly property FullName: string;        // qualified: "BankingApp.BankAccount"

  // Kind predicates. Exactly one of IsClass, IsInterface, IsRecord, IsEnum,
  // IsPrimitive, IsInlineArray, IsFunctionType is true for any Type.
  public readonly property IsClass: bool;
  public readonly property IsInterface: bool;
  public readonly property IsRecord: bool;
  public readonly property IsEnum: bool;
  public readonly property IsPrimitive: bool;       // int, bool, char, float, ...
  public readonly property IsInlineArray: bool;     // [N]T, [N owned]T?, ...
  public readonly property IsFunctionType: bool;    // (A, B) -> R
  public readonly property IsValueType: bool;       // = IsPrimitive or IsEnum
                                                    //   or IsRecord or IsInlineArray

  // Lattice queries.
  public readonly property BaseType: Type?;         // direct superclass; null for
                                                    //   Object, interfaces, value
                                                    //   types, function types
  public method Implements(iface: Type): bool;      // true if this type conforms to
                                                    //   the interface `iface`
  public method IsAssignableFrom(other: Type): bool;// true if a value of type
                                                    //   `other` is assignable to a
                                                    //   location of this type

  // Enum metadata. Valid only when IsEnum; on any other Type these raise
  // InvalidOperationException.
  public readonly property EnumNames: IReadOnlyList<string>;  // member names, declaration
                                                              //   order; read-only view (§4.1)
  public readonly property EnumValues: IReadOnlyList<long>;   // member values, same order
                                                      //   (widened to long; ulong-underlying
                                                      //   enums bit-reinterpret — see below)
end Type;

public method DescribeEnum(t: Type)
  if not t.IsEnum then return; end;
  var names := t.EnumNames;
  var values := t.EnumValues;
  for var i in 0..<names.Count do
    Console.WriteLine($"{names[i]} = {values[i]}");
  end;
end DescribeEnum;
