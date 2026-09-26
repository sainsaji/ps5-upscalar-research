# Big Buck Bunny, 720p web encode, 3x to 4K - difference statistics

Full frame, 3840x2160. Crop window: x=1060 y=1020 640x360.

| Mode | Mean abs diff vs Off (0-255) | Pixels changed by > 8 |
|---|---|---|
| Sharp (FSR1) | 2.12 | 3.4% |
| AI Standard (Anime4K S) | 1.90 | 3.7% |
| AI Large (Anime4K M) | 1.84 | 3.7% |
| AI Maximum (Anime4K UL) | 1.83 | 4.5% |

| Network pair | Mean abs diff | Pixels changed by > 8 |
|---|---|---|
| AI Large (Anime4K M) vs AI Standard (Anime4K S) | 0.79 | 0.23% |
| AI Maximum (Anime4K UL) vs AI Standard (Anime4K S) | 1.04 | 0.84% |
| AI Maximum (Anime4K UL) vs AI Large (Anime4K M) | 0.92 | 0.43% |
