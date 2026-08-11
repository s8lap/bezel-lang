public method ReportEachGeneric<T>(items: IEnumerable<T>) where T: class
  for var c in items do Console.WriteLine(c.ToString()); end;   // Object's surface only under `where T: class` (§14.2)
end ReportEachGeneric;

// Call with Array<Connection>: T inferred as Connection.
// Call with OwningArray<Connection>: T inferred as Connection.
// Same generic instantiation for both source forms.
