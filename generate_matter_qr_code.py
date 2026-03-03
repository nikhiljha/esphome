#!/usr/bin/env python3
"""Generate Matter QR code setup payload off-device.

This utility generates the same MT:... QR code string that the Matter device
logs on boot, without needing serial access. It uses the same algorithm as
chip::QRCodeSetupPayloadGenerator from the connectedhomeip project.

Usage:
    python3 generate_matter_qr_code.py --discriminator 3840 --passcode 20202021
    python3 generate_matter_qr_code.py -d 3840 -p 20202021 --vendor-id 0xFFF1 --product-id 0x8000

The output MT:... string can be converted to a visual QR code using any QR code
generator (e.g. qrencode, online tools, or the --qr flag if qrcode lib is installed).
"""

import argparse
import sys


# Matter QR code base38 encoding charset
BASE38_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."


def base38_encode(value: int, char_count: int) -> str:
    """Encode an integer into base38 string of exactly char_count characters."""
    result = []
    for _ in range(char_count):
        result.append(BASE38_CHARS[value % 38])
        value //= 38
    return "".join(result)


def encode_bits_to_base38(bits: int, bit_length: int) -> str:
    """Encode a bit string to base38, processing in chunks of 3 characters (for 16 bits)."""
    result = []
    offset = 0
    while offset < bit_length:
        remaining = bit_length - offset
        if remaining >= 16:
            # Extract 16 bits, encode as 3 base38 chars
            chunk = (bits >> offset) & 0xFFFF
            result.append(base38_encode(chunk, 3))
            offset += 16
        elif remaining >= 8:
            # Extract 8 bits, encode as 2 base38 chars
            chunk = (bits >> offset) & 0xFF
            result.append(base38_encode(chunk, 2))
            offset += 8
        else:
            # Extract remaining bits
            chunk = (bits >> offset) & ((1 << remaining) - 1)
            result.append(base38_encode(chunk, 2))
            offset += remaining
    return "".join(result)


def generate_qr_payload(
    discriminator: int,
    passcode: int,
    vendor_id: int = 0xFFF1,
    product_id: int = 0x8000,
    version: int = 0,
    commissioning_flow: int = 0,  # 0 = Standard, 1 = User-intent, 2 = Custom
    discovery_capabilities: int = 2,  # Bit 1 = BLE
) -> str:
    """Generate Matter QR code payload string (MT:... format).

    The payload is a bit-packed structure:
    - Version: 3 bits
    - Vendor ID: 16 bits
    - Product ID: 16 bits
    - Commissioning Flow: 2 bits
    - Discovery Capabilities: 8 bits (rendezvous flags)
    - Discriminator: 12 bits
    - Passcode: 27 bits
    - Padding: 4 bits
    Total: 88 bits
    """
    # Validate inputs
    if not 0 <= discriminator <= 4095:
        raise ValueError(f"Discriminator must be 0-4095, got {discriminator}")
    if not 1 <= passcode <= 99999998:
        raise ValueError(f"Passcode must be 1-99999998, got {passcode}")
    # Matter spec: passcode cannot be these invalid values
    invalid_passcodes = {
        11111111, 22222222, 33333333, 44444444, 55555555,
        66666666, 77777777, 88888888, 12345678, 87654321,
    }
    if passcode in invalid_passcodes:
        raise ValueError(f"Passcode {passcode} is not allowed by Matter spec")

    # Pack bits (LSB first)
    bits = 0
    offset = 0

    # Version: 3 bits
    bits |= (version & 0x7) << offset
    offset += 3

    # Vendor ID: 16 bits
    bits |= (vendor_id & 0xFFFF) << offset
    offset += 16

    # Product ID: 16 bits
    bits |= (product_id & 0xFFFF) << offset
    offset += 16

    # Commissioning Flow: 2 bits
    bits |= (commissioning_flow & 0x3) << offset
    offset += 2

    # Discovery Capabilities (Rendezvous Flags): 8 bits
    bits |= (discovery_capabilities & 0xFF) << offset
    offset += 8

    # Discriminator: 12 bits
    bits |= (discriminator & 0xFFF) << offset
    offset += 12

    # Passcode: 27 bits
    bits |= (passcode & 0x7FFFFFF) << offset
    offset += 27

    # Padding: 4 bits (zeros)
    offset += 4

    assert offset == 88, f"Expected 88 bits, got {offset}"

    # Encode to base38
    encoded = encode_bits_to_base38(bits, 88)

    return f"MT:{encoded}"


def main():
    parser = argparse.ArgumentParser(
        description="Generate Matter QR code setup payload (MT:... string)"
    )
    parser.add_argument(
        "-d", "--discriminator",
        type=int, default=3840,
        help="Discriminator value (0-4095, default: 3840)"
    )
    parser.add_argument(
        "-p", "--passcode",
        type=int, default=20202021,
        help="Passcode (1-99999998, default: 20202021)"
    )
    parser.add_argument(
        "--vendor-id",
        type=lambda x: int(x, 0), default=0xFFF1,
        help="Vendor ID (default: 0xFFF1 = test)"
    )
    parser.add_argument(
        "--product-id",
        type=lambda x: int(x, 0), default=0x8000,
        help="Product ID (default: 0x8000)"
    )
    parser.add_argument(
        "--flow",
        type=int, default=0, choices=[0, 1, 2],
        help="Commissioning flow (0=Standard, 1=User-intent, 2=Custom, default: 0)"
    )
    parser.add_argument(
        "--discovery",
        type=int, default=2,
        help="Discovery capabilities bitmask (1=SoftAP, 2=BLE, 4=OnNetwork, default: 2=BLE)"
    )
    parser.add_argument(
        "--qr",
        action="store_true",
        help="Generate visual QR code (requires 'qrcode' Python package)"
    )
    parser.add_argument(
        "--qr-output",
        type=str, default=None,
        help="Save QR code image to file (PNG). Requires 'qrcode[pil]' package."
    )

    args = parser.parse_args()

    try:
        payload = generate_qr_payload(
            discriminator=args.discriminator,
            passcode=args.passcode,
            vendor_id=args.vendor_id,
            product_id=args.product_id,
            commissioning_flow=args.flow,
            discovery_capabilities=args.discovery,
        )
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"QR Code Payload: {payload}")
    print(f"Discriminator:   {args.discriminator}")
    print(f"Passcode:        {args.passcode}")
    print(f"Vendor ID:       0x{args.vendor_id:04X}")
    print(f"Product ID:      0x{args.product_id:04X}")

    if args.qr or args.qr_output:
        try:
            import qrcode  # noqa: F401
        except ImportError:
            print("\nTo generate visual QR codes, install: pip install qrcode[pil]",
                  file=sys.stderr)
            sys.exit(1)

        qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M)
        qr.add_data(payload)
        qr.make(fit=True)

        if args.qr_output:
            img = qr.make_image(fill_color="black", back_color="white")
            img.save(args.qr_output)
            print(f"\nQR code saved to: {args.qr_output}")
        else:
            print()
            qr.print_ascii(invert=True)


if __name__ == "__main__":
    main()
