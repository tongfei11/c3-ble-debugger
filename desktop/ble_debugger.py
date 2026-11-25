#!/usr/bin/env python3
"""Interactive host for the C3 BLE debugger JSON-line protocol."""
import argparse
import cmd
import datetime
import errno
import json
import queue
import shlex
import sys
import threading
import time
from pathlib import Path


class LineDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.discarding = False

    def append(self, data):
        messages = []
        for byte in data:
            if byte == 10:
                if not self.discarding:
                    try:
                        message = json.loads(self.buffer)
                        if isinstance(message, dict):
                            messages.append(message)
                    except (ValueError, UnicodeError):
                        pass
                self.buffer.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.buffer) < 131072:
                    self.buffer.append(byte)
                else:
                    self.buffer.clear()
                    self.discarding = True
        return messages


class Session:
    def __init__(self, serial_port, on_event):
        self.serial = serial_port
        self.on_event = on_event
        self.pending = {}
        self.lock = threading.Lock()
        self.next_id = 0
        self.stopped = threading.Event()
        self.thread = None
        self.failure = None

    def start(self):
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def request(self, command, timeout=20, **arguments):
        response_queue = queue.Queue(maxsize=1)
        with self.lock:
            if self.failure:
                raise RuntimeError(self.failure)
            if self.stopped.is_set():
                raise RuntimeError('Serial session is closed')
            self.next_id += 1
            request_id = self.next_id
            payload = json.dumps(dict(arguments, id=request_id, cmd=command), separators=(',', ':')).encode() + b'\n'
            if len(payload) > 768:
                raise ValueError('Command exceeds firmware line limit')
            self.pending[request_id] = response_queue
            try:
                if self.serial.write(payload) != len(payload):
                    raise RuntimeError('Incomplete serial write')
            except Exception:
                self.pending.pop(request_id, None)
                raise
        try:
            response = response_queue.get(timeout=timeout)
            if not response.get('ok'):
                raise RuntimeError(str(response.get('error', 'Device rejected command')))
            return response.get('data', {})
        except queue.Empty:
            raise TimeoutError(f'{command} timed out. Run status before retrying writes or pairing.') from None
        finally:
            with self.lock:
                self.pending.pop(request_id, None)

    def _read(self):
        decoder = LineDecoder()
        try:
            while not self.stopped.is_set():
                for message in decoder.append(self.serial.read(4096)):
                    if 'event' in message:
                        self.on_event(message)
                    else:
                        with self.lock:
                            pending = self.pending.get(message.get('id'))
                            if pending and pending.empty():
                                pending.put_nowait(message)
        except Exception as error:
            if not self.stopped.is_set():
                with self.lock:
                    self.failure = f'Serial disconnected: {error}'
                    for pending in self.pending.values():
                        if pending.empty():
                            pending.put_nowait({'ok': False, 'error': self.failure})
                self.on_event({'event': 'serial_disconnected', 'error': str(error)})

    def close(self):
        self.stopped.set()
        with self.lock:
            for pending in self.pending.values():
                if pending.empty():
                    pending.put_nowait({'ok': False, 'error': 'Serial session closed'})
        if self.thread:
            self.thread.join(timeout=1)
        self.serial.close()


class Capture:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.label = ''
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = path.open('x', encoding='utf-8')
        self.path = path

    def write(self, message):
        with self.lock:
            record = {'time': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      'monotonic': time.monotonic(), 'label': self.label, 'message': message}
            self.file.write(json.dumps(record, ensure_ascii=False) + '\n')
            self.file.flush()

    def mark(self, label):
        with self.lock:
            self.label = label
        self.write({'event': 'marker', 'label': label})

    def close(self):
        self.file.close()


def parse_handle(text):
    value = int(text, 16) if text.lower().startswith('0x') else int(text)
    if not 1 <= value <= 65535:
        raise ValueError('Handle must be 1..65535. Use 0x for hexadecimal.')
    return value


def list_ports():
    from serial.tools.list_ports import comports
    return list(comports())


