"""Pulse the runtime's existing scripted pad input; always release afterward."""
import argparse
from pathlib import Path
import time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("tokens",nargs="+",help="Runtime pad tokens, e.g. cross or lx=160")
parser.add_argument("--milliseconds",type=int,default=100)
args=parser.parse_args()
if not 20<=args.milliseconds<=3000: parser.error("Pulse must be 20..3000 milliseconds")
path=Path(__file__).resolve().parent.parent/"analysis/uncharted-user/probe-pad.txt"
try:
    path.write_text(" ".join(args.tokens),encoding="ascii")
    time.sleep(args.milliseconds/1000)
finally:
    path.write_text("",encoding="ascii")
print("Released scripted pad input")
