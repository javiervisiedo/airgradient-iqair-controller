# AQI & Threshold Reference

## US EPA PM2.5 AQI (2024 revision, effective 2024-05-06)

| PM2.5 µg/m³ (truncated to 0.1) | AQI | Category | Colour |
| :--- | :--- | :--- | :--- |
| 0.0 – 9.0 | 0 – 50 | Good | `#00E400` |
| 9.1 – 35.4 | 51 – 100 | Moderate | `#FFFF00` |
| 35.5 – 55.4 | 101 – 150 | Unhealthy for Sensitive Groups | `#FF7E00` |
| 55.5 – 125.4 | 151 – 200 | Unhealthy | `#FF0000` |
| 125.5 – 225.4 | 201 – 300 | Very Unhealthy | `#8F3F97` |
| 225.5 – 325.4 | 301 – 500 | Hazardous | `#7E0023` |

`AQI = (Ihi − Ilo) / (Chi − Clo) × (C − Clo) + Ilo`, rounded to the nearest integer.

> [!NOTE]
> The pre-2024 table used 12.0 as the top of "Good". Some apps still use it.
> AirGradient's dashboard follows the current EPA table. Make sure tests use the 2024 values.

## Domain background on inputs (the spec decides which are used)

- **CO2**: purifiers don't remove it. It needs ventilation, not filtration.
- **Temperature/RH**: irrelevant to filtration. RH above 85% can inflate optical PM
  readings (AirGradient's `pm02Compensated` on the local API / `pm02_corrected` in cloud compensates for this).
- **tvocIndex / noxIndex**: enabled by default in v3.0 (`enable_multigas = true`) as a
  capped gas floor (IQAir GC MultiGas has activated carbon + KMnO4 cartridges).
