// RUN: %bezelc -c -o %t.o %s
//
// spec §14.2, v0.338 / §26.4, v0.338 / #5805 follow-up: the over-rejection
// guard for the record/enum/choice rule on extensions. A record as the extension
// TARGET (`extension IEnumerable<Holder.Point>`, a concrete instantiation: a
// qualified name is not a bare identifier, so it binds no block parameter —
// §26.4) is not a bound and stays admitted, as does a record nested in a generic
// argument of the target; a record as a TYPE ARGUMENT inside a bound
// (`where T: IComparable<Holder.Point>`) names the interface, not the record,
// and stays admitted; the remedy `where T: value` (spec :1024 and the §14.2
// table) admits records, enums and choices; `where T: class` and
// `where T: notnull` stay admitted.

module M;

public static class Holder
  public record Point(X: int, Y: int);
end Holder;

public extension IEnumerable<Holder.Point>
  public method A(): int => 1;
end;

public extension IEnumerable<Array<Holder.Point>>
  public method B(): int => 2;
end;

public extension IEnumerable<T> where T: value
  public method C(): int => 3;
end;

public extension IEnumerable<T> where T: class
  public method D(): int => 4;
end;

public extension IEnumerable<T> where T: notnull
  public method E(): int => 5;
end;

public extension IEnumerable<T> where T: IComparable<Holder.Point>
  public method F(): int => 6;
end;

public extension IEnumerable<T>
  public method G<U>(x: U): int where U: value => 7;
end;
