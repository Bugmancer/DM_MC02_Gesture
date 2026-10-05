"""Short-stroke inertial translation in the Fusion gravity-aligned world frame.

Input is already gravity-compensated world acceleration in m/s^2. No board
orientation or virtual pen-tip offset is used here. Position is held between
KEY strokes; it is a relative drawing estimate, not absolute positioning.
Zero-velocity updates, a noise deadband and weak velocity leakage limit drift
but also attenuate sustained motion. Six-axis IMUs cannot distinguish rest
from constant velocity or remove all orientation/gravity estimation error.
"""

import math


UINT32_MASK = 0xFFFFFFFF
MAX_SAMPLE_GAP_S = 0.1
NOISE_FLOOR_M_S2 = 0.08
FULL_RESPONSE_M_S2 = 0.16
VELOCITY_DECAY_S = 2.0
MAX_SPEED_M_S = 3.0
MAX_STROKE_DISPLACEMENT_M = 5.0
MAX_POSITION_M = 50.0
MAX_ACCELERATION_M_S2 = 200.0


class PenMotion:
    """Consume accepted 200 Hz world-motion samples with physical KEY state."""

    def __init__(self):
        self.reset()

    def reset(self):
        """Start a new world origin; require KEY release before a first stroke."""
        self._position = [0.0, 0.0, 0.0]
        self._velocity = [0.0, 0.0, 0.0]
        self._previous_acceleration = [0.0, 0.0, 0.0]
        self._idle_bias = [0.0, 0.0, 0.0]
        self._stroke_origin = [0.0, 0.0, 0.0]
        self._last_t = None
        self._released = False
        self.recording = False
        self.blocked = False
        self.reason = "release_required"

    def _halt(self, reason):
        self._velocity = [0.0, 0.0, 0.0]
        self._previous_acceleration = [0.0, 0.0, 0.0]
        self._released = False
        self.recording = False
        self.blocked = True
        self.reason = reason

    def invalidate(self, reason="sample_gap"):
        """Freeze after rejected upstream IMU data; KEY release must rearm."""
        self._halt(reason)
        return self._position.copy()

    @staticmethod
    def _parse(t_ms, world_linear, stationary, key_down):
        try:
            if (isinstance(t_ms, bool) or int(t_ms) != t_ms or
                    not 0 <= t_ms <= UINT32_MASK or
                    not isinstance(stationary, bool) or
                    not isinstance(key_down, bool) or len(world_linear) != 3):
                return None
            acceleration = [float(value) for value in world_linear]
            if (not all(math.isfinite(value) for value in acceleration) or
                    math.hypot(*acceleration) > MAX_ACCELERATION_M_S2):
                return None
            return int(t_ms), acceleration
        except (TypeError, ValueError, OverflowError):
            return None

    @staticmethod
    def _remove_noise(acceleration):
        magnitude = math.hypot(*acceleration)
        if magnitude <= NOISE_FLOOR_M_S2:
            return [0.0, 0.0, 0.0]
        if magnitude >= FULL_RESPONSE_M_S2:
            return acceleration
        # A radial smoothstep avoids direction-dependent thresholds and keeps
        # stronger physical accelerations unchanged.
        amount = ((magnitude - NOISE_FLOOR_M_S2) /
                  (FULL_RESPONSE_M_S2 - NOISE_FLOOR_M_S2))
        gain = amount * amount * (3.0 - 2.0 * amount)
        return [value * gain for value in acceleration]

    def feed(self, t_ms, world_linear, stationary, key_down):
        """Return a detached XYZ position in metres, frozen whenever KEY is up.

        A missing KEY state, bad sample, timing gap, or motion limit terminates
        integration until a subsequent valid release. Timestamp wrap is valid.
        """
        parsed = self._parse(t_ms, world_linear, stationary, key_down)
        if parsed is None:
            self._halt("invalid_sample")
            return self._position.copy()
        tick, acceleration = parsed
        dt = 0.0 if self._last_t is None else ((tick - self._last_t) & UINT32_MASK) / 1000.0
        previous_t = self._last_t
        self._last_t = tick
        if previous_t is not None and not 0.0 < dt <= MAX_SAMPLE_GAP_S:
            self._halt("sample_gap")
            return self._position.copy()

        if not key_down:
            self._velocity = [0.0, 0.0, 0.0]
            self._previous_acceleration = [0.0, 0.0, 0.0]
            self._released = True
            self.recording = False
            self.blocked = False
            self.reason = "ready"
            if stationary and math.hypot(*acceleration) <= 0.3:
                alpha = dt / (2.0 + dt)
                self._idle_bias = [bias + alpha * (value - bias)
                                   for value, bias in zip(acceleration, self._idle_bias)]
            return self._position.copy()

        if not self.recording:
            if not self._released:
                if not self.blocked:
                    self._halt("release_required")
                return self._position.copy()
            self._stroke_origin = self._position.copy()
            self._released = False
            self.recording = True
            self.blocked = False
            self.reason = "recording"
            self._previous_acceleration = ([0.0, 0.0, 0.0] if stationary else
                self._remove_noise([value - bias for value, bias in
                                    zip(acceleration, self._idle_bias)]))
            return self._position.copy()

        if stationary:
            self._velocity = [0.0, 0.0, 0.0]
            self._previous_acceleration = [0.0, 0.0, 0.0]
            self.reason = "stationary"
            return self._position.copy()

        filtered = self._remove_noise(
            [value - bias for value, bias in zip(acceleration, self._idle_bias)])
        decay = math.exp(-dt / VELOCITY_DECAY_S)
        next_velocity = [velocity * decay + (old + new) * (0.5 * dt)
                         for velocity, old, new in
                         zip(self._velocity, self._previous_acceleration, filtered)]
        next_position = [position + (old + new) * (0.5 * dt)
                         for position, old, new in zip(self._position, self._velocity, next_velocity)]
        if math.hypot(*next_velocity) > MAX_SPEED_M_S:
            self._halt("speed_limit")
        elif math.hypot(*(value - origin for value, origin in
                          zip(next_position, self._stroke_origin))) > MAX_STROKE_DISPLACEMENT_M:
            self._halt("stroke_limit")
        elif math.hypot(*next_position) > MAX_POSITION_M:
            self._halt("position_limit")
        else:
            self._position = next_position
            self._velocity = next_velocity
            self._previous_acceleration = filtered
            self.reason = "recording"
        return self._position.copy()

    def snapshot(self):
        return {
            "position_m": self._position.copy(),
            "velocity_m_s": self._velocity.copy(),
            "recording": self.recording,
            "blocked": self.blocked,
            "reason": self.reason,
        }
