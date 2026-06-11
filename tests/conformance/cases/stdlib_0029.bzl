public class StringBuilder
  public constructor();
  public constructor(capacity: int);
  public constructor(initial: string);

  public readonly property Length: int;
  public readonly property Capacity: int;

  public method Append(value: string): StringBuilder;
  public method Append(value: char): StringBuilder;
  public method Append(value: Object): StringBuilder;
  public method AppendLine(): StringBuilder;
  public method AppendLine(value: string): StringBuilder;
  public method AppendFormat(format: string, args: Array<Object>): StringBuilder;

  public method Insert(index: int, value: string): StringBuilder;
  public method Remove(startIndex: int, length: int): StringBuilder;
  public method Replace(oldValue: string, newValue: string): StringBuilder;
  public method Clear(): StringBuilder;

  public method ToString(): string;
end StringBuilder;
