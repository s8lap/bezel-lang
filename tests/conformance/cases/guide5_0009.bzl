for var entry in sessions.Drain() do
  using var e := move entry;     // DrainedEntry owns its Value; adopting disposes both
  Archive(e.Key, e.Value);       // e.Value: the owned Session, borrowed by Archive
end;
