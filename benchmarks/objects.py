class P:
    def __init__(self, v): self.v = v
def main():
    soma = 0
    for i in range(3000000):
        p = P(i); soma += p.v & 7
    print(soma)
main()
