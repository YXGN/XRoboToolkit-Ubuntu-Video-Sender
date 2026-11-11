import contextlib
import io
import threading
import unittest
from unittest.mock import Mock, patch

import mjpg_zmq_sender as sender


class SenderChecks(unittest.TestCase):
    def test_camera_setup_failure_releases_capture(self):
        capture = Mock()
        capture.isOpened.return_value = True
        capture.get.return_value = 0
        args = Mock(width=640, height=480, fps=60)
        spec = sender.CameraSpec("head", "/fake-camera", "inproc://head")
        with patch.object(sender.cv2, "VideoCapture", return_value=capture):
            with self.assertRaises(RuntimeError):
                sender._worker(spec, args, Mock(), threading.Event(), sender.CameraStats())
        capture.release.assert_called_once()

    def test_worker_failure_returns_error_and_closes_context(self):
        context = Mock()
        with patch.object(sender.zmq, "Context", return_value=context), \
                patch.object(sender, "_worker", side_effect=RuntimeError("camera unavailable")), \
                patch.object(sender.threading, "Timer") as timer, \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            result = sender.main([
                "--camera", "head=/fake-camera", "--bind", "head=inproc://head",
                "--duration", "60",
            ])
        self.assertEqual(result, 1)
        context.term.assert_called_once()
        timer.return_value.cancel.assert_called_once()

    def test_success_returns_zero(self):
        with patch.object(sender.zmq, "Context"), \
                patch.object(sender, "_worker"), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sender.main([
                "--camera", "head=/fake-camera", "--bind", "head=inproc://head",
            ]), 0)

    def test_xraw_v2_preserves_shape_role_encoding_and_payload(self):
        payload = b"jpeg-data"
        packet = sender._xraw("head_fpv", "jpeg", 7, 1280, 480, payload)
        fields = sender._HEADER.unpack_from(packet)
        self.assertEqual(fields[:6], (b"XRAW", 2, 1280, 480, 3, 7))
        self.assertGreater(fields[6], 0)
        self.assertGreater(fields[7], 0)
        self.assertEqual(fields[8].rstrip(b"\0"), b"head_fpv")
        self.assertEqual(fields[9].rstrip(b"\0"), b"jpeg")
        self.assertEqual(fields[10], fields[7])
        self.assertEqual(packet[sender._HEADER.size:], payload)


if __name__ == "__main__":
    unittest.main()
