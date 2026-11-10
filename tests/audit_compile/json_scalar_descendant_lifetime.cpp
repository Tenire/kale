#include "kale/http/json.h"

int main() {
    kale::Json root = kale::Json::parse("{\"val\":42}");
    root = root["val"];
    return root.get<int>() != 42;
}
