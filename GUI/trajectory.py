"""Incremental port of gaitmap 2.6.0's ZUPT-aided ESKF/RTS trajectory core.

Based on ``_kalman_numba_funcs.py`` at upstream commit
3c6e508bb35fb32cb00495f68c5677e900a9ceaa (MIT, MaD-Lab Erlangen).
See vendor/gaitmap-LICENSE.txt and vendor/gaitmap-PORT.md for provenance.

The nominal navigation, nine-dimensional world-frame error state, Joseph
measurement update, RTS recursion and final correction follow upstream.
This adapter uses NumPy, variable sample intervals, wxyz quaternions, SI units
and a three-dimensional zero-velocity observation. No ground-plane constraint
or position observation is used. Initial velocity is assumed zero. KEY release
is a *soft endpoint assumption*, not an independent velocity measurement.

The result is a short-stroke relative estimate. Inertial prediction is still
part of this algorithm; the correction and backward pass constrain its drift.
There is no absolute heading, absolute position or accelerometer-bias state.
"""

import math

import numpy as np


GRAVITY = 9.80665
_IDENTITY = np.eye(9)
_VELOCITY_OBSERVATION = np.zeros((3, 9))
_VELOCITY_OBSERVATION[:, 3:6] = np.eye(3)


def _vector(value, size, name):
    vector = np.asarray(value, dtype=float)
    if vector.shape != (size,) or not np.all(np.isfinite(vector)):
        raise ValueError(f"{name} must contain {size} finite values")
    return vector.copy()


def _multiply(left, right):
    w, x, y, z = left
    a, b, c, d = right
    return np.array((w*a-x*b-y*c-z*d, w*b+x*a+y*d-z*c,
                     w*c-x*d+y*a+z*b, w*d+x*c-y*b+z*a))


def _rotation_quaternion(vector):
    angle = np.linalg.norm(vector)
    factor = 0.5 if angle < 1e-12 else math.sin(0.5 * angle) / angle
    return np.concatenate(([math.cos(0.5 * angle)], vector * factor))


def _rotate(quaternion, vector):
    scalar, xyz = quaternion[0], quaternion[1:]
    cross = 2.0 * np.cross(xyz, vector)
    return vector + scalar * cross + np.cross(xyz, cross)


def _skew(vector):
    x, y, z = vector
    return np.array(((0.0, -z, y), (z, 0.0, -x), (-y, x, 0.0)))


def _symmetric(matrix):
    return 0.5 * (matrix + matrix.T)


def _right_solve(left, positive_matrix):
    """Compute left @ inverse(matrix) without explicitly forming an inverse."""
    try:
        return np.linalg.solve(positive_matrix.T, left.T).T
    except np.linalg.LinAlgError:
        return left @ np.linalg.pinv(positive_matrix, hermitian=True)


