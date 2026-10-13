# Reproduction

The offline verifier checks published file inventories/hashes, original score recomputation, nonces and derived run locks, declared measured resource scope, client unknown usage, effective server settings, native function projection and full denominators. `--self-test` rejects an altered sampled GPU peak and duplicate native arguments. It does not rerun a model, build UE or certify omitted assets.

```powershell
python -B verify_public_evidence.py --self-test
python -B prepare_reproduction.py --references-only --workspace C:/study/new_references --node8 C:/assets/node8.exe --node010 C:/assets/node010.exe --pnpm C:/tools/pnpm.cmd
python -B prepare_reproduction.py --workspace C:/study/new_replication --engine C:/assets/ninfer-serve.exe --model C:/assets/qwen3_8_27b_q4head_mtp_q4emb_mse.ninfer --node8 C:/assets/node8.exe --node010 C:/assets/node010.exe --pnpm C:/tools/pnpm.cmd
```

Fresh preparation requires Windows, Python 3.12+, official Node 8.17.0/Node 0.10.48 and pnpm11.25.0. The --references-only command does not require or open model/engine assets and passed all six original reference suites in a separate actual preparation. It still uses the Windows/NVIDIA monitoring scaffold; it is not a cross-platform model replay. For model execution, additionally supply exact hashed engine/model assets. Use an unused workspace and port18561; the original study used18560. The preparer never overwrites an existing workspace, stops foreign processes or opens the original author's drive paths. It verifies supplied asset hashes, installs six recorded dependency recipes with scripts disabled, rebinds paths, and creates explicitly separate replication freezes. It exercises real edits, syntax and all six reference suites with simulated serving/generation. These reference checks make zero model requests.

Add `--run-model` only when ready to execute the separately identified96-row model replication. It will create fresh candidate/serving processes, retain unsuccessful rows and run its separately bound analysis after completion. If pre-attempt resource admission interrupts the schedule, resume with `python -B C:/study/new_replication/inputs/driver.py`, then run `python -B C:/study/new_replication/analysis/analyse.py`. The driver skips completed locks; never delete them to retry failed attempts.

The authors actually prepared fresh dependencies in a separate directory and passed six reference suites plus configuration/analysis binding. The first preparation encountered an occupied port before a model request; its failure remains disclosed. This is same-machine source/dependency reconstruction with simulated generation, not a full public model rerun, independent hardware validation or human review. An optional `--reuse-dependencies-from` facility accepts only matching package and lock hashes from a previous prepared workspace; published reference verification used it after the port correction.

Hash verification does not supply missing model/engine assets. See the asset boundaries before expecting GPU replication to start. Downloaded existing XiaoManBox product weights are not asserted to equal the study MSE artifact.
