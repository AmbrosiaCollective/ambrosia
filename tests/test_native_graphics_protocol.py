"""CPU-only protocol rejection and ownership/lifetime checks; no driver."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('broker', Path(__file__).resolve().parents[1] / 'scripts/native_graphics_broker.py')
broker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(broker)
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

def packet(sequence, operation, owner=0):
    return broker.response(sequence, operation, owner)

check(broker.PACKET.size == 64)
check(broker.decode_request(packet(1, 1)) == (1, 1, 0))
check(broker.decode_request(packet(2, 3, 7)) == (2, 3, 7))
invalid = [b'', bytes(63), bytes(65), bytes(64), packet(0, 1), packet(1, 0),
           packet(1, 5), packet(1, 1, 7), packet(1, 2), packet(1, 3), packet(1, 4, 7)]
for index in (0, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 63):
    data = bytearray(packet(1, 1))
    data[index] = 9
    invalid.append(bytes(data))
for data in invalid:
    try:
        broker.decode_request(data)
    except ValueError:
        check(True)
    else:
        raise AssertionError('malformed request admitted')

class Device:
    api_version = [1, 3, 7]
    def __init__(self):
        self.closes = 0
        created.append(self)
    def close(self):
        self.closes += 1

created = []
commands = [packet(1, 1), packet(2, 1), packet(3, 3, 999), packet(4, 3, 1),
            packet(5, 2, 1), packet(6, 3, 1), packet(7, 1), packet(8, 2, 1),
            packet(9, 3, 2), packet(10, 4)]
replies = []
class Files:
    def fstat(self, descriptor):
        return SimpleNamespace(st_size=64)
    def pread(self, descriptor, size, offset):
        return commands[len(replies)]
    def pwrite(self, descriptor, data, offset):
        replies.append(broker.PACKET.unpack(data))
        return len(data)

original_os = broker.os
broker.os = Files()
try:
    broker.serve(1, 2, Device)
finally:
    broker.os = original_os
check(len(replies) == 10)
check([reply[9] for reply in replies] == [0, 1, 1, 0, 0, 1, 0, 1, 0, 0])
check(replies[0][3] == 1 and replies[6][3] == 2)
check(replies[0][10] == (1 << 22) | (3 << 12) | 7)
check(replies[3][10] == replies[8][10] == replies[0][10])
check(len(created) == 2 and all(device.closes == 1 for device in created))
print(json.dumps({'CPU_protocol_checks': checks, 'failures': 0, 'driver_loaded': False}))
