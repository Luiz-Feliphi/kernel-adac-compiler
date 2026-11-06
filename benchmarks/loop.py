def main():
    soma = 0
    for i in range(30000000): soma += (i % 7) * (i & 3)
    print(soma)
main()
