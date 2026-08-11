public enum Status
  Active, Draining, Stopped;
end Status;

public record Point(X: int, Y: int);              // structural =, with(…)
var p2 := p1 with(Y := 99);

public choice FetchOutcome                        // closed alternatives, payloads
  Fetched(body: string),
  NotModified,
  Failed(code: int);
end FetchOutcome;

public class Session                               // identity + owned resource
  private owned _log: FileStream;
  // … constructor, methods, disposal cascade
end Session;
