public class TimeoutException: Exception
  public constructor();
  public constructor(message: string);
  public constructor(message: string, inner: Exception);   // borrows inner; snapshot copied into CauseChain (spec §31.4, v0.162) — no reference retained
end TimeoutException;
