import fcntl, os, struct, sys, termios, time
port, secs = sys.argv[1], float(sys.argv[2])
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
BIS, BIC = 0x8004746C, 0x8004746D
DTR, RTS = 0x002, 0x004
# esptool-style hard reset: RTS low pulse (EN), DTR released
fcntl.ioctl(fd, BIC, struct.pack("I", DTR))
fcntl.ioctl(fd, BIS, struct.pack("I", RTS))
time.sleep(0.15)
fcntl.ioctl(fd, BIC, struct.pack("I", RTS))
# assert DTR so the CH343 keeps streaming (the dongle lesson, again)
fcntl.ioctl(fd, BIS, struct.pack("I", DTR))
end = time.time() + secs
buf = b""
while time.time() < end:
    try:
        b = os.read(fd, 4096)
        if b: buf += b
    except BlockingIOError:
        time.sleep(0.03)
os.close(fd)
sys.stdout.buffer.write(buf)
