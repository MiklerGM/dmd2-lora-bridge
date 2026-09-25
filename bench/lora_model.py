#!/usr/bin/env python3
"""
A model of the LoRa PHY encoding, after gr-lora_sdr (Tapparel et al.), for asking which symbol
error turns our frame into the frame the T-Deck decodes when it fails.

Encoding: explicit header nibbles, whitened payload nibbles (low nibble first), CRC nibbles; Hamming
coding (the first SF-2 nibbles at CR 4/8); diagonal interleaving in blocks, SF-2 bits per symbol
in the first block and, with LDRO, in every block, where a parity bit puts the symbol at 4n+1;
inverse Gray mapping; plus one. Decoding reverses it; `decode` can read the symbols as an implicit
header receiver does, which is how the T-Deck captured them.

  bench/lora_model.py --self-check
"""

import sys

SF, CR = 11, 1          # CR 4/5
N = 1 << SF


def whitening(n):
    out, w = [], 0xFF
    for _ in range(n):
        out.append(w)
        fb = ((w >> 7) ^ (w >> 5) ^ (w >> 4) ^ (w >> 3)) & 1
        w = ((w << 1) & 0xFF) | fb
    return out


def header_nibbles(length, cr, has_crc):
    a0, a1, a2 = length >> 4, length & 0xF, (cr << 1) | has_crc
    bit = lambda x, i: (x >> i) & 1
    c4 = bit(a0, 3) ^ bit(a0, 2) ^ bit(a0, 1) ^ bit(a0, 0)
    c3 = bit(a0, 3) ^ bit(a1, 3) ^ bit(a1, 2) ^ bit(a1, 1) ^ bit(a2, 0)
    c2 = bit(a0, 2) ^ bit(a1, 3) ^ bit(a1, 0) ^ bit(a2, 3) ^ bit(a2, 1)
    c1 = bit(a0, 1) ^ bit(a1, 2) ^ bit(a1, 0) ^ bit(a2, 2) ^ bit(a2, 1) ^ bit(a2, 0)
    c0 = bit(a0, 0) ^ bit(a1, 1) ^ bit(a2, 3) ^ bit(a2, 2) ^ bit(a2, 1) ^ bit(a2, 0)
    return [a0, a1, a2, c4, c3 << 3 | c2 << 2 | c1 << 1 | c0]


def crc16(payload):
    """LoRa payload CRC as gr-lora_sdr computes it: CCITT over all but the last two bytes, then
    those two XORed in."""
    crc = 0
    for b in payload[:-2]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc ^ payload[-1] ^ (payload[-2] << 8)


def hamming_enc(nibble, cr):
    d = [(nibble >> i) & 1 for i in range(4)]            # d0 = LSB, sent first
    if cr == 1:
        return [d[0], d[1], d[2], d[3], d[0] ^ d[1] ^ d[2] ^ d[3]]
    p = [d[0] ^ d[1] ^ d[2], d[1] ^ d[2] ^ d[3], d[0] ^ d[1] ^ d[3], d[0] ^ d[2] ^ d[3]]
    return [d[0], d[1], d[2], d[3]] + p[:cr]


def hamming_dec(cw):
    d = cw[:4]
    if len(cw) == 8 and sum(cw) % 2:                    # odd: one error, correct it
        s0 = cw[0] ^ cw[1] ^ cw[2] ^ cw[4]
        s1 = cw[1] ^ cw[2] ^ cw[3] ^ cw[5]
        s2 = cw[0] ^ cw[1] ^ cw[3] ^ cw[6]
        flip = {5: 0, 7: 1, 3: 2, 6: 3}.get(s0 | s1 << 1 | s2 << 2)
        if flip is not None:
            d = list(d)
            d[flip] ^= 1
    return d[0] | d[1] << 1 | d[2] << 2 | d[3] << 3


def gray(x):
    return x ^ (x >> 1)


def gray_inv(x):
    out = x
    for j in range(1, SF):
        out ^= x >> j
    return out


