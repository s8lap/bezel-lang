public interface IBankAccount: IAudited
  method Deposit(amount: decimal);        // interface members carry NO visibility
  readonly property Id: string;           // getter-only;  `property Balance: decimal;` is read-write
  property [i: int]: string;              // indexer
  static method Parse(s: string): IBankAccount;             // §11.8, abstract-only
end IBankAccount;

record Point(X: int, Y: int);             // positional-only record: ends with `;`
record Money(Amount: decimal, Cur: string serialized "currency" equality skip);
record Rectangle                          // body record
  public readonly property Width: decimal;
  public constructor(w: decimal)  Width := w;  end constructor;
  public method Area(): decimal => Width * Width;
end Rectangle;
enum Priority
  Low, Medium, High;                      // the constant list ends with `;`
  public method IsUrgent(): bool => this = High;
  // compiler-provided on EVERY enum (v0.293): Priority.FromUnderlyingOrNull(u): Priority?, Priority.FromNameOrNull(s): Priority?
  // (declared identifier, exact), Priority.NameOf(v): string = nameof(Priority.X) for a written X. ToString() renders the TYPE name.
end Priority;
enum HttpStatus: short  Ok := 200, NotFound := 404;  end HttpStatus;   // underlying type

public choice FetchOutcome
  Fetched(owned response: Response),
  NotModified,
  Failed(status: int, message: string);   // the case list ends with `;`
  public method IsOk(): bool => (case this of Fetched(_) => true, _ => false);
end FetchOutcome;

extension string                          // module level; public | internal (default internal)
  public property IsBlank: bool => this.Length = 0;
end;                                      // `extern "libc" convention "cdecl" ... end;` declares FFI entries
