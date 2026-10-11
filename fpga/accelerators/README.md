# Optional tree-model source export

`export_conifer.py` converts a supplied, trusted XGBoost `Booster.save_model`
JSON file into an unrolled Conifer HLS source project. It supplies no trained
model, features, weights, trading decision, transport adapter or board shell.
No export, compilation, inference, simulation or synthesis was run while adding
this adapter; its API calls were checked against the pinned source only.

The dependency is Conifer commit
`29b60872bfa94d182637280355c1d993b8742050`, already present in the optional
`accelerators` source profile in `fpga/dependencies.lock.json`. Fetching only
that source is sufficient for this adapter:

```sh
python3 scripts/fpga_deps.py fetch conifer
```

The exporter imports this clean checkout directly and checks its commit before
loading it. It never installs packages. Use an independently managed, locked
Python environment containing XGBoost, NumPy, pandas, SciPy, packaging and
pybind11. The exact XGBoost release must match both `--xgboost-version` and the
version recorded by the saved model. Model producers must retain their own
dependency lock and training provenance. All installed package versions used
by the exporter are recorded in its metadata; they are not selected or changed
by this project.

Example invocation for a model saved by XGBoost 1.7.6:

```sh
python3 fpga/accelerators/export_conifer.py \
  --model /path/to/trusted_frozen_model.json \
  --output /path/to/new_hls_sources \
  --xgboost-version 1.7.6 \
  --precision 'ap_fixed<24,10,AP_RND_CONV,AP_SAT>' \
  --part "$BOARD_PART" --clock-ns 4 \
  --dense-finite-features
```

Set `BOARD_PART` to the exact device identifier from the licensed board project.
The example precision and requested period are configuration examples, not
validated numerical or timing recommendations. The precision applies to inputs,
thresholds, weights and accumulated scores. This adapter accepts signed widths
up to 64 bits, with at least two integer bits so the one-hot split weights can
represent one. Rounding (`AP_TRN` or `AP_RND_CONV`) and overflow (`AP_WRAP` or
`AP_SAT`) must be explicit. Range and rounding errors remain the responsibility
of numerical validation before use.

The source-reviewed path uses the XGBoost converter's `convert()` and the HLS
writer's `auto_config()`, `make_model()` and `write()`. The writer is loaded from
its exact pinned file: the upstream HLS package initializer imports absent
`PynqAlveoDriver`/`XrtDriver` names and imports PYNQ even when no board runtime is
needed. This adapter bypasses that initializer without changing the checkout or
installing a substitute driver. No calls to `compile`, `build` or
`decision_function` are made. The result includes the HLS kernel,
model parameters and upstream build scripts, plus `export_metadata.json` with
the source model hash, package versions, feature order and requested precision.
The output directory must not exist. A failed write retains an
`EXPORT_INCOMPLETE` marker and must not be integrated. Generated build scripts
remain inactive until invoked separately. The adapter disables their simulation
options, but a later explicit invocation still requests synthesis.
Output inside the pinned checkout is rejected. Custom project names must use
the `nexus_` prefix and an identifier suffix without double underscores, avoiding
C++ keywords and the upstream writer's fixed helper/type names.

The pinned writer emits an obsolete third `tree_scores` argument in its header
and testbench, while its implemented top takes only input and score arrays. The
adapter repairs that declaration with an exact source anchor and omits the
obsolete testbench and its build-script reference. It does not substitute an
unvalidated testbench. Both adjustments are recorded in the export metadata;
the pinned dependency checkout is unchanged.

## Supported model and integration contract

- Only numeric `gbtree` ensembles with one output are supported, using
  `binary:logistic` or `reg:squarederror`. DART, categorical splits, multiclass,
  vector leaves and parallel-tree forests are rejected.
- The entire saved tree ensemble is exported. Early-stopping metadata is
  rejected: freeze the intended tree subset as a separate artifact and remove
  obsolete stopping attributes before exporting it.
- Missing-value routing is absent from the pinned Conifer converter. The
  enclosing feature pipeline must reject missing, NaN and infinite inputs and
  supply dense numeric values in the recorded order. `--dense-finite-features`
  declares this requirement; it does not implement it or inspect a dataset.
- The pre-2.0 XGBoost conversion path substitutes a zero raw base score.
  Accordingly, this adapter accepts older binary models only with base score
  0.5, and older regression models only with base score zero. It also checks the
  converted base score against the supplied model.
- The pinned converter warns about numerical disagreements with XGBoost 2.0
  and later. Successful source export is not evidence of numerical agreement.
  Fixed-point threshold rounding, score accumulation and boundary cases must
  be assessed against the exact producer model before deployment.
- Only score element zero is meaningful for these supported objectives.
  Binary classification produces a raw margin, not a probability; no sigmoid,
  feature scaling, calibration or decision threshold is added.
- The kernel uses the upstream array-based HLS function boundary, not the
  project's 64-bit market-data stream. Feature extraction, framing, clock
  crossing, backpressure, DMA and result ownership require a separate adapter.
  The configured part does not supply UL3524 transceiver, pin or timing
  constraints. No clock frequency, resource budget or latency is claimed.

VHDL export is deliberately excluded from this source-only command. In the
pinned implementation, constructing `VHDLModel` constructs
`FixedPointConverter`, invokes `g++`, and loads a compiled conversion helper.
Offering that backend as a write-only operation would conceal a compilation
step. No replacement fixed-point conversion implementation is introduced here.

Conifer's main license is Apache-2.0; retained output keeps upstream notices and
receives `LICENSE.conifer`. Component notices must also be reviewed: the omitted
upstream testbench carries GPL-3.0-or-later, and the separately licensed Conifer
FPU is not used. Coyote remains a separately pinned platform reference: this adapter
does not imply GPU peer-to-peer, RDMA or a common shell with the network path.

Source references:

- [Pinned XGBoost converter](https://github.com/thesps/conifer/blob/29b60872bfa94d182637280355c1d993b8742050/conifer/converters/xgboost.py)
- [Pinned HLS writer](https://github.com/thesps/conifer/blob/29b60872bfa94d182637280355c1d993b8742050/conifer/backends/xilinxhls/writer.py)
- [Pinned fixed-point helper](https://github.com/thesps/conifer/blob/29b60872bfa94d182637280355c1d993b8742050/conifer/utils/fixed_point.py)
- [XGBoost 1.7 model-loading API](https://xgboost.readthedocs.io/en/release_1.7.0/python/python_api.html#xgboost.Booster.load_model)
