#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

int main(int argc, char const** argv)
{
    if (argc != 3)
    {
        /*
         * e.g.
         * build file: /../builds/../Qt_5_11_3_Clang_7_Zap/Debug/build.ninja
         * clang bin:  /../profiling-llvm/llvm/build/bin/clang++
         */
        std::cout << "usage: ~ path/to/build.ninja path/to/profiling/clang++" << std::endl;

        return EXIT_SUCCESS;
    }

    auto build_file = std::string(argv[1]);
    auto clang = std::string(argv[2]);

    std::cout << "Config:" << std::endl;
    std::cout << "  build file: " << build_file << std::endl;
    std::cout << "  clang bin:  " << clang << std::endl;

    return EXIT_SUCCESS;
}
