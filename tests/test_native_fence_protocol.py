"""CPU-only new packet rejection and exact old decoder negative control."""
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('fence_broker', Path(__file__).resolve().parents[1] / 'scripts/native_graphics_fence_broker.py')
broker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(broker)
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

valid = bytes(broker.response(1, 5, 1, value=2**40))
check(broker.decode(valid) == (1, 5, 1, 2**40))
check(broker.decode(bytes(broker.response(2, 6, 1))) == (2, 6, 1, 0))
check(broker.decode(bytes(broker.response(3, 7, 1, value=2**64 - 2))) == (3, 7, 1, 2**64 - 2))
invalid = [b'', bytes(63), bytes(65), bytes(64), bytes(broker.response(0, 5, 1)),
           bytes(broker.response(1, 5, 0)), bytes(broker.response(1, 10, 1)),
           bytes(broker.response(1, 6, 1, value=1)), bytes(broker.response(1, 8, 1, value=1)),
           bytes(broker.response(1, 7, 1, value=2**64 - 1))]
for offset in (0, 20, 32, 36, 40, 44, 48, 52, 56, 63):
    data = bytearray(valid)
    data[offset] = 9
    invalid.append(data)
for data in invalid:
    try:
        broker.decode(data)
    except ValueError:
        check(True)
    else:
        raise AssertionError('malformed fence packet admitted')
try:
    broker.base.decode_request(valid)
except ValueError:
    check(True)
else:
    raise AssertionError('old protocol unexpectedly implements fence creation')
print(json.dumps({'CPU_fence_protocol_checks': checks, 'failures': 0,
                  'exact_old_decoder_rejects_new_command': True, 'driver_loaded': False}))
