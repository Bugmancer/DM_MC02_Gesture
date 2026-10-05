"""Fusion attitude in a gravity-aligned, right-handed world frame (Z up).

Inputs use SI units and body-to-world quaternions use w,x,y,z order.  A
six-axis IMU supplies relative heading, not geographic north.  zero() keeps
that world orientation and resets only the translation origin and elapsed
time.  Translation belongs to the stroke estimator and is supplied through
set_translation(); this module never integrates acceleration into position.
"""

from collections import deque
import time

import imufusion
import numpy as np


GRAVITY = 9.80665
SAMPLE_RATE = 200
MAX_SAMPLE_GAP_S = 0.1
MAX_WALL_GAP_S = 0.5
UINT32_MASK = 0xFFFFFFFF


def _normalise_quaternion(value):
    value = np.asarray(value, dtype=float)
    return value / np.linalg.norm(value)


def _gravity_quaternion(acceleration):
    # Shortest rotation from measured body gravity to world +Z avoids the
    # large startup acceleration artefact of initializing a tilted IMU flat.
    direction = acceleration / np.linalg.norm(acceleration)
    if direction[2] < -0.999999:
        return np.array((0.0, 1.0, 0.0, 0.0))
    return _normalise_quaternion((1.0 + direction[2], direction[1],
                                  -direction[0], 0.0))


