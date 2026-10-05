"""Synthetic physical trajectories for the ported ESKF/RTS; no IMU required."""

import math
import time
import unittest

import numpy as np

from trajectory import GRAVITY, IncrementalRtsTrajectory


def quaternion(axis, angle):
    axis = np.asarray(axis, dtype=float)
    return np.concatenate(([math.cos(angle / 2)], axis / np.linalg.norm(axis) * math.sin(angle / 2)))


def rotation_matrix(q):
    w, x, y, z = q
    return np.array(((1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)),
                     (2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)),
                     (2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y))))


def translate(initial, direction=(0.8, -0.4, 0.6), bias=(0, 0, 0), samples=400):
    tracker = IncrementalRtsTrajectory(initial)
    matrix = rotation_matrix(initial)
    direction = np.asarray(direction)
    bias = np.asarray(bias)
    position, velocity = np.zeros(3), np.zeros(3)
    expected = [position.copy()]
    for index in range(samples):
        acceleration = direction * (1 if index < samples // 2 else -1)
        position = position + velocity * 0.005 + acceleration * (0.5 * 0.005 ** 2)
        velocity = velocity + acceleration * 0.005
        expected.append(position.copy())
        measured = matrix.T @ (acceleration + np.array((0, 0, GRAVITY))) + bias
        tracker.append(measured, (0, 0, 0), 0.005, stationary=False)
    return tracker, np.asarray(expected)


class TrajectoryTests(unittest.TestCase):
    def test_three_dimensional_open_stroke_keeps_height_and_endpoint(self):
        tracker, expected = translate((1, 0, 0, 0))
        result = tracker.finish()
        np.testing.assert_allclose(result["positions"], expected, atol=1e-10)
        np.testing.assert_allclose(result["velocities"][-1], np.zeros(3), atol=1e-10)
        self.assertGreater(result["positions"][-1, 2], 0.5)
        self.assertGreater(np.linalg.norm(result["positions"][-1]), 0.9)

    def test_arbitrary_initial_tilt_preserves_world_translation(self):
        for q in ((1, 0, 0, 0), quaternion((1, 2, 3), 1.7), quaternion((1, 0, 0), math.pi)):
            with self.subTest(quaternion=q):
                tracker, expected = translate(q)
                np.testing.assert_allclose(tracker.finish()["positions"], expected, atol=1e-10)

    def test_rotating_without_translation_does_not_make_a_virtual_pen_arm(self):
        omega = 0.9
        tracker = IncrementalRtsTrajectory((1, 0, 0, 0))
        for index in range(600):
            q = quaternion((0, 1, 0), omega * (index + 1) * 0.005)
            body_acceleration = rotation_matrix(q).T @ np.array((0, 0, GRAVITY))
            tracker.append(body_acceleration, (0, omega, 0), 0.005)
        np.testing.assert_allclose(tracker.finish()["positions"], 0, atol=1e-10)

    def test_endpoint_constraint_reduces_bias_drift_on_all_axes(self):
        tracker, expected = translate(quaternion((1, 3, -2), 0.8), bias=(0.09, -0.07, 0.06))
        raw_error = np.linalg.norm(tracker.position - expected[-1])
        result = tracker.finish()
        repaired_error = np.linalg.norm(result["positions"][-1] - expected[-1])
        self.assertGreater(raw_error, 0.1)
        self.assertLess(repaired_error, raw_error * 0.25)
        self.assertLess(np.linalg.norm(result["velocities"][-1]), 0.005)
        self.assertLess(np.max(np.linalg.norm(result["positions"] - expected, axis=1)), 0.08)

    def test_rotating_body_bias_still_preserves_short_world_stroke(self):
        tracker = IncrementalRtsTrajectory((1, 0, 0, 0))
        position, velocity = np.zeros(3), np.zeros(3)
        expected = [position.copy()]
        for index in range(400):
            acceleration = np.array((0.8, -0.4, 0.6)) * (1 if index < 200 else -1)
            position += velocity * 0.005 + acceleration * (0.5 * 0.005 ** 2)
            velocity += acceleration * 0.005
            expected.append(position.copy())
            q = quaternion((0, 1, 0), 0.7 * (index + 1) * 0.005)
            measured = rotation_matrix(q).T @ (acceleration + (0, 0, GRAVITY))
            tracker.append(measured + (0.06, -0.04, 0.03), (0, 0.7, 0), 0.005)
        preview_error = np.linalg.norm(tracker.position - position)
        result = tracker.finish()
        corrected_error = np.linalg.norm(result["positions"] - expected, axis=1).max()
        self.assertLess(corrected_error, 0.05)
        self.assertLess(corrected_error, preview_error * 0.3)

    def test_stationary_bias_has_bounded_online_position(self):
        tracker = IncrementalRtsTrajectory(quaternion((1, 0, 0), 0.5))
        acceleration = rotation_matrix(quaternion((1, 0, 0), 0.5)).T @ np.array((0, 0, GRAVITY))
        for index in range(1600):
            jitter = np.array((0.02 * math.sin(index), 0.03 * math.cos(index), 0.01 * math.sin(index * 0.3)))
            tracker.append(acceleration + jitter + (0.08, -0.06, 0.09), (0, 0, 0), 0.005, True)
        self.assertLess(np.linalg.norm(tracker.position), 0.01)
        self.assertLess(np.linalg.norm(tracker.velocity), 0.003)
        self.assertLess(np.max(np.linalg.norm(tracker.finish()["positions"], axis=1)), 0.01)

    def test_variable_intervals_and_covariances_remain_well_defined(self):
        tracker = IncrementalRtsTrajectory((1, 0, 0, 0))
        for index in range(400):
            tracker.append((0.01, -0.015, GRAVITY), (0, 0, 0), (0.004, 0.005, 0.006)[index % 3], True)
            covariance = tracker._posterior_covariances[-1]
            self.assertGreater(np.linalg.eigvalsh(covariance).min(), 0)
            np.testing.assert_allclose(covariance, covariance.T, atol=1e-12)
        result = tracker.finish()
        self.assertTrue(all(np.isfinite(value).all() for value in result.values()))
        self.assertEqual(result["positions"].shape, (401, 3))

    def test_endpoint_is_soft_and_does_not_invent_a_closed_loop(self):
        tracker = IncrementalRtsTrajectory((1, 0, 0, 0))
        for _ in range(200):
            tracker.append((1, 0, GRAVITY), (0, 0, 0), 0.005)
        result = tracker.finish()
        self.assertGreater(result["velocities"][-1, 0], 0)
        self.assertLess(result["velocities"][-1, 0], 0.03)
        self.assertGreater(np.linalg.norm(result["positions"][-1]), 0)
        np.testing.assert_array_equal(result["positions"][0], np.zeros(3))

    def test_finish_idempotent_and_arrays_detached(self):
        tracker, _ = translate((1, 0, 0, 0), samples=20)
        result = tracker.finish()
        expected = result["positions"].copy()
        result["positions"][:] = 999
        np.testing.assert_array_equal(tracker.finish()["positions"], expected)
        self.assertEqual(tracker.sample_count, 20)
        with self.assertRaises(RuntimeError):
            tracker.append((0, 0, GRAVITY), (0, 0, 0), 0.005)
        self.assertEqual(IncrementalRtsTrajectory((1, 0, 0, 0)).finish()["positions"].shape, (1, 3))

    def test_invalid_sample_leaves_history_unchanged(self):
        tracker = IncrementalRtsTrajectory((1, 0, 0, 0))
        for sample in (((0, 0, math.nan), (0, 0, 0), 0.005, False),
                       ((0, 0, GRAVITY), (0, 0), 0.005, False),
                       ((0, 0, GRAVITY), (0, 0, 0), 0, False),
                       ((0, 0, GRAVITY), (0, 0, 0), 0.2, False),
                       ((0, 0, GRAVITY), (0, 0, 0), 0.005, None)):
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                tracker.append(*sample)
        self.assertEqual(tracker.sample_count, 0)


if __name__ == "__main__":
    started = time.perf_counter()
    unittest.main(exit=False)
    print(f"trajectory verification: {time.perf_counter() - started:.3f}s")
