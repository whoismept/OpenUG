import struct
def jdlz_decompress(data):
    usize = struct.unpack_from('<I', data, 8)[0]
    out = bytearray()
    flags1 = 1; flags2 = 1; ip = 16
    while ip < len(data) and len(out) < usize:
        if flags1 == 1: flags1 = data[ip] | 0x100; ip += 1
        if flags2 == 1: flags2 = data[ip] | 0x100; ip += 1
        if (flags1 & 1) == 1:
            if (flags2 & 1) == 1:  # short
                length = (data[ip+1] | ((data[ip] & 0xF0) << 4)) + 3
                dist   = (data[ip] & 0x0F) + 1
            else:                  # long
                dist   = (data[ip+1] | ((data[ip] & 0xE0) << 3)) + 17
                length = (data[ip] & 0x1F) + 3
            ip += 2
            for _ in range(length):
                out.append(out[len(out)-dist])
            flags2 >>= 1
        else:
            out.append(data[ip]); ip += 1
        flags1 >>= 1
    return bytes(out)
