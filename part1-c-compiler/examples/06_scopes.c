// Scopes: shadowing gives each declaration its own IR name (x, x.1, x.2)
int main() {
    int x = 1;
    int total = 0;
    {
        int x = 10;               // shadows the outer x
        total = total + x;
        {
            int x = 100;          // shadows again
            total = total + x;
        }
    }
    total = total + x;            // the outer x is still 1
    print(total);                 // 111
    return x;
}
