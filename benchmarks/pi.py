def main():
    s = 0.0; sg = 1.0
    for k in range(20000000):
        s += sg / (2 * k + 1); sg = -sg
    print("%.6f" % (s * 4))
main()
