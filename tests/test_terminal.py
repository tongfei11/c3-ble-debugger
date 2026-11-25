import json
import os
import select
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


@unittest.skipUnless(sys.platform != 'win32', 'PTY integration test requires POSIX')
class TerminalTests(unittest.TestCase):
    def test_terminal_session_with_real_serial_port(self):
        import pty
        master, slave = pty.openpty()
        stop = threading.Event()
        commands = []
        def device():
            buffer = bytearray()
            while not stop.is_set():
                readable, _, _ = select.select([master], [], [], 0.05)
                if not readable:
                    continue
                try:
                    buffer.extend(os.read(master, 4096))
                except OSError:
                    break
                while b'\n' in buffer:
                    raw, _, buffer = buffer.partition(b'\n')
                    try:
                        request = json.loads(raw)
                    except (ValueError, UnicodeError):
                        continue
                    commands.append(request['cmd'])
                    response = {'id': request['id'], 'ok': True,
                                'data': {'name': 'c3-ble-debugger', 'protocol': 1, 'build': '0001'}}
                    os.write(master, json.dumps(response).encode() + b'\n')
        worker = threading.Thread(target=device)
        worker.start()
        try:
            with tempfile.TemporaryDirectory() as folder:
                root = Path(__file__).resolve().parents[1]
                log = Path(folder) / 'session.jsonl'
                result = subprocess.run([sys.executable, str(root / 'desktop/ble_debugger.py'),
                                         '--port', os.ttyname(slave), '--log', str(log)],
                                        input='mark select\nstatus\nquit\n', text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(commands, ['hello', 'status'])
                records = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(records[1]['message']['event'], 'marker')
                self.assertTrue(all(record['label'] == 'select' for record in records[1:]))
                self.assertIn('C3 BLE Debugger 0008', result.stdout)
        finally:
            stop.set()
            worker.join(timeout=1)
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    unittest.main()
