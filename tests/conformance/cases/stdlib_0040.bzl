public class StringBuilder
  public constructor();
  public constructor(capacity: int);
  public constructor(initial: string);

  public readonly property Length: int;
  public pure readonly property Capacity: int;

  public method Append(value: string): StringBuilder;
  public method Append(value: char): StringBuilder;
  public method Append(value: Object): StringBuilder;
  // v0.165 (#1355): per-type overloads for bool and the numeric primitives —
  // observable behavior = Append(value.ToString()); no Object boxing on the path.
  public method Append(value: bool): StringBuilder;
  public method Append(value: int): StringBuilder;      // + byte/sbyte/short/ushort/uint/long/ulong/float/double/decimal, same shape
  public method Append(value: ReadOnlyStringSpan): StringBuilder;   // v0.222 (#1810): copies the span's
                                                                    //   code points; = Append(value.ToString())
                                                                    //   without the intermediate string
  public method AppendLine(): StringBuilder;
  public method AppendLine(value: string): StringBuilder;
  public method AppendFormat(format: string, args: Array<Object>): StringBuilder;

  public method Insert(index: int, value: string): StringBuilder;
  public method Remove(startIndex: int, length: int): StringBuilder;
  public method Replace(oldValue: string, newValue: string): StringBuilder;
  public method Clear(): StringBuilder;

  public method ToString(): string;
end StringBuilder;
