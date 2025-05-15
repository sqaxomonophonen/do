#!/usr/bin/env python
import struct,os,sys
args = sys.argv[1:]
def usage(err=None):
	if err:
		out = sys.stderr
		out.write("Error: %s\n" % err)
		status = 1
	else:
		out = sys.stdout
		status = 0
	out.write("Usage: %s <command> <input file> <output file>\n" % sys.argv[0])
	out.write("Commands:\n")
	out.write("  txt_to_c5t   Converts text to C5T (sets RGB=0x80 and XY=0x00)\n")
	out.write("  c5t_to_txt   Converts C5T to text (lossy: discards components)\n")
	out.write("  c5t_to_html  Converts C5T to HTML (discards X/Y, squashes RGB into 0x40-0xff range\n")
	sys.exit(status)

if len(args)==0: usage()
cmd = args[0]
args = args[1:]
def getio():
	global args
	if len(args)!=2: usage("expected filename")
	input = open(args[0]).read()
	input = open(args[0]).read()
	args = args[1:]
	if len(args)==0:
		return (input, sys.stdout, False)
	else:
		return (input, open(args[0],"wb"), True)

def pack_c5(c,r=0x80,g=0x80,b=0x80,x=0,y=0):
	return struct.pack("BBBBBBBB", c&255, (c>>8)&255, (c>>16)&255, y, r, g, b, x)

HEADER = b"C5T1Do01"

def readc5t(path):
	input  = open(args[0],"rb").read()
	if len(input)%8 != 0:
		sys.stderr.write("%s: not a c5t file? expected size to be a multiple of 8, but length was %d" % (path, len(input)))
		sys.exit(1)
	if input[0:8] != HEADER:
		sys.stderr.write("%s: not a c5t file? expected %s\n" % (path, repr(HEADER)))
		sys.exit(1)
	tuples = []
	input = input[8:]
	for i in range(0,len(input),8):
		t = struct.unpack("BBBBBBBB", input[i:i+8])
		tuples.append({
			"c": (t[0]) + (t[1]<<8) + (t[2]<<16),
			"y": t[3],
			"r": t[4],
			"g": t[5],
			"b": t[6],
			"x": t[7],
		})
	return tuples

def squash(x):
	c0=0x40
	return round(c0 + (x*(0xff-c0))/0xff)

if cmd == "txt_to_c5t":
	if len(args)!=2: usage("expected <input> and <output> paths")
	input  = open(args[0],"r").read()
	output = open(args[1],"wb")
	output.write(HEADER)
	for c in input: output.write(pack_c5(ord(c)))
	output.close()
elif cmd == "c5t_to_html":
	if len(args)!=2: usage("expected <input> and <output> paths")
	input = readc5t(args[0])
	output = open(args[1],"w")
	output.write("<style>body{background-color:black;font-family:monospace;}</style>\n")
	for t in input:
		c=t["c"]
		if c==ord("\n"):
			output.write("<br>\n")
			continue
		elif c<32:
			continue
		r,g,b = [("%.2x" % squash(x)) for x in [t["r"], t["g"], t["b"]]]
		output.write("<span style=\"color:#%s%s%s\">%s</span>" % (r,g,b,chr(c)))
	output.close()
elif cmd == "c5t_to_txt":
	if len(args)!=2: usage("expected <input> and <output> paths")
	input = readc5t(args[0])
	output = open(args[1],"w")
	for t in input:
		output.write("%s" % chr(t["c"]))
	output.close()
else:
	usage("invalid command %s" % repr(cmd))
