# Study protocol

The 96-row schedule and a separate analysis program froze before the first formal task request. Six qualified defects contribute8 cells and 2 repetitions each. Task/reference qualification and transport checks preceded task generation. A seeded shuffled schedule (seed20261014) is retained in `source/operative_inputs/schedule.json`. Every attempted row is final; no rescue replay or best-of selection is allowed. Admission failures before an attempt may delay resumption without creating a new attempt.

| Cell | Interface | Maximum whole history pairs | Description |
|---|---|---:|---|
| J0C | literal JSON |0| concise |
| J0E | literal JSON |0| explicit |
| J2C | literal JSON |2| concise |
| J2E | literal JSON |2| explicit |
| N0C | native functions |0| concise |
| N0E | native functions |0| explicit |
| N2C | native functions |2| concise |
| N2E | native functions |2| explicit |

Both interfaces offer read_file, apply_edits, write_file, syntax_check and finish without phase gating. The latest turn repeats the requirement. No hidden test, reference implementation, shell or network is exposed to the model. Fault-localized editable paths are supplied, so this is not unrestricted repository discovery. Original full developer suites run once after interaction irrevocably closes; they never provide repair feedback.

Every cell preserves complete editable source as ordinary text. Exact token counting may remove only whole historical action-result pairs and then explicitly omit read views. Counts must equal the actual generation input token count. Native functions undergo full recursive schema validation and projection into a canonical action; the controller does not fill arguments, repair code or submit automatically. Empty or length-terminated output is discarded. Submission requires an actual edit and an explicit finish; full-suite passing is reported separately.

Common limits:600 seconds from before preparation/startup through terminal acceptance and primary result serialization,90 seconds reserved for terminal acceptance,120 seconds startup,30 seconds cancellation barrier,24 tools,32 generations,4 edits after syntax feedback,8192 output tokens,24576 context tokens with 256 safety tokens. GPU cap 15872 MiB; host-used cap 56320 MiB. Admission requires GPU occupancy at most 700 MiB, host usage at most 50000 MiB and at least2 GiB free scratch space. All owned native validation costs count in the budget. Shutdown/reconciliation/archiving are timed separately and cannot add repair opportunities. Syntax checking uses Node 8 even where original Express behavior runs under Node 0.10; a syntax pass therefore does not attest legacy-runtime execution compatibility.

One MSE-labelled artifact and one RTX5080 workstation are held fixed. Thinking and speculation are disabled; temperature0, greedy seed0, presence penalty1.5, frequency penalty0, top_k20, top_p0.8 and min_p0 are common. The bound configuration and actual server logs remain authoritative.

Analysis retains96 planned rows,12 per cell. Each factor has 48 matched comparisons holding task, repeat and the other factors fixed. Repeat concordance is reported across 48 task-cell pairs. These are descriptive contrasts in six selected related defects, not population causal estimates or significance tests.
