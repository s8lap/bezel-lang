# v0.345 landing data

Not part of the language's history: the data the v0.345 spec train needs when its review branch (`spec/v0.345`)
is merged — the changelog entry with tracker placeholders (`v0345/entry.md`), the trackers and questions to file
(`trackers.json`, `questions.json`), the per-issue comments and relabels, the lane and critic reports, and the
landing scripts (`file_issues2.py`, `finalize.py`, `post_land.py`, `post_labels.py`).

At merge: file the trackers (`file_issues2.py`), replace each "(tracker `KEY`, filed when this revision merges)"
in the v0.345 changelog entry with its issue link, then run `post_land.py` and `post_labels.py --write`.
