
import os

fwd_path = 'states/0_1549'
rb_path = 'states/1_1549'

if not os.path.exists(fwd_path):
    print(f"Missing {fwd_path}")
    exit()
if not os.path.exists(rb_path):
    print(f"Missing {rb_path}")
    exit()

a = open(fwd_path, 'rb').read()
b = open(rb_path, 'rb').read()

print(f"Filesize A: {len(a)}")
print(f"Filesize B: {len(b)}")

if a == b:
    print("FILES ARE IDENTICAL")
else:
    print("FILES DIFFER")
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            print(f"First diff at offset 0x{i:X} ({i})")
            print(f"A: 0x{a[i]:02X}")
            print(f"B: 0x{b[i]:02X}")
            
            # Analyze neighbors
            start = max(0, i-16)
            end = min(len(a), i+16)
            print("Context A:", a[start:end].hex())
            print("Context B:", b[start:end].hex())
            break