class IncrementalRtsTrajectory:
    """One stroke in a fixed, gravity-aligned world frame with Z pointing up.

    ``append`` accepts raw body accelerations including gravity (m/s^2), body
    angular velocities (rad/s) and elapsed seconds. The initial quaternion is
    body-to-world in wxyz order, normally supplied by the continuously running
    AHRS. ``finish`` returns N+1 positions, velocities and quaternions, including
    the initial state. It is idempotent and returns detached arrays.

    Noise densities describe continuous acceleration/gyro process uncertainty.
    Stationary and endpoint standard deviations are expressed in m/s. All axes
    use equal noise values, including Z. Position starts at zero for each stroke.
    """

    def __init__(self, initial_quaternion_wxyz, *,
                 acceleration_noise_density=0.2, gyro_noise_density=0.01,
                 stationary_velocity_std=0.015, endpoint_velocity_std=0.025):
        quaternion = _vector(initial_quaternion_wxyz, 4, "initial quaternion")
        norm = np.linalg.norm(quaternion)
        if norm < 1e-12:
            raise ValueError("initial quaternion must have nonzero norm")
        quaternion /= norm
        noise = (acceleration_noise_density, gyro_noise_density,
                 stationary_velocity_std, endpoint_velocity_std)
        if not all(math.isfinite(value) and value > 0.0 for value in noise):
            raise ValueError("noise parameters must be finite and positive")
        self._acceleration_variance = acceleration_noise_density ** 2
        self._gyro_variance = gyro_noise_density ** 2
        self._stationary_variance = stationary_velocity_std ** 2
        self._endpoint_variance = endpoint_velocity_std ** 2
        # A strictly positive initial covariance also makes the RTS solves
        # well-defined before any observations have arrived.
        covariance = np.diag([1e-12] * 3 + [0.005 ** 2] * 3 +
                             [math.radians(3.0) ** 2] * 3)
        self._positions = [np.zeros(3)]
        self._velocities = [np.zeros(3)]
        self._quaternions = [quaternion]
        self._prior_errors = [np.zeros(9)]
        self._posterior_errors = [np.zeros(9)]
        self._prior_covariances = [covariance.copy()]
        self._posterior_covariances = [covariance]
        self._transitions = []
        self._finished = None

    @property
    def sample_count(self):
        return len(self._transitions)

    @property
    def position(self):
        if self._finished is not None:
            return self._finished["positions"][-1].copy()
        return self._positions[-1] - self._posterior_errors[-1][:3]

    @property
    def velocity(self):
        if self._finished is not None:
            return self._finished["velocities"][-1].copy()
        return self._velocities[-1] - self._posterior_errors[-1][3:6]

    def _zero_velocity(self, error, covariance, nominal_velocity, variance):
        observation = _VELOCITY_OBSERVATION
        measurement_noise = np.eye(3) * variance
        innovation = nominal_velocity - observation @ error
        innovation_covariance = observation @ covariance @ observation.T + measurement_noise
        gain = _right_solve(covariance @ observation.T, innovation_covariance)
        corrected_error = error + gain @ innovation
        factor = _IDENTITY - gain @ observation
        corrected_covariance = (factor @ covariance @ factor.T +
                                gain @ measurement_noise @ gain.T)
        return corrected_error, _symmetric(corrected_covariance)

    def append(self, accel_body_si, gyro_rad, dt, stationary=False):
        if self._finished is not None:
            raise RuntimeError("a finished trajectory cannot accept samples")
        acceleration = _vector(accel_body_si, 3, "acceleration")
        gyroscope = _vector(gyro_rad, 3, "gyroscope")
        if not math.isfinite(dt) or not 0.0 < dt <= 0.1:
            raise ValueError("sample interval must be in (0, 0.1] seconds")
        if not isinstance(stationary, (bool, np.bool_)):
            raise ValueError("stationary must be a boolean")

        quaternion = _multiply(self._quaternions[-1], _rotation_quaternion(gyroscope * dt))
        quaternion /= np.linalg.norm(quaternion)
        rotated_acceleration = _rotate(quaternion, acceleration)
        linear_acceleration = rotated_acceleration - np.array((0.0, 0.0, GRAVITY))
        velocity = self._velocities[-1] + linear_acceleration * dt
        position = (self._positions[-1] + self._velocities[-1] * dt +
                    linear_acceleration * (0.5 * dt * dt))

        transition = _IDENTITY.copy()
        transition[:3, 3:6] = np.eye(3) * dt
        transition[3:6, 6:] = -_skew(rotated_acceleration) * dt
        # Discretize the same p/v/orientation error model for variable dt.
        # A continuous white acceleration process gives correlated p/v noise.
        process_noise = np.zeros((9, 9))
        process_noise[:3, :3] = np.eye(3) * self._acceleration_variance * dt ** 3 / 3.0
        process_noise[:3, 3:6] = np.eye(3) * self._acceleration_variance * dt ** 2 / 2.0
        process_noise[3:6, :3] = process_noise[:3, 3:6]
        process_noise[3:6, 3:6] = np.eye(3) * self._acceleration_variance * dt
        process_noise[6:, 6:] = np.eye(3) * self._gyro_variance * dt
        prior_error = transition @ self._posterior_errors[-1]
        prior_covariance = _symmetric(
            transition @ self._posterior_covariances[-1] @ transition.T + process_noise)
        if stationary:
            posterior_error, posterior_covariance = self._zero_velocity(
                prior_error, prior_covariance, velocity, self._stationary_variance)
        else:
            posterior_error, posterior_covariance = prior_error.copy(), prior_covariance.copy()

        self._positions.append(position)
        self._velocities.append(velocity)
        self._quaternions.append(quaternion)
        self._transitions.append(transition)
        self._prior_errors.append(prior_error)
        self._posterior_errors.append(posterior_error)
        self._prior_covariances.append(prior_covariance)
        self._posterior_covariances.append(posterior_covariance)
        return self.position, self.velocity

    def finish(self):
        """Apply the soft endpoint zero-velocity observation and RTS smoothing."""
        if self._finished is not None:
            return {name: value.copy() for name, value in self._finished.items()}
        errors = np.asarray(self._posterior_errors).copy()
        covariances = np.asarray(self._posterior_covariances).copy()
        errors[-1], covariances[-1] = self._zero_velocity(
            errors[-1], covariances[-1], self._velocities[-1], self._endpoint_variance)
        for index in range(self.sample_count - 1, -1, -1):
            gain = _right_solve(
                self._posterior_covariances[index] @ self._transitions[index].T,
                self._prior_covariances[index + 1])
            errors[index] = self._posterior_errors[index] + gain @ (
                errors[index + 1] - self._prior_errors[index + 1])
            covariances[index] = _symmetric(self._posterior_covariances[index] + gain @ (
                covariances[index + 1] - self._prior_covariances[index + 1]) @ gain.T)

        positions = np.asarray(self._positions) - errors[:, :3]
        positions -= positions[0]
        velocities = np.asarray(self._velocities) - errors[:, 3:6]
        quaternions = np.asarray([
            _multiply(_rotation_quaternion(error[6:]), quaternion)
            for error, quaternion in zip(errors, self._quaternions)
        ])
        quaternions /= np.linalg.norm(quaternions, axis=1, keepdims=True)
        self._finished = {"positions": positions, "velocities": velocities,
                          "quaternions": quaternions}
        return {name: value.copy() for name, value in self._finished.items()}
