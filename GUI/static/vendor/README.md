# Local browser dependencies

The UI makes no runtime requests to a CDN.

| Library | Version | Source | License |
| --- | --- | --- | --- |
| Lucide | 0.468.0 | https://unpkg.com/lucide@0.468.0/dist/umd/lucide.min.js | ISC; see lucide.LICENSE |
| Chart.js | 4.4.7 | https://unpkg.com/chart.js@4.4.7/dist/chart.umd.js | MIT; see chartjs.LICENSE.md |
| Three.js / OrbitControls | 0.180.0 | https://unpkg.com/three@0.180.0/ | MIT; see three.LICENSE |

OrbitControls' `three` import is changed to `./three.module.js` for local
module resolution. All other vendor code is unchanged.

`../board.png` is copied from the board vendor's local DM-MC02 reference
materials (`dm-mc02/image/DM_H7.png`); its original usage terms apply.
