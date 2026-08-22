await for var t in Task.WhenEach(move tasks) do         // t: owned OwningTask<Report>
  using var report := await t;                          // Pattern B: result transfers
  Publish(report);
end;
