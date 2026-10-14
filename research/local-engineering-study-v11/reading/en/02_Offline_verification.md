# Offline verification

Python 3.11 or later is sufficient. No GPU, model, private drive, third-party package or network is needed after download and extraction. Run from the extracted bundle:

```text
python -B verify_public_evidence.py --self-test
```

The verifier checks the complete public inventory and hashes, original-score recomputation, 60 locks/nonces, per-row clocks, emitted-sample cutoffs/peaks, engine/client request and usage bindings, native projection without argument repair, effective modes, aggregate denominators, matched pairs and exact engine-source bytes. It rejects altered sampled peaks and duplicate native keys. Unknown client usage remains null; prospectively allowed incomplete traces stay failed. Passing offline verification does not rerun the models or establish external human review.

The original protected manifests under source/operative_inputs identify private-origin bytes. Deidentified source is not directly executable before asset/path/dependency rebinding; use prepare_replication.py for fresh Windows work.
