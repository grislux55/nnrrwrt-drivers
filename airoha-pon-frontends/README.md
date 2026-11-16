# Airoha PON optical frontends

| Provider | Devices | Calibration source |
| --- | --- | --- |
| `airoha-en7572` | EN7572 | 512-byte BOB NVMEM cell `calibration`; optional `calibration-magic` and `calibration-crc` cells for the stock record check |
| `airoha-paged-bosa` | GN28L95, UX3363 | `APONCAL` NVMEM cell or board firmware |