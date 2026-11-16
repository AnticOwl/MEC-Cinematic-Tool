# Minimal protobuf wire format: parse a message into [(field, wire type, value)] (value: int for varint / fixed,
# bytes for length-delimited) and build it back. Unknown fields are kept as they are, in order.
import struct


def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def _put_varint(n):
    out = bytearray()
    while True:
        c = n & 0x7F
        n >>= 7
        if n:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


def parse(b):
    i, out = 0, []
    while i < len(b):
        key, i = _varint(b, i)
        f, w = key >> 3, key & 7
        if w == 0:
            v, i = _varint(b, i)
        elif w == 1:
            v = struct.unpack_from('<Q', b, i)[0]
            i += 8
        elif w == 2:
            n, i = _varint(b, i)
            v = bytes(b[i:i + n])
            i += n
        elif w == 5:
            v = struct.unpack_from('<I', b, i)[0]
            i += 4
        else:
            raise ValueError(f'wire type {w} at {i}')
        out.append((f, w, v))
    return out


def build(fields):
    out = bytearray()
    for f, w, v in fields:
        out += _put_varint((f << 3) | w)
        if w == 0:
            out += _put_varint(v)
        elif w == 1:
            out += struct.pack('<Q', v)
        elif w == 2:
            out += _put_varint(len(v)) + v
        elif w == 5:
            out += struct.pack('<I', v)
    return bytes(out)
