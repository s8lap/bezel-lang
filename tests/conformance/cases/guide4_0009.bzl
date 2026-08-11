var cfg := await FetchConfigAsync();    // ERROR: cfg would outlive the temporary
                                        //   task that owns the Config
Render(await FetchConfigAsync());       // OK: the borrow stays inside the statement,
                                        //   the task is disposed at its end