def nibbles_to_symbols(nibbles, ldro=True):
    """Hamming, interleaving, Gray and the +1, block by block."""
    symbols, i, first = [], 0, True
    while i < len(nibbles):
        sf_app = SF - 2 if (first or ldro) else SF
        cr = 4 if first else CR
        block = nibbles[i:i + sf_app] + [0] * max(0, i + sf_app - len(nibbles))
        cws = [hamming_enc(n, cr) for n in block]
        cw_len = 4 + cr
        for col in range(cw_len):
            bits = [cws[(col - j - 1) % sf_app][col] for j in range(sf_app)]
            if sf_app != SF:
                bits += [sum(bits) % 2, 0]
            v = 0
            for b in bits:
                v = v << 1 | b
            symbols.append((gray_inv(v) + 1) % N)
        i += sf_app
        first = False
    return symbols


def symbols_to_nibbles(symbols, ldro=True):
    nibbles, i, first = [], 0, True
    while i < len(symbols):
        sf_app = SF - 2 if (first or ldro) else SF
        cr = 4 if first else CR
        cw_len = 4 + cr
        block = symbols[i:i + cw_len]
        if len(block) < cw_len:
            break
        rows = []
        for k in block:
            s = (k - 1) % N
            if sf_app != SF:
                s //= 4
            g = gray(s)
            rows.append([(g >> (sf_app - 1 - j)) & 1 for j in range(sf_app)])
        cws = [[0] * cw_len for _ in range(sf_app)]
        for col in range(cw_len):
            for j in range(sf_app):
                cws[(col - j - 1) % sf_app][col] = rows[col][j]
        nibbles += [hamming_dec(cw) for cw in cws]
        i += cw_len
        first = False
    return nibbles


def transmit(frame, ldro=True):
    """Symbols of an explicit-header, CRC-on packet carrying `frame`."""
    w = whitening(len(frame))
    nib = header_nibbles(len(frame), CR, 1)
    for b, x in zip(frame, w):
        nib += [(b ^ x) & 0xF, (b ^ x) >> 4]
    crc = crc16(list(frame))
    nib += [crc & 0xF, (crc >> 4) & 0xF, (crc >> 8) & 0xF, (crc >> 12) & 0xF]
    return nibbles_to_symbols(nib, ldro)


def decode_implicit(symbols, length, ldro=True):
    """What an implicit-header receiver with CRC off hands over for these symbols."""
    nib = symbols_to_nibbles(symbols, ldro)
    w = whitening(length)
    return bytes(((nib[2 * j] | nib[2 * j + 1] << 4) ^ w[j]) & 0xFF for j in range(length))


def ebyte_frame(seq):
    """The bench's frame for a sequence number: EBYTE header and the XORed payload."""
    payload = f"{seq:08d}".encode() + b"A" * 30
    c = 0
    for b in payload:
        c ^= b
    return bytes([0x2C, 0x12, 0xA0 ^ c, 0x01 ^ c, 0, 0, 0, len(payload)]) + bytes(b ^ 0x12 for b in payload)


def self_check():
    assert whitening(8) == [0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85]
    assert header_nibbles(46, 1, 1) == [2, 14, 3, 1, 1]
    for n in range(16):
        cw = hamming_enc(n, 4)
        assert hamming_dec(cw) == n
        for k in range(4):
            bad = list(cw)
            bad[k] ^= 1
            assert hamming_dec(bad) == n
    frame = ebyte_frame(5)
    assert frame[:8] == bytes([0x2C, 0x12, 0xA5, 0x04, 0, 0, 0, 0x26])
    sym = transmit(frame)
    assert len(sym) == 63
    assert all((s - 1) % 4 == 0 for s in sym)             # reduced rate: 4n+1 throughout
    out = decode_implicit(sym, 51)
    assert out[:3] == bytes([0x1D, 0xED, 0xCD])
    print("lora_model: self-check passes")


if __name__ == "__main__":
    sys.exit(self_check())
