# Fresh Windows replication

Use Windows, Python 3.12 or later, pnpm 11.25.0, official Node 0.10.28/6.17.1/8.17.0 and a new output workspace. Exact Node hashes and official URLs are in runtime_recipes. Dependency installation disables scripts and uses frozen locks. A reference-only run executes the five original developer suites and negative scoring controls with simulated serving/generation/quiescence and zero model requests:

```text
python -B prepare_replication.py --workspace NEW_DIRECTORY --references-only --node01028 NODE01028_EXE --node6 NODE6_EXE --node8 NODE8_EXE --pnpm PNPM_COMMAND
```

The new directory must not already exist. Optional --reuse-dependencies-from points to a parent containing task-case directories with matching package.json/pnpm-lock.yaml and node_modules. Reference results have separate locks and are not model successes.

For model execution, omit --references-only and additionally provide --engine ENGINE_EXE --model36 MODEL36_NINFER --model38 MODEL38_NINFER --run-model. The preparer checks exact three asset hashes, creates new derivative freezes and an analysis plan, and runs a separately labeled 60-row replication. Provisioning matched checkpoints remains a prerequisite; weights and engine/Node executables are not included. Do not silently substitute an unmatched model or infer that a locally named checkpoint proves an upstream revision. Initial GPU admission≤700MiB, host≤50000MiB and scratch≥2GiB remain in the copied driver; it can stop before a row when admission fails. Already-started rows must not be reset or rerun to improve results.
