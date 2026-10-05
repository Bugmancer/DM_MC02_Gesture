"""KEY lifecycle and world-frame output for the gaitmap-derived RTS tracker.

Attitude comes from the continuously running Fusion filter at stroke start.
The trajectory core consumes raw body acceleration (including gravity) and
angular rate. Release supplies an approximate zero end velocity and smooths
the whole stroke; it does not force the endpoint back to the starting point.
"""

from collections import deque

import numpy as np

from trajectory import IncrementalRtsTrajectory


UINT32_MASK = 0xFFFFFFFF
MAX_SAMPLE_GAP_S = 0.1
MAX_STROKE_SAMPLES = 12000  # One minute at the board's 200 Hz sample rate.
MAX_POSITION_M = 100.0


class PenMotion:
    def __init__(self):
        self._stroke_id = 0
        self.reset()

    def reset(self):
        self._position = np.zeros(3)
        self._velocity = np.zeros(3)
        self._origin = np.zeros(3)
        self._trajectory = None
        self._corrections = deque(maxlen=8)
        self._last_t = None
        self._released = False
        self.recording = False
        self.blocked = False
        self.reason = "release_required"

    def _halt(self, reason):
        self._velocity.fill(0)
        self._trajectory = None
        self._released = False
        self.recording = False
        self.blocked = True
        self.reason = reason

    def invalidate(self, reason="sample_gap"):
        self._halt(reason)
        return self._position.tolist()

    @staticmethod
    def _parse(t_ms, acceleration, gyroscope, quaternion, stationary, key_down):
        try:
            if (isinstance(t_ms, bool) or int(t_ms) != t_ms or
                    not 0 <= t_ms <= UINT32_MASK or
                    not isinstance(stationary, bool) or not isinstance(key_down, bool)):
                return None
            acc = np.asarray(acceleration, dtype=float)
            gyr = np.asarray(gyroscope, dtype=float)
            quat = np.asarray(quaternion, dtype=float)
            if (acc.shape != (3,) or gyr.shape != (3,) or quat.shape != (4,) or
                    not all(np.all(np.isfinite(v)) for v in (acc, gyr, quat)) or
                    np.linalg.norm(acc) > 200 or np.linalg.norm(gyr) > 70 or
                    np.linalg.norm(quat) < 0.001):
                return None
            return int(t_ms), acc, gyr, quat / np.linalg.norm(quat)
        except (TypeError, ValueError, OverflowError):
            return None

    def _finish(self):
        result = self._trajectory.finish()
        points = result["positions"]
        points = points - points[0] + self._origin
        if not np.all(np.isfinite(points)) or np.max(np.abs(points)) > MAX_POSITION_M:
            self._halt("position_limit")
            return False
        # Send up to 50 Hz corrected geometry, retaining both stroke boundaries.
        indices = list(range(0, len(points), 4))
        if indices[-1] != len(points) - 1:
            indices.append(len(points) - 1)
        self._corrections.append({"stroke_id": self._stroke_id,
                                  "points": points[indices].tolist()})
        self._position = points[-1].copy()
        self._trajectory = None
        return True

    def feed(self, t_ms, acceleration, gyroscope, quaternion, stationary, key_down):
        """Consume a valid RAW sample; return a detached world XYZ in metres."""
        parsed = self._parse(t_ms, acceleration, gyroscope, quaternion, stationary, key_down)
        if parsed is None:
            return self.invalidate("invalid_sample")
        tick, acc, gyr, quat = parsed
        dt = 0.0 if self._last_t is None else ((tick - self._last_t) & UINT32_MASK) / 1000.0
        previous_t = self._last_t
        self._last_t = tick
        if previous_t is not None and not 0.0 < dt <= MAX_SAMPLE_GAP_S:
            return self.invalidate("sample_gap")

        if not key_down:
            if self.recording and self._trajectory is not None:
                try:
                    if not self._finish():
                        return self._position.tolist()
                except (ValueError, FloatingPointError, np.linalg.LinAlgError):
                    return self.invalidate("invalid_sample")
            self._velocity.fill(0)
            self._released = True
            self.recording = False
            self.blocked = False
            self.reason = "ready"
            return self._position.tolist()

        if not self.recording:
            if not self._released:
                if not self.blocked:
                    self._halt("release_required")
                return self._position.tolist()
            self._stroke_id += 1
            self._origin = self._position.copy()
            self._trajectory = IncrementalRtsTrajectory(quat)
            self._released = False
            self.recording = True
            self.blocked = False
            self.reason = "recording"
            return self._position.tolist()

        if self._trajectory.sample_count >= MAX_STROKE_SAMPLES:
            return self.invalidate("stroke_capacity")
        try:
            position, velocity = self._trajectory.append(acc, gyr, dt, stationary)
        except (ValueError, FloatingPointError, np.linalg.LinAlgError):
            return self.invalidate("invalid_sample")
        position = np.asarray(position) + self._origin
        if not np.all(np.isfinite(position)) or np.max(np.abs(position)) > MAX_POSITION_M:
            return self.invalidate("position_limit")
        self._position = position.copy()
        self._velocity = np.asarray(velocity).copy()
        self.reason = "stationary" if stationary else "recording"
        return self._position.tolist()

    def corrections(self, after=0):
        return [{"stroke_id": item["stroke_id"],
                 "points": [point.copy() for point in item["points"]]}
                for item in self._corrections if item["stroke_id"] > after]

    def snapshot(self):
        return {
            "position_m": self._position.tolist(),
            "velocity_m_s": self._velocity.tolist(),
            "recording": self.recording,
            "blocked": self.blocked,
            "reason": self.reason,
            "stroke_id": self._stroke_id,
            "algorithm": "gaitmap-eskf-rts",
        }
