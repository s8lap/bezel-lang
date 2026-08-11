public class DomainFaultedException: Exception
  public readonly property FaultStackTrace: string;   // stack trace text of the original exception
  // Message carries the original exception's message text.
end DomainFaultedException;