class PoseEstimator:
    """Consume 200 Hz RAW records; callers provide serialization if shared."""

    def __init__(self, clock=None):
        self._clock = clock or time.monotonic
        self.reset()

    def reset(self):
        """Discard all sensor/session history on reconnect or device reboot."""
        self._ahrs = imufusion.Ahrs()
        self._fusion_settings = imufusion.Settings(
            imufusion.CONVENTION_NWU, 0.35, 2000, 8, 0, 5 * SAMPLE_RATE)
        self._ahrs.settings = self._fusion_settings
        self._offset = imufusion.Offset(SAMPLE_RATE)
        self._quaternion = np.array((1.0, 0.0, 0.0, 0.0))
        self._position = np.zeros(3)
        self._velocity = np.zeros(3)
        self._world_linear = np.zeros(3)
        self._body_acceleration = np.zeros(3)
        self._corrected_gyro = np.zeros(3)
        self._trail = deque(maxlen=160)
        self._last_t = None
        self._last_seq = None
        self._last_wall = None
        self._elapsed = 0.0
        self._last_trail_elapsed = None
        self._still_elapsed = 0.0
        self._stationary = False
        self._available = False
        self._status = "waiting"

    def zero(self):
        """Reset translation and elapsed time without rotating the world frame."""
        self._position.fill(0)
        self._velocity.fill(0)
        self._trail.clear()
        self._last_trail_elapsed = None
        self._elapsed = 0.0
        if self._available and self._status != "gap":
            self._status = "stationary" if self._stationary else "tracking"

    def set_translation(self, position, velocity, trail=None):
        """Accept finite world-frame vectors from the stroke estimator.

        Explicit trails replace the preview with their last 160 points.  If
        omitted, positions are appended at most 10 Hz of accepted sensor time.
        Invalid updates return False and leave the previous estimate intact.
        """
        try:
            position = np.asarray(position, dtype=float)
            velocity = np.asarray(velocity, dtype=float)
            if (position.shape != (3,) or velocity.shape != (3,) or
                    not np.all(np.isfinite(position)) or not np.all(np.isfinite(velocity))):
                return False
            if trail is not None:
                trail = np.asarray(trail, dtype=float)
                if trail.shape == (0,):
                    trail = np.empty((0, 3))
                if trail.ndim != 2 or trail.shape[1] != 3 or not np.all(np.isfinite(trail)):
                    return False
        except (TypeError, ValueError, OverflowError):
            return False
        self._position = position.copy()
        self._velocity = velocity.copy()
        if trail is not None:
            self._trail = deque(trail[-160:].tolist(), maxlen=160)
            self._last_trail_elapsed = self._elapsed
        elif self._available and self._status != "gap" and (
                self._last_trail_elapsed is None or
                self._elapsed - self._last_trail_elapsed + 1e-9 >= 0.1):
            self._trail.append(position.tolist())
            self._last_trail_elapsed = self._elapsed
        return True

    def _gap(self):
        self._velocity.fill(0)
        self._world_linear.fill(0)
        self._body_acceleration.fill(0)
        self._corrected_gyro.fill(0)
        self._still_elapsed = 0.0
        self._stationary = False
        self._status = "gap" if self._available else "waiting"

    @staticmethod
    def _record(record):
        try:
            tick = record["t"]
            seq = record.get("seq")
            if isinstance(tick, bool) or int(tick) != tick or not 0 <= tick <= UINT32_MASK:
                return None
            if seq is not None and (isinstance(seq, bool) or int(seq) != seq or
                                    not 0 <= seq <= UINT32_MASK):
                return None
            acceleration = np.array([record[key] for key in ("ax", "ay", "az")], dtype=float)
            gyroscope = np.array([record[key] for key in ("gx", "gy", "gz")], dtype=float)
            if (acceleration.shape != (3,) or gyroscope.shape != (3,) or
                    not np.all(np.isfinite(acceleration)) or not np.all(np.isfinite(gyroscope)) or
                    np.any(np.abs(acceleration) > 200) or np.any(np.abs(gyroscope) > 40)):
                return None
            return int(tick), None if seq is None else int(seq), acceleration, gyroscope
        except (KeyError, TypeError, ValueError, OverflowError):
            return None

    def feed(self, record):
        """Return True for an updated orientation; input uses ms, m/s^2, rad/s."""
        parsed = self._record(record)
        if parsed is None:
            self._gap()
            return
        tick, seq, acceleration, gyroscope = parsed
        now = self._clock()
        if self._last_t is not None:
            delta_ms = (tick - self._last_t) & UINT32_MASK
            delta_seq = None if seq is None or self._last_seq is None else (seq - self._last_seq) & UINT32_MASK
            # Unsigned arithmetic accepts the normal counter wrap but rejects
            # duplicates and older packets without moving the time baseline.
            if delta_ms == 0 or delta_ms >= 0x80000000 or (
                    delta_seq is not None and (delta_seq == 0 or delta_seq >= 0x80000000)):
                return
            dt = delta_ms / 1000.0
            self._last_t, self._last_seq = tick, seq
            wall_gap = now - self._last_wall > MAX_WALL_GAP_S
            self._last_wall = now
            if dt > MAX_SAMPLE_GAP_S or wall_gap:
                self._gap()
                return
        else:
            # Gravity can only initialize tilt when there is a useful vector.
            if not 0.5 * GRAVITY <= np.linalg.norm(acceleration) <= 1.5 * GRAVITY:
                return
            self._quaternion = _gravity_quaternion(acceleration)
            self._ahrs.quaternion = imufusion.Quaternion(self._quaternion)
            # Tilt is already seeded from gravity. Finish Fusion's startup at
            # zero elapsed time; its default startup otherwise forces yaw to
            # zero for three seconds in the no-magnetometer update method.
            self._ahrs.settings = imufusion.Settings(
                imufusion.CONVENTION_NWU, 0, 2000, 8, 0, 5 * SAMPLE_RATE)
            self._ahrs.update_no_magnetometer(np.zeros(3), acceleration / GRAVITY, 0.0)
            self._ahrs.settings = self._fusion_settings
            self._last_t, self._last_seq, self._last_wall = tick, seq, now
            self._available = True
            self.zero()
            dt = 0.0

        corrected_gyro = self._offset.update(np.rad2deg(gyroscope))
        self._ahrs.update_no_magnetometer(corrected_gyro, acceleration / GRAVITY, dt)
        self._quaternion = _normalise_quaternion(self._ahrs.quaternion.wxyz)
        earth_linear = np.asarray(self._ahrs.earth_acceleration, dtype=float) * GRAVITY
        if not np.all(np.isfinite(earth_linear)) or not np.all(np.isfinite(self._quaternion)):
            self.reset()
            return

        self._elapsed += dt
        self._world_linear = earth_linear.copy()
        self._body_acceleration = acceleration.copy()
        self._corrected_gyro = np.deg2rad(corrected_gyro)

        quiet = (np.linalg.norm(self._corrected_gyro) < 0.055 and
                 abs(np.linalg.norm(acceleration) - GRAVITY) < 0.16 and
                 np.linalg.norm(earth_linear) < 0.22)
        self._still_elapsed = min(0.2, self._still_elapsed + dt) if quiet else 0.0
        self._stationary = self._still_elapsed >= 0.2
        self._status = "stationary" if self._stationary else "tracking"
        return True

    def _expire(self):
        if self._available and self._clock() - self._last_wall > MAX_WALL_GAP_S:
            self._gap()

    def world_motion(self):
        """Sensor SI inputs and Fusion attitude in the viewer's world frame."""
        self._expire()
        return {
            "quaternion": [float(value) for value in self._quaternion],
            "acceleration_m_s2": [float(value) for value in self._world_linear],
            "body_acceleration_m_s2": [float(value) for value in self._body_acceleration],
            "gyroscope_rad_s": [float(value) for value in self._corrected_gyro],
            "stationary": self._stationary,
            "timestamp_ms": self._last_t,
            "available": self._available and self._status != "gap",
            "status": self._status,
        }

    def snapshot(self):
        self._expire()
        angles = imufusion.Quaternion(self._quaternion).to_euler()
        return {
            "available": self._available,
            "quaternion": [float(value) for value in self._quaternion],
            "euler_deg": dict(zip(("roll", "pitch", "yaw"), (float(value) for value in angles))),
            "position_m": self._position.tolist(),
            "velocity_m_s": self._velocity.tolist(),
            "linear_accel_m_s2": self._world_linear.tolist(),
            "stationary": self._stationary,
            "position_limited": False,
            "elapsed_s": float(self._elapsed),
            "status": self._status,
            "trail": [list(point) for point in self._trail],
            "timestamp_ms": self._last_t,
        }
