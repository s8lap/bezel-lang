public class Stopwatch
  public constructor();
  public static method StartNew(): owned Stopwatch;

  public readonly property IsRunning: bool;
  public readonly property Elapsed: TimeSpan;
  public readonly property ElapsedMilliseconds: long;

  public method Start();
  public method Stop();
  public method Reset();
end Stopwatch;
