//! case: s35_2_0001
//! doc: spec
//! section: 35.2
//! title: a choice body admits exactly what an enum body admits
//! rule: A choice body may declare, after the case list, exactly what an enum body may:
//!   instance methods, expression-bodied read-only instance properties, static
//!   methods, static readonly fields (including static owned readonly), const
//!   members, and operator overloads.
//! expect: accept
module Conf;

public class Cache
  public constructor()
  end constructor;
end Cache;

public choice Priority
  Low,
  Medium,
  High;

  public method IsUrgent(): bool => (case this of High => true, _ => false);

  public property Label: string => (case this of
    Low    => "low",
    Medium => "medium",
    High   => "high");

  public static method Default(): Priority => Medium;

  public const Max: int := 2;

  public static owned readonly Shared: Cache := new Cache();

  public static method operator =(a: Priority, b: Priority): bool =>
      (case a of
        Low    => (case b of Low => true, _ => false),
        Medium => (case b of Medium => true, _ => false),
        High   => (case b of High => true, _ => false));
  public static method operator <>(a: Priority, b: Priority): bool => not (a = b);
  public method Equals(other: Priority): bool => this = other;
  public method GetHashCode(): int => (case this of Low => 0, Medium => 1, High => 2);
end Priority;
