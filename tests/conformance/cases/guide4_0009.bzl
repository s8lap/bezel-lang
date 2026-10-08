var cfg := await FetchConfigAsync();    // works: the task is pinned to a
                                        //   hidden slot until the end of this block;
                                        //   cfg borrows the Config through it
Render(await FetchConfigAsync());       // also fine: borrow confined to the statement,
                                        //   the task disposed at its end
