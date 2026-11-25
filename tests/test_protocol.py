import json
import queue
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'desktop'))
from ble_debugger import LineDecoder, Session


class FakeSerial:
    def __init__(self):
        self.incoming = queue.Queue()
        self.written = []
        self.closed = False

    def read(self, count):
        try:
            return self.incoming.get(timeout=0.02)
        except queue.Empty:
            return b''

    def write(self, data):
        self.written.append(json.loads(data))
        return len(data)

    def close(self):
        self.closed = True


class ProtocolTests(unittest.TestCase):
    def test_fragmented_and_corrupt_lines(self):
        decoder = LineDecoder()
        self.assertEqual(decoder.append(b'{"id":1,'), [])
        self.assertEqual(decoder.append(b'"ok":true}\r\nnoise\n'), [{'id': 1, 'ok': True}])
        self.assertEqual(decoder.append(b'A' * 200000), [])
        self.assertEqual(decoder.append(b'ignored\n{"event":"ready"}\n'), [{'event': 'ready'}])

    def test_response_correlated_amid_notifications(self):
        serial = FakeSerial()
        events = []
        session = Session(serial, events.append)
        session.start()
        def device():
            while not serial.written:
                time.sleep(0.005)
            request = serial.written[0]
            serial.incoming.put(json.dumps({'event': 'notification', 'handle': 43, 'hex': '4200'}).encode() + b'\n')
            serial.incoming.put(json.dumps({'id': request['id'], 'ok': True, 'data': {'connected': True}}).encode() + b'\n')
        worker = threading.Thread(target=device)
        worker.start()
        self.assertEqual(session.request('status', timeout=1), {'connected': True})
        worker.join()
        self.assertEqual(events[0]['event'], 'notification')
        session.close()
        self.assertTrue(serial.closed)

    def test_device_error_and_timeout(self):
        serial = FakeSerial()
        session = Session(serial, lambda event: None)
        session.start()
        with self.assertRaises(TimeoutError):
            session.request('status', timeout=0.03)
        serial.incoming.put(b'{"id":1,"ok":true,"data":{}}\n')
        def reject():
            while len(serial.written) < 2:
                time.sleep(0.005)
            serial.incoming.put(b'{"id":2,"ok":false,"error":"not_connected"}\n')
        worker = threading.Thread(target=reject)
        worker.start()
        with self.assertRaisesRegex(RuntimeError, 'not_connected'):
            session.request('discover', timeout=1)
        worker.join()
        session.close()


if __name__ == '__main__':
    unittest.main()
