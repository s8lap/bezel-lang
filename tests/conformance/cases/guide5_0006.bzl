for var conn in pool do          // default iteration over an OwningArray: borrows
  conn.Dispose();                // ERROR: only owners dispose — no exception to this
end;
