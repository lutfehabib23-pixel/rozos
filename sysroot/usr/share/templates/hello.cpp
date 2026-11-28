// A C++ program for rozOS - Build > Compile and Run (F5) uses g++
// (needs: rozpkg install binutils gcc)
#include <iostream>
#include <string>
#include <vector>

int main()
{
    std::vector<std::string> words{"Hello", "from", "C++", "on", "rozOS"};
    for (const auto &w : words)
        std::cout << w << ' ';
    std::cout << '\n';
    return 0;
}
