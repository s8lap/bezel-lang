public class Random
  public constructor();
  public constructor(seed: long);

  public method Next(): int;                    // 0..int.MaxValue
  public method Next(maxValue: int): int;       // 0..<maxValue
  public method Next(minValue: int, maxValue: int): int;
  public method NextLong(): long;
  public method NextDouble(): double;           // [0.0, 1.0)
  public method NextBytes(buffer: Array<byte>);
  public method NextBoolean(): bool;
end Random;
