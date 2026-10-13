# PPL records and input reconstruction

Six original reports are published as machine-path-redacted numeric derivatives under `data/ppl`. Their 47811 scored tokens,47812 stream tokens and 512/4096/24576 context settings remain unchanged. The offline verifier recomputes each window-NLL sum and exp(total_nll/scored_tokens). No new model scoring was performed. The old/new comparisons support deployment analysis, not task-completion improvement or a full standard corpus benchmark.

A post hoc source check matched the local input byte-for-byte to a prefix of [Salesforce WikiText-2 raw-v1](https://huggingface.co/datasets/Salesforce/wikitext/tree/b08601e04326c79dfdd32d625aee71d232d685c3/wikitext-2-raw-v1), fixed commit `b08601e04326c79dfdd32d625aee71d232d685c3`. Parquet SHA256: `5f1bea067869d04849c0f975a2b29c4ff47d867f484f5010ea5e861eab246d91`. Preserve all test rows in order, join them with one LF, normalize LF to CRLF, encode UTF-8 and keep the first 201377 bytes. The resulting SHA256 is `bf5b371fec7f2dcccdee81b8e0f81f0565c09421e7291a2f27fb32b9d0cfd6d6`. Full reconstructed local test text also matches its separately recorded hash. The standard nonraw-v1 candidate did not match and its failure remains in the release.

```powershell
python -m pip install pyarrow==20.0.0
python -B reconstruct_ppl_corpus.py --output C:/study/long_48k.txt
```

The published reconstruction program fetched the pinned public Parquet, verified the full/input hashes and produced the exact input in a new local path with zero model requests. This establishes reproducible input bytes; it does not retroactively supply an original pre-experiment source timestamp. Token counts were recorded by the original model/tokenizer and are not inferred from byte length. The source upstream retains its own dataset/license documentation; this release distributes reports and a reconstruction recipe, not raw dataset text or a new corpus license. Fresh PPL scoring still requires the exact separately provided artifacts and scoring executable/source build. The original reports bind artifact names/signatures, not a newly reconstructed engine build.
