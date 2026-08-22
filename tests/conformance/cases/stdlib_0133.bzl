public abstract class Stream
  public abstract readonly property CanRead: bool;
  public abstract readonly property CanWrite: bool;
  public abstract readonly property CanSeek: bool;

  public abstract method Read(buffer: Array<byte>, offset: int, count: int): int;
  public abstract method Write(buffer: Array<byte>, offset: int, count: int);
  public method Read(buffer: Span<byte>): int;             // v0.165: span carries its own range
  public method Write(buffer: ReadOnlySpan<byte>);          //   (§4.14 convention: slice first)
  public abstract method Flush();

  public abstract method Seek(offset: long, origin: SeekOrigin): long;
  public abstract readonly property Length: long;
  public abstract property Position: long;

  public async virtual method ReadAsync(buffer: Array<byte>, offset: int, count: int,
                                         token: CancellationToken := CancellationToken.None): Task<int>;
  public async virtual method WriteAsync(buffer: Array<byte>, offset: int, count: int,
                                          token: CancellationToken := CancellationToken.None): Task;

  // Convenience methods
  public method ReadAll(): owned Array<byte>;            // reads from current position to end
  public async method ReadAllAsync(token: CancellationToken := CancellationToken.None): OwningTask<Array<byte>>;
end Stream;
