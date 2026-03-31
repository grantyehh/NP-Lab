#include <iostream>
#include <fstream>
#include <string>

int main(int argc, char *argv[]) {
    // 如果沒參數 → 讀標準輸入
    if (argc == 1) {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << line << "\n";
        }
        return 0;
    }

    // 輸出每個檔案
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i]);
        if (!file.is_open()) {
            std::cerr << "cat: cannot open " << argv[i] << "\n";
            continue;
        }

        std::string line;
        while (std::getline(file, line)) {
            std::cout << line << "\n";
        }
    }

    return 0;
}