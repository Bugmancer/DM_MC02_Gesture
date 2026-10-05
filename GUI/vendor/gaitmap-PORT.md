# Gaitmap Trajectory Core

`../trajectory.py` adapts the MIT-licensed numerical implementation in
gaitmap 2.6.0, commit `3c6e508bb35fb32cb00495f68c5677e900a9ceaa`:

https://github.com/mad-lab-fau/gaitmap/blob/3c6e508bb35fb32cb00495f68c5677e900a9ceaa/gaitmap/trajectory_reconstruction/trajectory_methods/_kalman_numba_funcs.py

The port retains the nominal navigation equations, nine-dimensional
position/velocity/orientation error state, world-frame transition matrix,
zero-velocity correction, Joseph covariance update, backward RTS pass, and
final error-state correction. References used by upstream are D. Simon Colomar,
J. Nilsson and P. Handel, "Smoothing for ZUPT-aided INSs," and Joan Sola,
"Quaternion kinematics for the error-state Kalman filter."

Changes for this application:

- NumPy-only incremental API, avoiding gaitmap's pandas/Numba/tooling stack.
- Variable sample intervals, SI gyroscope input and wxyz quaternions.
- Gravity 9.80665 m/s^2 to match the board and Fusion AHRS.
- No level-walking or fixed-height update; all three translation axes remain free.
- Continuous-noise discretization and positive initial covariance replace
  upstream's fixed per-sample noise matrices and singular position covariance.
- Linear solves replace explicit inverses; symmetry is retained after updates.
- Online output subtracts the current error estimate from the nominal state.
- A soft zero-velocity endpoint assumption is applied when a stroke finishes.
- Start position is reanchored after smoothing, without forcing a closed loop.

This is an adaptation for short handheld strokes, not an upstream-validated
handwriting tracker. It assumes approximately zero initial and terminal speed;
releasing KEY while still moving can distort the reconstructed stroke. It does
not estimate accelerometer bias or recover unobservable constant velocity,
absolute heading or position. The algorithm still contains inertial prediction;
its improvement over unconstrained accumulation is the error-state estimation
and retrospective correction of the complete stroke.

The `gaitmap_mad` package is not included or used. Upstream's MIT license is
reproduced in `gaitmap-LICENSE.txt`.
