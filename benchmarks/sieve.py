def main():
    n = 5000000
    c = [0] * (n + 1)
    cnt = 0
    for i in range(2, n + 1):
        if c[i] == 0:
            cnt += 1
            for j in range(i * i, n + 1, i): c[j] = 1
    print(cnt)
main()