class DebugShell(cmd.Cmd):
    intro = 'C3 BLE Debugger 0004. Type help. Use mark <label> before pressing a hardware button.'
    prompt = 'ble> '

    def __init__(self, session, capture):
        super().__init__()
        self.session = session
        self.capture = capture
        self.devices = []

    def emptyline(self):
        pass  # Never repeat a write, pairing request or bond removal on empty Enter.

    def onecmd(self, line):
        try:
            return super().onecmd(line)
        except (ValueError, RuntimeError, TimeoutError) as error:
            print(f'Error: {error}')

    def request(self, name, **arguments):
        self.capture.write({'direction': 'tx', 'cmd': name, **arguments})
        try:
            result = self.session.request(name, timeout=35, **arguments)
        except (RuntimeError, TimeoutError) as error:
            self.capture.write({'direction': 'rx', 'cmd': name, 'error': str(error)})
            raise
        self.capture.write({'direction': 'rx', 'cmd': name, 'data': result})
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return result

    def do_scan(self, line):
        """scan [seconds] -- list advertising BLE devices (default 5, maximum 15)."""
        seconds = int(line or '5')
        if not 1 <= seconds <= 15:
            raise ValueError('Scan duration must be 1..15 seconds')
        result = self.request('scan', seconds=seconds)
        self.devices = result.get('devices', [])
        for index, device in enumerate(self.devices, 1):
            print(f"{index:2}  {device.get('address')}  {device.get('address_type')}  {device.get('rssi')} dBm  {json.dumps(device.get('name', ''), ensure_ascii=False)}")

    def do_connect(self, line):
        """connect <scan-number|MAC> [public|random] [secure] -- connect, optionally bond."""
        parts = shlex.split(line)
        if not parts:
            raise ValueError('Provide a scan result number or MAC address')
        target = parts.pop(0)
        address_type = 'public'
        if target.isdigit():
            index = int(target) - 1
            if not 0 <= index < len(self.devices):
                raise ValueError('Scan result number out of range')
            device = self.devices[index]
            target = device['address']
            address_type = device.get('address_type', 'public')
        groups = target.split(':')
        if len(groups) != 6 or any(len(group) != 2 or any(c not in '0123456789abcdefABCDEF' for c in group) for group in groups):
            raise ValueError('Invalid Bluetooth MAC address')
        for part in parts:
            if part in ('public', 'random'):
                address_type = part
            elif part != 'secure':
                raise ValueError(f'Unknown connect option: {part}')
        self.request('connect', address=target.lower(), address_type=address_type, secure='secure' in parts)

    def do_status(self, line):
        """status -- inspect connection and firmware state."""
        self.request('status')

    def do_discover(self, line):
        """discover -- enumerate services, characteristics and descriptors."""
        self.request('discover')

    def do_read(self, line):
        """read <handle> -- read a characteristic's raw bytes."""
        self.request('read', handle=parse_handle(line.strip()))

    def do_write(self, line):
        """write <handle> <hex> [no-response] -- write a characteristic."""
        parts = shlex.split(line)
        if len(parts) not in (2, 3) or (len(parts) == 3 and parts[2] != 'no-response'):
            raise ValueError('Usage: write <handle> <hex> [no-response]')
        payload = bytes.fromhex(parts[1])
        if not 1 <= len(payload) <= 128:
            raise ValueError('Write payload must be 1..128 bytes')
        self.request('write', handle=parse_handle(parts[0]), hex=payload.hex(), response=len(parts) == 2)

    def do_subscribe(self, line):
        """subscribe <handle> [notify|indicate] -- subscribe to one chosen characteristic."""
        parts = shlex.split(line)
        if len(parts) not in (1, 2) or (len(parts) == 2 and parts[1] not in ('notify', 'indicate')):
            raise ValueError('Usage: subscribe <handle> [notify|indicate]')
        self.request('subscribe', handle=parse_handle(parts[0]), mode=parts[1] if len(parts) == 2 else 'notify')

    def do_unsubscribe(self, line):
        """unsubscribe <handle> -- stop notifications for one characteristic."""
        self.request('unsubscribe', handle=parse_handle(line.strip()))

    def do_pair(self, line):
        """pair -- negotiate encryption/bonding on the current connection."""
        self.request('pair')

    def do_disconnect(self, line):
        """disconnect -- disconnect the BLE target."""
        self.request('disconnect')

    def do_bonds(self, line):
        """bonds -- list saved pairing records."""
        self.request('bonds')

    def do_forget(self, line):
        """forget <MAC> -- delete one specified local pairing record."""
        self.request('forget', address=line.strip())

    def do_mark(self, line):
        """mark <label> -- annotate following notifications, e.g. mark volume-up."""
        self.capture.mark(line.strip())
        print(f'Capture label: {line.strip()}')

    def do_raw(self, line):
        """raw <JSON object> -- send an advanced protocol command."""
        payload = json.loads(line)
        if not isinstance(payload, dict) or not isinstance(payload.get('cmd'), str):
            raise ValueError('JSON must contain a cmd string')
        payload.pop('id', None)
        name = payload.pop('cmd')
        self.request(name, **payload)

    def do_ports(self, line):
        """ports -- list local serial ports."""
        for port in list_ports():
            print(f'{port.device}  {port.description}')

    def do_quit(self, line):
        """quit -- close this serial session."""
        return True

    do_exit = do_quit
    do_EOF = do_quit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', action='store_true', help='List serial ports and exit')
    parser.add_argument('--port', help='Serial port, such as /dev/cu.usbmodem... or COM3')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--log', type=Path, help='New JSONL capture file (never overwrites)')
    args = parser.parse_args()
    try:
        import serial
        if args.ports:
            for port in list_ports():
                print(f'{port.device}  {port.description}')
            return 0
        if not args.port:
            parser.error('Choose a serial port with --port. Use --ports to list available ports.')
        path = args.log or Path('logs') / ('capture-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.jsonl')
        capture = Capture(path)
        session = None
        try:
            port = serial.Serial(args.port, args.baud, timeout=0.1, write_timeout=2,
                                 rtscts=False, dsrdtr=False, exclusive=True if sys.platform != 'win32' else None)
            try:
                port.dtr = False
                port.rts = False
            except OSError as error:
                if error.errno not in (errno.ENOTTY, errno.EINVAL):
                    port.close()
                    raise
            def event_received(event):
                capture.write({'direction': 'event', **event})
                print('\n' + json.dumps(event, ensure_ascii=False), flush=True)
            session = Session(port, event_received)
            session.start()
            print(f'Capture: {capture.path}')
            identity = session.request('hello', timeout=8)
            if identity.get('name') != 'c3-ble-debugger' or identity.get('protocol') != 1:
                raise RuntimeError('This port is not running compatible C3 BLE Debugger firmware')
            print(json.dumps(identity, ensure_ascii=False, indent=2))
            capture.write({'direction': 'rx', 'cmd': 'hello', 'data': identity})
            DebugShell(session, capture).cmdloop()
            return 0
        finally:
            if session:
                session.close()
            capture.close()
    except ImportError:
        print('Install desktop/requirements.txt with pip before running this tool.', file=sys.stderr)
        return 1
    except (OSError, RuntimeError, TimeoutError, KeyboardInterrupt) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
