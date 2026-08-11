public record Digest(Rows: int, Bytes: long);        // sendable value record

public sealed class Batch                            // sendable: sealed, wholly-owned state
  public owned Rows: OwningArray<Row>;               // Row: itself a sealed, sendable class
  public constructor(owned rows: OwningArray<Row>) Rows := rows; end constructor;
end Batch;

public static method Summarize(owned batch: Batch): Digest
  using var b := move batch;                         // adopt (Guide 1 §9)
  return new Digest(b.Rows.Count, 0);
end Summarize;

// In the spawning domain:
using var join := Domain.Spawn(Summarize, new Batch(LoadRows()));  // the batch MOVES in
var digest := await join;                            // value result: copied back at the join
